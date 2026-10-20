/* gvs-server.c - HTTP front end for voicegram upload and playback
 *
 * Copyright (C) 2026 Ole Kristian Aamot <ole.aamot@aamotinnovation.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * A deliberately small HTTP/1.1 server: one request per connection
 * (Connection: close), Content-Length bodies only, single byte ranges.
 * That is all the gnome-voice client and GStreamer's souphttpsrc need.
 * Put it behind nginx/Apache/Caddy for TLS and rate limiting.
 *
 * See PROTOCOL.md for the wire protocol.
 */
#include "config.h"

#include <string.h>
#include <glib/gstdio.h>

#include "gvs-server.h"
#include "gvs-voicegram.h"

#define MAX_LINE        8192
#define MAX_HEADER_SIZE (32 * 1024)
#define MAX_HEADERS     64
#define COPY_CHUNK      (64 * 1024)
#define LIST_DEFAULT    100
#define LIST_MAX        1000

struct _GvsServer {
  GvsStore        *store;
  GvsServerConfig  config;
  GSocketService  *service;
  guint16          port;
};

typedef struct {
  GvsServer            *server;
  GSocketConnection    *conn;
  GBufferedInputStream *in;
  GOutputStream        *out;
  gchar                *peer;

  gchar                *method;
  gchar                *path;
  gchar                *query;
  GHashTable           *headers;      /* lower-cased name -> value */
  gint64                content_length;

  guint                 status;
  gint64                bytes_sent;
  gboolean              head_only;
} Request;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static const gchar *
reason_phrase (guint status)
{
  switch (status)
    {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

static const gchar *
header (Request *req, const gchar *name)
{
  return g_hash_table_lookup (req->headers, name);
}

static gboolean
str_equal_const_time (const gchar *a, const gchar *b)
{
  gsize la = strlen (a), lb = strlen (b), i;
  guchar diff = (la != lb);

  for (i = 0; i < la; i++)
    diff |= (guchar) a[i] ^ (guchar) b[i % (lb ? lb : 1)];
  return diff == 0;
}

/* Reads one CRLF/LF-terminated line, refusing lines over MAX_LINE. */
static gchar *
read_line (Request *req, gsize *total, GError **error)
{
  GString *line = g_string_sized_new (128);

  for (;;)
    {
      int c = g_buffered_input_stream_read_byte (req->in, NULL, error);

      if (c < 0)
        {
          if (error && *error == NULL)
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                                 "Connection closed");
          g_string_free (line, TRUE);
          return NULL;
        }
      (*total)++;
      if (c == '\n')
        break;
      if (line->len >= MAX_LINE || *total > MAX_HEADER_SIZE)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                               "Request header too large");
          g_string_free (line, TRUE);
          return NULL;
        }
      g_string_append_c (line, (gchar) c);
    }

  if (line->len > 0 && line->str[line->len - 1] == '\r')
    g_string_truncate (line, line->len - 1);
  return g_string_free (line, FALSE);
}

/* ------------------------------------------------------------------ */
/* Responses                                                           */
/* ------------------------------------------------------------------ */

static gboolean
write_all (Request *req, const void *data, gsize len)
{
  gsize written = 0;
  gboolean ok = g_output_stream_write_all (req->out, data, len, &written, NULL, NULL);

  req->bytes_sent += written;
  return ok;
}

static gboolean
send_head (Request     *req,
           guint        status,
           const gchar *content_type,
           gint64       content_length,
           const gchar *extra)
{
  g_autoptr(GDateTime) now = g_date_time_new_now_utc ();
  g_autofree gchar *date = g_date_time_format (now, "%a, %d %b %Y %H:%M:%S GMT");
  GString *h = g_string_new (NULL);
  gboolean ok;

  req->status = status;
  g_string_append_printf (h, "HTTP/1.1 %u %s\r\n", status, reason_phrase (status));
  g_string_append_printf (h, "Date: %s\r\n", date);
  g_string_append (h, "Server: " PACKAGE_NAME "/" PACKAGE_VERSION "\r\n");
  g_string_append (h, "Connection: close\r\n");
  g_string_append (h, "Access-Control-Allow-Origin: *\r\n");
  g_string_append (h, "X-Content-Type-Options: nosniff\r\n");
  if (content_type)
    g_string_append_printf (h, "Content-Type: %s\r\n", content_type);
  if (content_length >= 0)
    g_string_append_printf (h, "Content-Length: %" G_GINT64_FORMAT "\r\n", content_length);
  if (extra)
    g_string_append (h, extra);
  g_string_append (h, "\r\n");

  ok = write_all (req, h->str, h->len);
  g_string_free (h, TRUE);
  return ok;
}

static void
send_response (Request     *req,
               guint        status,
               const gchar *content_type,
               const gchar *body,
               gssize       len,
               const gchar *extra)
{
  if (body && len < 0)
    len = strlen (body);
  if (!body)
    len = 0;

  if (!send_head (req, status, body ? content_type : NULL, len, extra))
    return;
  if (body && len > 0 && !req->head_only)
    write_all (req, body, len);
}

static void
send_error (Request *req, guint status, const gchar *fmt, ...) G_GNUC_PRINTF (3, 4);

static void
send_error (Request *req, guint status, const gchar *fmt, ...)
{
  g_autofree gchar *msg = NULL;
  g_autofree gchar *body = NULL;
  const gchar *extra = NULL;
  va_list ap;

  va_start (ap, fmt);
  msg = g_strdup_vprintf (fmt, ap);
  va_end (ap);

  body = g_strdup_printf ("%u %s: %s\n", status, reason_phrase (status), msg);
  if (status == 401)
    extra = "WWW-Authenticate: Bearer realm=\"" PACKAGE_NAME "\"\r\n";
  else if (status == 405)
    extra = "Allow: GET, HEAD, POST, PUT, DELETE, OPTIONS\r\n";
  send_response (req, status, "text/plain; charset=utf-8", body, -1, extra);
}

/* ------------------------------------------------------------------ */
/* Request parsing                                                     */
/* ------------------------------------------------------------------ */

static gboolean
read_request (Request *req)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *line = NULL;
  g_auto(GStrv) parts = NULL;
  gsize total = 0;
  guint n_headers = 0;
  gchar *q;
  const gchar *cl;

  /* Tolerate leading blank lines (RFC 9112 §2.2). */
  do
    {
      g_clear_pointer (&line, g_free);
      line = read_line (req, &total, &error);
      if (line == NULL)
        {
          if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE))
            send_error (req, 431, "%s", error->message);
          else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
            send_error (req, 408, "Timed out waiting for request");
          return FALSE;
        }
    }
  while (*line == '\0' && total < 16);

  parts = g_strsplit (line, " ", 4);
  if (g_strv_length (parts) != 3 || *parts[0] == '\0' || parts[1][0] != '/')
    {
      send_error (req, 400, "Malformed request line");
      return FALSE;
    }
  if (!g_str_equal (parts[2], "HTTP/1.1") && !g_str_equal (parts[2], "HTTP/1.0"))
    {
      send_error (req, 505, "Only HTTP/1.0 and HTTP/1.1 are supported");
      return FALSE;
    }

  req->method = g_strdup (parts[0]);
  q = strchr (parts[1], '?');
  if (q)
    {
      req->query = g_strdup (q + 1);
      *q = '\0';
    }
  req->path = g_strdup (parts[1]);

  for (;;)
    {
      g_autofree gchar *hl = read_line (req, &total, &error);
      gchar *colon;
      gchar *name;
      gchar *value;

      if (hl == NULL)
        {
          if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE))
            send_error (req, 431, "%s", error->message);
          return FALSE;
        }
      if (*hl == '\0')
        break;
      if (++n_headers > MAX_HEADERS)
        {
          send_error (req, 431, "Too many header fields");
          return FALSE;
        }
      colon = strchr (hl, ':');
      if (colon == NULL || colon == hl || g_ascii_isspace (hl[0]))
        {
          send_error (req, 400, "Malformed header field");
          return FALSE;
        }
      name = g_ascii_strdown (hl, colon - hl);
      value = g_strstrip (g_strdup (colon + 1));
      if (strchr (name, ' ') || strchr (name, '\t'))
        {
          g_free (name);
          g_free (value);
          send_error (req, 400, "Malformed header field name");
          return FALSE;
        }
      /* Duplicate Content-Length is a request-smuggling vector. */
      if (g_str_equal (name, "content-length") && g_hash_table_contains (req->headers, name))
        {
          g_free (name);
          g_free (value);
          send_error (req, 400, "Duplicate Content-Length");
          return FALSE;
        }
      g_hash_table_replace (req->headers, name, value);
    }

  if (header (req, "transfer-encoding"))
    {
      send_error (req, 411, "Chunked uploads are not supported; send Content-Length");
      return FALSE;
    }

  req->content_length = -1;
  cl = header (req, "content-length");
  if (cl)
    {
      guint64 v;

      if (!g_ascii_string_to_unsigned (cl, 10, 0, G_MAXINT64, &v, NULL))
        {
          send_error (req, 400, "Invalid Content-Length");
          return FALSE;
        }
      req->content_length = (gint64) v;
    }

  req->head_only = g_str_equal (req->method, "HEAD");
  return TRUE;
}

static gboolean
check_auth (Request *req)
{
  const gchar *token = req->server->config.token;
  const gchar *auth;

  if (req->server->config.read_only)
    {
      send_error (req, 403, "This server is read-only");
      return FALSE;
    }
  if (token == NULL || *token == '\0')
    return TRUE;

  auth = header (req, "authorization");
  if (auth && g_ascii_strncasecmp (auth, "Bearer ", 7) == 0 &&
      str_equal_const_time (auth + 7, token))
    return TRUE;

  send_error (req, 401, "A valid bearer token is required");
  return FALSE;
}

static void
maybe_send_continue (Request *req)
{
  const gchar *expect = header (req, "expect");

  if (expect && g_ascii_strcasecmp (expect, "100-continue") == 0)
    write_all (req, "HTTP/1.1 100 Continue\r\n\r\n", 25);
}

/* Header values may be percent-encoded so that non-ASCII names
 * (Ålesund, Bjørn) survive HTTP; raw UTF-8 is accepted as well. */
static gchar *
header_text (Request *req, const gchar *name, gsize max)
{
  const gchar *raw = header (req, name);
  g_autofree gchar *dec = NULL;

  if (raw == NULL || *raw == '\0')
    return NULL;
  dec = g_uri_unescape_string (raw, NULL);
  if (dec == NULL || !g_utf8_validate (dec, -1, NULL))
    return NULL;
  if (strlen (dec) > max)
    return NULL;
  /* No control characters in metadata. */
  for (const gchar *p = dec; *p; p++)
    if ((guchar) *p < 0x20)
      return NULL;
  return g_steal_pointer (&dec);
}

/* Base URL used for <stream> links. */
static gchar *
base_url (Request *req)
{
  const gchar *host;

  if (req->server->config.public_url)
    return g_strdup (req->server->config.public_url);

  host = header (req, "host");
  if (host && *host && strlen (host) < 256)
    {
      const gchar *p;
      for (p = host; *p; p++)
        if (!g_ascii_isalnum (*p) && !strchr (".-:[]_", *p))
          break;
      if (*p == '\0')
        return g_strdup_printf ("http://%s", host);
    }
  return g_strdup_printf ("http://%s:%u", g_get_host_name (), req->server->port);
}

static gchar *
stream_url (Request *req, const gchar *id)
{
  g_autofree gchar *base = base_url (req);
  return g_strdup_printf ("%s/voicegrams/%s.ogg", base, id);
}

static GvsVoicegram *
load_voicegram (Request *req, const gchar *id)
{
  g_autofree gchar *path = gvs_store_path (req->server->store, id, "voice");
  GvsVoicegram *vg = gvs_voicegram_load (path, NULL);

  if (vg == NULL)
    vg = gvs_voicegram_new ();
  g_free (vg->id);
  vg->id = g_strdup (id);
  g_free (vg->stream);
  vg->stream = stream_url (req, id);
  return vg;
}

/* ------------------------------------------------------------------ */
/* Handlers                                                            */
/* ------------------------------------------------------------------ */

static void
handle_index (Request *req)
{
  static const gchar body[] =
    PACKAGE_NAME " " PACKAGE_VERSION "\n"
    "Voicegram server for GNOME Voice <" PACKAGE_URL ">\n"
    "\n"
    "GET    /voicegrams               list voicegrams (Voicegram XML)\n"
    "POST   /voicegrams               upload Ogg Vorbis audio\n"
    "PUT    /voicegrams/ID.voice      attach .voice metadata\n"
    "GET    /voicegrams/ID.ogg        play audio\n"
    "GET    /voicegrams/ID.voice      metadata\n"
    "DELETE /voicegrams/ID            remove (token required)\n";

  send_response (req, 200, "text/plain; charset=utf-8", body, -1, NULL);
}

static void
handle_list (Request *req)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) ids = gvs_store_list (req->server->store, &error);
  GString *xml;
  guint limit = LIST_DEFAULT, i;

  if (ids == NULL)
    {
      send_error (req, 500, "Cannot list voicegrams");
      g_warning ("list: %s", error->message);
      return;
    }

  if (req->query)
    {
      g_autoptr(GHashTable) params = g_uri_parse_params (req->query, -1, "&",
                                                         G_URI_PARAMS_NONE, NULL);
      const gchar *l = params ? g_hash_table_lookup (params, "limit") : NULL;
      guint64 v;

      if (l && g_ascii_string_to_unsigned (l, 10, 1, LIST_MAX, &v, NULL))
        limit = (guint) v;
    }

  xml = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  g_string_append (xml, "<voice version=\"" PACKAGE_VERSION "\">\n");
  for (i = 0; i < ids->len && i < limit; i++)
    {
      g_autoptr(GvsVoicegram) vg = load_voicegram (req, ids->pdata[i]);
      g_autofree gchar *station = gvs_voicegram_station_xml (vg, 2);
      g_string_append (xml, station);
    }
  g_string_append (xml, "</voice>\n");

  send_response (req, 200, "application/xml; charset=utf-8", xml->str, xml->len,
                 "Cache-Control: no-cache\r\n");
  g_string_free (xml, TRUE);
}

static void
handle_get_voice (Request *req, const gchar *id)
{
  g_autoptr(GvsVoicegram) vg = NULL;
  g_autofree gchar *xml = NULL;

  if (!gvs_store_exists (req->server->store, id, "ogg"))
    {
      send_error (req, 404, "No voicegram %s", id);
      return;
    }
  vg = load_voicegram (req, id);
  xml = gvs_voicegram_to_xml (vg);
  send_response (req, 200, "application/xml; charset=utf-8", xml, -1, NULL);
}

/* Parses "bytes=a-b", "bytes=a-" or "bytes=-n". Returns FALSE if the
 * header is absent or not a single range we understand (→ send 200). */
static gboolean
parse_range (const gchar *value, gint64 size, gint64 *start, gint64 *end,
             gboolean *unsatisfiable)
{
  const gchar *spec;
  const gchar *dash;
  guint64 a = 0, b = 0;

  *unsatisfiable = FALSE;
  if (value == NULL || !g_str_has_prefix (value, "bytes="))
    return FALSE;
  spec = value + 6;
  if (strchr (spec, ','))
    return FALSE;
  dash = strchr (spec, '-');
  if (dash == NULL)
    return FALSE;

  if (dash == spec)
    {
      /* suffix range */
      if (!g_ascii_string_to_unsigned (dash + 1, 10, 1, G_MAXINT64, &b, NULL))
        return FALSE;
      if (size == 0)
        {
          *unsatisfiable = TRUE;
          return TRUE;
        }
      *start = (gint64) b >= size ? 0 : size - (gint64) b;
      *end = size - 1;
      return TRUE;
    }

  {
    g_autofree gchar *first = g_strndup (spec, dash - spec);
    if (!g_ascii_string_to_unsigned (first, 10, 0, G_MAXINT64, &a, NULL))
      return FALSE;
  }
  if (dash[1] == '\0')
    b = size > 0 ? (guint64) size - 1 : 0;
  else if (!g_ascii_string_to_unsigned (dash + 1, 10, 0, G_MAXINT64, &b, NULL) || b < a)
    return FALSE;

  if ((gint64) a >= size)
    {
      *unsatisfiable = TRUE;
      return TRUE;
    }
  *start = (gint64) a;
  *end = MIN ((gint64) b, size - 1);
  return TRUE;
}

static void
handle_get_audio (Request *req, const gchar *id)
{
  g_autofree gchar *path = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GFileInputStream) fin = NULL;
  g_autoptr(GFileInfo) info = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *extra = NULL;
  gint64 size, start = 0, end, remaining;
  gboolean unsat = FALSE, ranged;
  guint8 *buf;

  if (!gvs_store_exists (req->server->store, id, "ogg"))
    {
      send_error (req, 404, "No voicegram %s", id);
      return;
    }

  path = gvs_store_path (req->server->store, id, "ogg");
  file = g_file_new_for_path (path);
  fin = g_file_read (file, NULL, &error);
  if (fin)
    info = g_file_input_stream_query_info (fin, G_FILE_ATTRIBUTE_STANDARD_SIZE, NULL, &error);
  if (info == NULL)
    {
      /* Deleted between exists() and read(). */
      send_error (req, 404, "No voicegram %s", id);
      return;
    }

  size = g_file_info_get_size (info);
  end = size - 1;
  ranged = parse_range (header (req, "range"), size, &start, &end, &unsat);

  if (unsat)
    {
      extra = g_strdup_printf ("Content-Range: bytes */%" G_GINT64_FORMAT "\r\n", size);
      req->status = 416;
      send_response (req, 416, NULL, NULL, 0, extra);
      return;
    }

  remaining = size > 0 ? end - start + 1 : 0;
  if (ranged)
    extra = g_strdup_printf ("Accept-Ranges: bytes\r\n"
                             "Content-Range: bytes %" G_GINT64_FORMAT "-%" G_GINT64_FORMAT
                             "/%" G_GINT64_FORMAT "\r\n"
                             "Content-Disposition: inline; filename=\"%s.ogg\"\r\n",
                             start, end, size, id);
  else
    extra = g_strdup_printf ("Accept-Ranges: bytes\r\n"
                             "Content-Disposition: inline; filename=\"%s.ogg\"\r\n", id);

  if (!send_head (req, ranged ? 206 : 200, "audio/ogg", remaining, extra) || req->head_only)
    return;

  if (start > 0 &&
      !g_seekable_seek (G_SEEKABLE (fin), start, G_SEEK_SET, NULL, &error))
    return;

  buf = g_malloc (COPY_CHUNK);
  while (remaining > 0)
    {
      gssize n = g_input_stream_read (G_INPUT_STREAM (fin), buf,
                                      MIN (remaining, COPY_CHUNK), NULL, NULL);
      if (n <= 0 || !write_all (req, buf, n))
        break;
      remaining -= n;
    }
  g_free (buf);
}

static gboolean
is_ogg_content_type (const gchar *ct)
{
  g_autofree gchar *base = NULL;
  const gchar *semi;

  if (ct == NULL)
    return TRUE;
  semi = strchr (ct, ';');
  base = g_strstrip (g_ascii_strdown (ct, semi ? semi - ct : -1));
  return g_str_equal (base, "audio/ogg") ||
         g_str_equal (base, "application/ogg") ||
         g_str_equal (base, "audio/vorbis") ||
         g_str_equal (base, "audio/x-vorbis+ogg") ||
         g_str_equal (base, "application/octet-stream");
}

static void
handle_upload_audio (Request *req)
{
  GvsServer *server = req->server;
  g_autoptr(GError) error = NULL;
  g_autoptr(GOutputStream) tmp = NULL;
  g_autoptr(GvsVoicegram) vg = NULL;
  g_autofree gchar *tmp_path = NULL;
  g_autofree gchar *id = NULL;
  g_autofree gchar *xml = NULL;
  g_autofree gchar *extra = NULL;
  g_autofree gchar *lat = NULL;
  g_autofree gchar *lon = NULL;
  g_autoptr(GDateTime) now = NULL;
  gint64 remaining;
  gboolean ok = TRUE, checked_magic = FALSE, disk_error = FALSE;
  guint8 *buf;

  if (!check_auth (req))
    return;
  if (req->content_length < 0)
    {
      send_error (req, 411, "Content-Length is required");
      return;
    }
  if (req->content_length == 0)
    {
      send_error (req, 400, "Empty upload");
      return;
    }
  if (req->content_length > server->config.max_audio_size)
    {
      send_error (req, 413, "Voicegram exceeds %" G_GINT64_FORMAT " bytes",
                  server->config.max_audio_size);
      return;
    }
  if (!is_ogg_content_type (header (req, "content-type")))
    {
      send_error (req, 415, "Voicegrams must be Ogg Vorbis (audio/ogg)");
      return;
    }

  tmp = gvs_store_create_tmp (server->store, &tmp_path, &error);
  if (tmp == NULL)
    {
      g_warning ("upload: %s", error->message);
      send_error (req, 500, "Cannot store voicegram");
      return;
    }

  maybe_send_continue (req);

  buf = g_malloc (COPY_CHUNK);
  remaining = req->content_length;
  while (remaining > 0)
    {
      gssize n = g_input_stream_read (G_INPUT_STREAM (req->in), buf,
                                      MIN (remaining, COPY_CHUNK), NULL, &error);
      if (n <= 0)
        {
          ok = FALSE;
          break;
        }
      if (!checked_magic)
        {
          /* Every Ogg page starts with the capture pattern "OggS". The
           * first read may be short; insist on at least 4 bytes. */
          if (n < 4 && remaining > n)
            {
              /* extremely unlikely; read the rest of the magic */
              gsize got = 0;
              if (!g_input_stream_read_all (G_INPUT_STREAM (req->in), buf + n,
                                            MIN (4 - n, remaining - n), &got, NULL, NULL))
                {
                  ok = FALSE;
                  break;
                }
              n += got;
            }
          if (n < 4 || memcmp (buf, "OggS", 4) != 0)
            {
              g_free (buf);
              g_output_stream_close (tmp, NULL, NULL);
              g_unlink (tmp_path);
              send_error (req, 415, "Upload is not an Ogg stream");
              return;
            }
          checked_magic = TRUE;
        }
      if (!g_output_stream_write_all (tmp, buf, n, NULL, NULL, &error))
        {
          disk_error = TRUE;
          ok = FALSE;
          break;
        }
      remaining -= n;
    }
  g_free (buf);

  if (!g_output_stream_close (tmp, NULL, ok ? &error : NULL) && ok)
    {
      disk_error = TRUE;
      ok = FALSE;
    }

  if (!ok)
    {
      g_unlink (tmp_path);
      if (disk_error)
        {
          g_warning ("upload: %s", error ? error->message : "write failed");
          send_error (req, 500, "Cannot store voicegram");
        }
      else
        send_error (req, 400, "Upload ended early (%" G_GINT64_FORMAT " bytes missing)",
                    remaining);
      return;
    }

  /* Metadata from the X-Voicegram-* headers. */
  now = g_date_time_new_now_utc ();
  id = gvs_store_new_id ();
  vg = gvs_voicegram_new ();
  vg->id = g_strdup (id);
  vg->date = g_date_time_format_iso8601 (now);
  vg->version = header_text (req, "x-voicegram-version", 32);
  vg->name = header_text (req, "x-voicegram-name", 256);
  vg->uri = header_text (req, "x-voicegram-uri", 1024);
  vg->location = header_text (req, "x-voicegram-location", 256);
  lat = header_text (req, "x-voicegram-lat", 32);
  lon = header_text (req, "x-voicegram-lon", 32);
  if (lat && lon)
    gvs_voicegram_set_coords (vg, lat, lon);
  vg->stream = stream_url (req, id);

  xml = gvs_voicegram_to_xml (vg);
  if (!gvs_store_write (server->store, id, "voice", xml, strlen (xml), &error) ||
      !gvs_store_commit (server->store, tmp_path, id, "ogg", &error))
    {
      g_warning ("upload %s: %s", id, error->message);
      g_unlink (tmp_path);
      gvs_store_delete (server->store, id, NULL);
      send_error (req, 500, "Cannot store voicegram");
      return;
    }

  g_message ("stored voicegram %s (%" G_GINT64_FORMAT " bytes) from %s",
             id, req->content_length, req->peer);

  extra = g_strdup_printf ("Location: /voicegrams/%s.voice\r\nX-Voicegram-Id: %s\r\n", id, id);
  send_response (req, 201, "application/xml; charset=utf-8", xml, -1, extra);
}

static void
handle_put_voice (Request *req, const gchar *id)
{
  GvsServer *server = req->server;
  g_autoptr(GError) error = NULL;
  g_autoptr(GvsVoicegram) vg = NULL;
  g_autoptr(GvsVoicegram) old = NULL;
  g_autofree gchar *body = NULL;
  g_autofree gchar *path = NULL;
  g_autofree gchar *xml = NULL;
  gsize got = 0;

  if (!check_auth (req))
    return;
  if (!gvs_store_exists (server->store, id, "ogg"))
    {
      send_error (req, 404, "No voicegram %s; POST the audio first", id);
      return;
    }
  if (req->content_length < 0)
    {
      send_error (req, 411, "Content-Length is required");
      return;
    }
  if ((guint64) req->content_length > server->config.max_meta_size)
    {
      send_error (req, 413, "Metadata exceeds %" G_GSIZE_FORMAT " bytes",
                  server->config.max_meta_size);
      return;
    }

  maybe_send_continue (req);

  body = g_malloc (req->content_length + 1);
  if (!g_input_stream_read_all (G_INPUT_STREAM (req->in), body, req->content_length,
                                &got, NULL, &error) ||
      got != (gsize) req->content_length)
    {
      send_error (req, 400, "Upload ended early");
      return;
    }
  body[got] = '\0';

  vg = gvs_voicegram_parse (body, got, &error);
  if (vg == NULL)
    {
      send_error (req, 400, "Invalid voicegram metadata: %s", error->message);
      return;
    }

  /* The server owns id, date and stream; the client owns the rest. */
  path = gvs_store_path (server->store, id, "voice");
  old = gvs_voicegram_load (path, NULL);
  g_free (vg->id);
  vg->id = g_strdup (id);
  g_free (vg->date);
  vg->date = g_strdup (old && old->date ? old->date : NULL);
  if (vg->date == NULL)
    {
      g_autoptr(GDateTime) now = g_date_time_new_now_utc ();
      vg->date = g_date_time_format_iso8601 (now);
    }
  g_free (vg->stream);
  vg->stream = stream_url (req, id);

  xml = gvs_voicegram_to_xml (vg);
  if (!gvs_store_write (server->store, id, "voice", xml, strlen (xml), &error))
    {
      g_warning ("metadata %s: %s", id, error->message);
      send_error (req, 500, "Cannot store metadata");
      return;
    }

  g_message ("updated metadata for voicegram %s from %s", id, req->peer);
  send_response (req, 200, "application/xml; charset=utf-8", xml, -1, NULL);
}

static void
handle_delete (Request *req, const gchar *id)
{
  g_autoptr(GError) error = NULL;

  /* Deletion always requires the token, even on open-upload servers. */
  if (req->server->config.token == NULL || *req->server->config.token == '\0')
    {
      send_error (req, 403, "Deletion is disabled (no token configured)");
      return;
    }
  if (!check_auth (req))
    return;

  if (!gvs_store_delete (req->server->store, id, &error))
    {
      send_error (req, 404, "No voicegram %s", id);
      return;
    }
  g_message ("deleted voicegram %s by %s", id, req->peer);
  send_response (req, 204, NULL, NULL, 0, NULL);
}

static void
route (Request *req)
{
  const gchar *m = req->method;
  gboolean get = g_str_equal (m, "GET") || g_str_equal (m, "HEAD");
  const gchar *rest;
  g_autofree gchar *id = NULL;
  const gchar *ext = NULL;
  gchar *dot;

  if (g_str_equal (m, "OPTIONS"))
    {
      send_response (req, 204, NULL, NULL, 0,
                     "Allow: GET, HEAD, POST, PUT, DELETE, OPTIONS\r\n"
                     "Access-Control-Allow-Methods: GET, HEAD, POST, PUT, DELETE, OPTIONS\r\n"
                     "Access-Control-Allow-Headers: Authorization, Content-Type, Range, "
                     "X-Voicegram-Name, X-Voicegram-Uri, X-Voicegram-Location, "
                     "X-Voicegram-Lat, X-Voicegram-Lon, X-Voicegram-Version\r\n"
                     "Access-Control-Max-Age: 86400\r\n");
      return;
    }

  if (g_str_equal (req->path, "/"))
    {
      if (get)
        handle_index (req);
      else
        send_error (req, 405, "Method not allowed");
      return;
    }

  if (g_str_equal (req->path, "/health"))
    {
      if (get)
        send_response (req, 200, "text/plain", "ok\n", 3, "Cache-Control: no-cache\r\n");
      else
        send_error (req, 405, "Method not allowed");
      return;
    }

  if (g_str_equal (req->path, "/voicegrams") || g_str_equal (req->path, "/voicegrams/"))
    {
      if (get)
        handle_list (req);
      else if (g_str_equal (m, "POST"))
        handle_upload_audio (req);
      else
        send_error (req, 405, "Method not allowed");
      return;
    }

  if (!g_str_has_prefix (req->path, "/voicegrams/"))
    {
      send_error (req, 404, "Not found");
      return;
    }

  rest = req->path + strlen ("/voicegrams/");
  id = g_strdup (rest);
  dot = strrchr (id, '.');
  if (dot)
    {
      *dot = '\0';
      ext = dot + 1;
    }
  if (!gvs_store_id_is_valid (id) ||
      (ext && !g_str_equal (ext, "ogg") && !g_str_equal (ext, "voice")))
    {
      send_error (req, 404, "Not found");
      return;
    }

  if (get)
    {
      if (g_strcmp0 (ext, "ogg") == 0)
        handle_get_audio (req, id);
      else
        handle_get_voice (req, id);
    }
  else if (g_str_equal (m, "PUT") && g_strcmp0 (ext, "voice") == 0)
    handle_put_voice (req, id);
  else if (g_str_equal (m, "DELETE") && ext == NULL)
    handle_delete (req, id);
  else if (g_str_equal (m, "GET") || g_str_equal (m, "PUT") ||
           g_str_equal (m, "POST") || g_str_equal (m, "DELETE"))
    send_error (req, 405, "Method not allowed");
  else
    send_error (req, 501, "Method not implemented");
}

/* ------------------------------------------------------------------ */
/* Connection handling                                                 */
/* ------------------------------------------------------------------ */

static gchar *
describe_peer (GSocketConnection *conn)
{
  g_autoptr(GSocketAddress) addr = g_socket_connection_get_remote_address (conn, NULL);

  if (G_IS_INET_SOCKET_ADDRESS (addr))
    {
      GInetAddress *ia = g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (addr));
      return g_inet_address_to_string (ia);
    }
  return g_strdup ("unknown");
}

static gboolean
on_run (GThreadedSocketService *service,
        GSocketConnection      *conn,
        GObject                *source,
        gpointer                user_data)
{
  GvsServer *server = user_data;
  Request req = { 0 };

  g_socket_set_timeout (g_socket_connection_get_socket (conn), server->config.timeout);

  req.server = server;
  req.conn = conn;
  req.peer = describe_peer (conn);
  req.in = G_BUFFERED_INPUT_STREAM (
      g_buffered_input_stream_new_sized (g_io_stream_get_input_stream (G_IO_STREAM (conn)),
                                         16 * 1024));
  g_filter_input_stream_set_close_base_stream (G_FILTER_INPUT_STREAM (req.in), FALSE);
  req.out = g_io_stream_get_output_stream (G_IO_STREAM (conn));
  req.headers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  if (read_request (&req))
    route (&req);

  if (req.method)
    g_message ("%s \"%s %s%s%s\" %u %" G_GINT64_FORMAT,
               req.peer, req.method, req.path,
               req.query ? "?" : "", req.query ? req.query : "",
               req.status, req.bytes_sent);

  g_output_stream_flush (req.out, NULL, NULL);
  g_io_stream_close (G_IO_STREAM (conn), NULL, NULL);

  g_object_unref (req.in);
  g_hash_table_unref (req.headers);
  g_free (req.peer);
  g_free (req.method);
  g_free (req.path);
  g_free (req.query);
  return TRUE;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

GvsServer *
gvs_server_new (GvsStore               *store,
                const GvsServerConfig  *config,
                GError                **error)
{
  g_autoptr(GvsServer) server = g_new0 (GvsServer, 1);
  g_autoptr(GSocketAddress) addr = NULL;
  g_autoptr(GSocketAddress) effective = NULL;
  g_autoptr(GInetAddress) inet = NULL;

  server->store = store;
  server->config = *config;
  server->config.address = g_strdup (config->address);
  server->config.public_url = g_strdup (config->public_url);
  server->config.token = g_strdup (config->token);
  if (server->config.public_url)
    {
      /* strip trailing slashes */
      gsize n = strlen (server->config.public_url);
      while (n > 0 && server->config.public_url[n - 1] == '/')
        server->config.public_url[--n] = '\0';
    }

  server->service = g_threaded_socket_service_new (MAX (1, (gint) config->max_clients));

  if (config->address && *config->address)
    {
      inet = g_inet_address_new_from_string (config->address);
      if (inet == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       "Invalid bind address: %s", config->address);
          return NULL;
        }
      addr = g_inet_socket_address_new (inet, config->port);
      if (!g_socket_listener_add_address (G_SOCKET_LISTENER (server->service), addr,
                                          G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP,
                                          NULL, &effective, error))
        return NULL;
      server->port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (effective));
    }
  else if (config->port == 0)
    {
      server->port = g_socket_listener_add_any_inet_port (G_SOCKET_LISTENER (server->service),
                                                          NULL, error);
      if (server->port == 0)
        return NULL;
    }
  else
    {
      if (!g_socket_listener_add_inet_port (G_SOCKET_LISTENER (server->service),
                                            config->port, NULL, error))
        return NULL;
      server->port = config->port;
    }

  g_signal_connect (server->service, "run", G_CALLBACK (on_run), server);
  return g_steal_pointer (&server);
}

void
gvs_server_free (GvsServer *server)
{
  if (server == NULL)
    return;
  if (server->service)
    {
      g_socket_service_stop (server->service);
      g_socket_listener_close (G_SOCKET_LISTENER (server->service));
      g_object_unref (server->service);
    }
  g_free (server->config.address);
  g_free (server->config.public_url);
  g_free (server->config.token);
  g_free (server);
}

guint16
gvs_server_get_port (GvsServer *server)
{
  return server->port;
}

void
gvs_server_start (GvsServer *server)
{
  g_socket_service_start (server->service);
}

void
gvs_server_stop (GvsServer *server)
{
  g_socket_service_stop (server->service);
}
