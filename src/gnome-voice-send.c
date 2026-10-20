/* gnome-voice-send.c - upload a voicegram to a gnome-voice-server
 *
 * Copyright (C) 2026 Ole Kristian Aamot <ole.aamot@aamotinnovation.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * By default this sends the recording gnome-voice leaves behind:
 *   $XDG_MUSIC_DIR/GNOME.ogg     (Ogg Vorbis audio)
 *   $XDG_MUSIC_DIR/GNOME.voice   (Voicegram XML metadata)
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

#include <gio/gio.h>

#include "gvs-voicegram.h"

#define DEFAULT_SERVER "http://localhost:8085"

static gchar   *opt_server;
static gchar   *opt_token_file;
static gchar   *opt_ogg;
static gchar   *opt_voice;
static gchar   *opt_name;
static gchar   *opt_location;
static gchar   *opt_lat;
static gchar   *opt_lon;
static gboolean opt_list;
static gchar   *opt_delete;
static gboolean opt_quiet;

static const GOptionEntry entries[] = {
  { "server",     's', 0, G_OPTION_ARG_STRING,   &opt_server,
    "Server URL (default $GVS_SERVER_URL or " DEFAULT_SERVER ")", "URL" },
  { "token-file", 't', 0, G_OPTION_ARG_FILENAME, &opt_token_file,
    "File holding the upload token (default $GVS_TOKEN)", "FILE" },
  { "ogg",        'o', 0, G_OPTION_ARG_FILENAME, &opt_ogg,
    "Ogg Vorbis recording (default ~/Music/GNOME.ogg)", "FILE" },
  { "voice",      'v', 0, G_OPTION_ARG_FILENAME, &opt_voice,
    "Voicegram XML metadata (default: next to the .ogg)", "FILE" },
  { "name",       'n', 0, G_OPTION_ARG_STRING,   &opt_name,
    "Speaker / station name", "NAME" },
  { "location",   'l', 0, G_OPTION_ARG_STRING,   &opt_location,
    "Place name, e.g. Oslo", "PLACE" },
  { "lat",        0,   0, G_OPTION_ARG_STRING,   &opt_lat,
    "Latitude", "DEG" },
  { "lon",        0,   0, G_OPTION_ARG_STRING,   &opt_lon,
    "Longitude", "DEG" },
  { "list",       'L', 0, G_OPTION_ARG_NONE,     &opt_list,
    "Print the server's voicegram list instead of uploading", NULL },
  { "delete",     'D', 0, G_OPTION_ARG_STRING,   &opt_delete,
    "Delete voicegram ID (token required)", "ID" },
  { "quiet",      'q', 0, G_OPTION_ARG_NONE,     &opt_quiet,
    "Only print the voicegram id", NULL },
  { NULL }
};

typedef struct {
  guint       status;
  GHashTable *headers;
  GBytes     *body;
} Response;

static void
response_clear (Response *r)
{
  g_clear_pointer (&r->headers, g_hash_table_unref);
  g_clear_pointer (&r->body, g_bytes_unref);
}

static gchar *
read_line (GDataInputStream *in, GError **error)
{
  gsize len;
  gchar *line = g_data_input_stream_read_line (in, &len, NULL, error);

  if (line && len > 0 && line[len - 1] == '\r')
    line[len - 1] = '\0';
  return line;
}

/* One HTTP request on a fresh connection. @body_file streams a file,
 * @body sends memory; at most one may be set. */
static gboolean
http_request (GUri         *base,
              const gchar  *method,
              const gchar  *path,
              GPtrArray    *extra_headers,
              const gchar  *content_type,
              GFile        *body_file,
              GBytes       *body,
              Response     *resp,
              GError      **error)
{
  g_autoptr(GSocketClient) client = g_socket_client_new ();
  g_autoptr(GSocketConnection) conn = NULL;
  g_autoptr(GDataInputStream) in = NULL;
  g_autoptr(GString) req = g_string_new (NULL);
  g_autofree gchar *status_line = NULL;
  g_auto(GStrv) parts = NULL;
  GOutputStream *out;
  const gchar *host = g_uri_get_host (base);
  const gchar *base_path = g_uri_get_path (base);
  gboolean tls = g_strcmp0 (g_uri_get_scheme (base), "https") == 0;
  gint port = g_uri_get_port (base);
  gint64 length = 0;
  const gchar *cl;
  guint i;

  if (port < 0)
    port = tls ? 443 : 80;
  g_socket_client_set_tls (client, tls);
  g_socket_client_set_timeout (client, 120);

  conn = g_socket_client_connect_to_host (client, host, port, NULL, error);
  if (conn == NULL)
    return FALSE;

  if (body_file)
    {
      g_autoptr(GFileInfo) info =
          g_file_query_info (body_file, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                             G_FILE_QUERY_INFO_NONE, NULL, error);
      if (info == NULL)
        return FALSE;
      length = g_file_info_get_size (info);
    }
  else if (body)
    length = g_bytes_get_size (body);

  {
    g_autofree gchar *bp = g_strdup (base_path ? base_path : "");
    gsize n = strlen (bp);
    while (n > 0 && bp[n - 1] == '/')
      bp[--n] = '\0';
    g_string_append_printf (req, "%s %s%s HTTP/1.1\r\n", method, bp, path);
  }
  if ((tls && port == 443) || (!tls && port == 80))
    g_string_append_printf (req, "Host: %s\r\n", host);
  else
    g_string_append_printf (req, "Host: %s:%d\r\n", host, port);
  g_string_append (req, "User-Agent: gnome-voice-send/" PACKAGE_VERSION "\r\n");
  g_string_append (req, "Connection: close\r\n");
  if (body_file || body)
    {
      g_string_append_printf (req, "Content-Length: %" G_GINT64_FORMAT "\r\n", length);
      if (content_type)
        g_string_append_printf (req, "Content-Type: %s\r\n", content_type);
    }
  for (i = 0; extra_headers && i < extra_headers->len; i++)
    g_string_append_printf (req, "%s\r\n", (gchar *) extra_headers->pdata[i]);
  g_string_append (req, "\r\n");

  out = g_io_stream_get_output_stream (G_IO_STREAM (conn));
  if (!g_output_stream_write_all (out, req->str, req->len, NULL, NULL, error))
    return FALSE;

  if (body_file)
    {
      g_autoptr(GFileInputStream) fin = g_file_read (body_file, NULL, error);
      if (fin == NULL ||
          g_output_stream_splice (out, G_INPUT_STREAM (fin),
                                  G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE, NULL, error) < 0)
        return FALSE;
    }
  else if (body)
    {
      if (!g_output_stream_write_all (out, g_bytes_get_data (body, NULL),
                                      g_bytes_get_size (body), NULL, NULL, error))
        return FALSE;
    }

  in = g_data_input_stream_new (g_io_stream_get_input_stream (G_IO_STREAM (conn)));
  g_data_input_stream_set_newline_type (in, G_DATA_STREAM_NEWLINE_TYPE_LF);

  /* Skip interim 1xx responses. */
  for (;;)
    {
      g_clear_pointer (&status_line, g_free);
      status_line = read_line (in, error);
      if (status_line == NULL)
        {
          if (error && *error == NULL)
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                                 "Server closed the connection");
          return FALSE;
        }
      g_clear_pointer (&parts, g_strfreev);
      parts = g_strsplit (status_line, " ", 3);
      if (g_strv_length (parts) < 2 || !g_str_has_prefix (parts[0], "HTTP/"))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       "Bad response: %s", status_line);
          return FALSE;
        }
      resp->status = atoi (parts[1]);
      resp->headers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
      for (;;)
        {
          g_autofree gchar *h = read_line (in, error);
          gchar *colon;
          if (h == NULL)
            return FALSE;
          if (*h == '\0')
            break;
          colon = strchr (h, ':');
          if (colon)
            g_hash_table_replace (resp->headers,
                                  g_ascii_strdown (h, colon - h),
                                  g_strstrip (g_strdup (colon + 1)));
        }
      if (resp->status >= 200)
        break;
      g_clear_pointer (&resp->headers, g_hash_table_unref);
    }

  cl = g_hash_table_lookup (resp->headers, "content-length");
  if (cl)
    {
      gsize n = g_ascii_strtoull (cl, NULL, 10), got = 0;
      gchar *buf = g_malloc (n + 1);
      if (!g_input_stream_read_all (G_INPUT_STREAM (in), buf, n, &got, NULL, error))
        {
          g_free (buf);
          return FALSE;
        }
      buf[got] = '\0';
      resp->body = g_bytes_new_take (buf, got);
    }
  else
    {
      g_autoptr(GOutputStream) mem = g_memory_output_stream_new_resizable ();
      if (g_output_stream_splice (mem, G_INPUT_STREAM (in),
                                  G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET, NULL, error) < 0)
        return FALSE;
      resp->body = g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (mem));
    }

  g_io_stream_close (G_IO_STREAM (conn), NULL, NULL);
  return TRUE;
}

static void
print_body_error (const Response *r)
{
  gsize n = 0;
  const gchar *b = r->body ? g_bytes_get_data (r->body, &n) : NULL;

  g_printerr ("Server answered %u", r->status);
  if (b && n)
    g_printerr (": %.*s", (int) n, b);
  else
    g_printerr ("\n");
}

static void
add_header (GPtrArray *h, const gchar *name, const gchar *value)
{
  g_autofree gchar *esc = NULL;

  if (value == NULL || *value == '\0')
    return;
  /* Percent-encode so names like "Ålesund" survive HTTP. */
  esc = g_uri_escape_string (value, " ,()", TRUE);
  g_ptr_array_add (h, g_strdup_printf ("%s: %s", name, esc));
}

int
main (int argc, char **argv)
{
  g_autoptr(GOptionContext) ctx = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GUri) base = NULL;
  g_autoptr(GPtrArray) headers = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GFile) ogg = NULL;
  g_autoptr(GvsVoicegram) meta = NULL;
  g_autofree gchar *token = NULL;
  g_autofree gchar *voice_path = NULL;
  g_autofree gchar *id = NULL;
  Response r = { 0 };
  const gchar *server;

  setlocale (LC_ALL, "");

  ctx = g_option_context_new ("- send a GNOME Voice voicegram to a gnome-voice-server");
  g_option_context_add_main_entries (ctx, entries, NULL);
  if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
      g_printerr ("%s\n", error->message);
      return EXIT_FAILURE;
    }

  server = opt_server ? opt_server :
           g_getenv ("GVS_SERVER_URL") ? g_getenv ("GVS_SERVER_URL") : DEFAULT_SERVER;
  base = g_uri_parse (server, G_URI_FLAGS_NONE, &error);
  if (base == NULL || g_uri_get_host (base) == NULL ||
      (g_strcmp0 (g_uri_get_scheme (base), "http") != 0 &&
       g_strcmp0 (g_uri_get_scheme (base), "https") != 0))
    {
      g_printerr ("Invalid server URL: %s\n", server);
      return EXIT_FAILURE;
    }

  if (opt_token_file)
    {
      if (!g_file_get_contents (opt_token_file, &token, NULL, &error))
        {
          g_printerr ("%s\n", error->message);
          return EXIT_FAILURE;
        }
      g_strstrip (token);
    }
  else if (g_getenv ("GVS_TOKEN"))
    token = g_strdup (g_getenv ("GVS_TOKEN"));
  if (token && *token)
    g_ptr_array_add (headers, g_strdup_printf ("Authorization: Bearer %s", token));

  /* --list */
  if (opt_list)
    {
      gsize n;
      const gchar *b;

      if (!http_request (base, "GET", "/voicegrams", headers, NULL, NULL, NULL, &r, &error))
        {
          g_printerr ("%s: %s\n", server, error->message);
          return EXIT_FAILURE;
        }
      if (r.status != 200)
        {
          print_body_error (&r);
          response_clear (&r);
          return EXIT_FAILURE;
        }
      b = g_bytes_get_data (r.body, &n);
      fwrite (b, 1, n, stdout);
      response_clear (&r);
      return EXIT_SUCCESS;
    }

  /* --delete ID */
  if (opt_delete)
    {
      g_autofree gchar *path = g_strdup_printf ("/voicegrams/%s", opt_delete);
      gboolean ok;

      if (!http_request (base, "DELETE", path, headers, NULL, NULL, NULL, &r, &error))
        {
          g_printerr ("%s: %s\n", server, error->message);
          return EXIT_FAILURE;
        }
      ok = r.status == 204;
      if (!ok)
        print_body_error (&r);
      else if (!opt_quiet)
        g_print ("Deleted %s\n", opt_delete);
      response_clear (&r);
      return ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }

  /* Upload: find the recording and its metadata. */
  if (opt_ogg)
    ogg = g_file_new_for_commandline_arg (opt_ogg);
  else
    {
      const gchar *music = g_get_user_special_dir (G_USER_DIRECTORY_MUSIC);
      g_autofree gchar *p = g_build_filename (music ? music : g_get_home_dir (),
                                              "GNOME.ogg", NULL);
      ogg = g_file_new_for_path (p);
    }
  if (!g_file_query_exists (ogg, NULL))
    {
      g_autofree gchar *p = g_file_get_parse_name (ogg);
      g_printerr ("No recording at %s (record one with gnome-voice, or use --ogg)\n", p);
      return EXIT_FAILURE;
    }

  if (opt_voice)
    voice_path = g_strdup (opt_voice);
  else
    {
      g_autofree gchar *op = g_file_get_path (ogg);
      if (op && g_str_has_suffix (op, ".ogg"))
        {
          g_autofree gchar *stem = g_strndup (op, strlen (op) - 4);
          voice_path = g_strconcat (stem, ".voice", NULL);
        }
    }
  if (voice_path && g_file_test (voice_path, G_FILE_TEST_EXISTS))
    {
      meta = gvs_voicegram_load (voice_path, &error);
      if (meta == NULL)
        {
          g_printerr ("%s: %s\n", voice_path, error->message);
          return EXIT_FAILURE;
        }
    }
  else
    {
      if (opt_voice)
        {
          g_printerr ("No metadata file %s\n", opt_voice);
          return EXIT_FAILURE;
        }
      meta = gvs_voicegram_new ();
    }

  /* Command-line metadata overrides the .voice file. */
  if (opt_name)
    g_free (meta->name), meta->name = g_strdup (opt_name);
  if (meta->name == NULL)
    meta->name = g_strdup (g_get_real_name ());
  if (opt_location)
    g_free (meta->location), meta->location = g_strdup (opt_location);
  if (opt_lat || opt_lon)
    {
      if (!opt_lat || !opt_lon || !gvs_voicegram_set_coords (meta, opt_lat, opt_lon))
        {
          g_printerr ("--lat and --lon must both be given as decimal degrees\n");
          return EXIT_FAILURE;
        }
    }

  /* 1. POST the audio with metadata in headers. */
  {
    g_autoptr(GPtrArray) h = g_ptr_array_new_with_free_func (g_free);
    guint i;

    for (i = 0; i < headers->len; i++)
      g_ptr_array_add (h, g_strdup (headers->pdata[i]));
    add_header (h, "X-Voicegram-Name", meta->name);
    add_header (h, "X-Voicegram-Uri", meta->uri);
    add_header (h, "X-Voicegram-Location", meta->location);
    add_header (h, "X-Voicegram-Version", meta->version);
    if (meta->has_coords)
      {
        gchar a[G_ASCII_DTOSTR_BUF_SIZE], b[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd (a, sizeof a, "%.6f", meta->lat);
        g_ascii_formatd (b, sizeof b, "%.6f", meta->lon);
        add_header (h, "X-Voicegram-Lat", a);
        add_header (h, "X-Voicegram-Lon", b);
      }

    if (!http_request (base, "POST", "/voicegrams", h, "audio/ogg", ogg, NULL, &r, &error))
      {
        g_printerr ("%s: %s\n", server, error->message);
        return EXIT_FAILURE;
      }
    if (r.status != 201)
      {
        print_body_error (&r);
        response_clear (&r);
        return EXIT_FAILURE;
      }
    id = g_strdup (g_hash_table_lookup (r.headers, "x-voicegram-id"));
    response_clear (&r);
    if (id == NULL)
      {
        g_printerr ("Server did not return a voicegram id\n");
        return EXIT_FAILURE;
      }
  }

  /* 2. PUT the .voice file, if the client wrote one, so the stored
   *    metadata is exactly what gnome-voice recorded. */
  if (voice_path && g_file_test (voice_path, G_FILE_TEST_EXISTS) &&
      !opt_name && !opt_location && !opt_lat)
    {
      g_autofree gchar *contents = NULL;
      g_autofree gchar *path = g_strdup_printf ("/voicegrams/%s.voice", id);
      gsize len;

      if (!g_file_get_contents (voice_path, &contents, &len, &error))
        {
          g_printerr ("%s\n", error->message);
          return EXIT_FAILURE;
        }
      {
        g_autoptr(GBytes) body = g_bytes_new_take (g_steal_pointer (&contents), len);
        if (!http_request (base, "PUT", path, headers, "application/xml", NULL, body, &r, &error))
          {
            g_printerr ("%s: %s\n", server, error->message);
            return EXIT_FAILURE;
          }
      }
      if (r.status != 200)
        {
          g_printerr ("Audio stored as %s, but metadata was rejected. ", id);
          print_body_error (&r);
          response_clear (&r);
          return EXIT_FAILURE;
        }
      response_clear (&r);
    }

  if (opt_quiet)
    g_print ("%s\n", id);
  else
    {
      g_autofree gchar *s = g_uri_to_string (base);
      gsize n = strlen (s);
      while (n > 0 && s[n - 1] == '/')
        s[--n] = '\0';
      g_print ("Voicegram %s stored.\nListen: %s/voicegrams/%s.ogg\n", id, s, id);
    }
  return EXIT_SUCCESS;
}
