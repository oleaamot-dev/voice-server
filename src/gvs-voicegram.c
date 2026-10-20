/* gvs-voicegram.c - Voicegram Recording 1.0 metadata (.voice XML)
 *
 * Copyright (C) 2026 Ole Kristian Aamot <ole.aamot@aamotinnovation.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include "config.h"

#include <string.h>
#include <math.h>

#include "gvs-voicegram.h"

G_DEFINE_QUARK (gvs-voicegram-error-quark, gvs_voicegram_error)

/* g_set_str() needs GLib 2.76; keep our own for older systems. */
static void
set_str (gchar **dest, const gchar *value)
{
  if (*dest == value)
    return;
  g_free (*dest);
  *dest = g_strdup (value);
}

GvsVoicegram *
gvs_voicegram_new (void)
{
  return g_new0 (GvsVoicegram, 1);
}

void
gvs_voicegram_free (GvsVoicegram *vg)
{
  if (vg == NULL)
    return;
  g_free (vg->version);
  g_free (vg->id);
  g_free (vg->date);
  g_free (vg->name);
  g_free (vg->uri);
  g_free (vg->location);
  g_free (vg->stream);
  g_free (vg);
}

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

typedef enum {
  IN_NONE,
  IN_LOCATION,
  IN_STREAM,
} TextTarget;

typedef struct {
  GvsVoicegram *vg;
  guint         depth;
  gboolean      in_first_station;
  gboolean      seen_station;
  TextTarget    target;
  GString      *text;
} ParseState;

static gboolean
parse_double (const gchar *s, gdouble min, gdouble max, gdouble *out)
{
  gchar *end = NULL;
  gdouble v;

  if (s == NULL || *s == '\0')
    return FALSE;
  v = g_ascii_strtod (s, &end);
  while (end && g_ascii_isspace (*end))
    end++;
  if (end == NULL || *end != '\0' || !isfinite (v) || v < min || v > max)
    return FALSE;
  *out = v;
  return TRUE;
}

gboolean
gvs_voicegram_set_coords (GvsVoicegram *vg,
                          const gchar  *lat,
                          const gchar  *lon)
{
  gdouble la, lo;

  if (!parse_double (lat, -90.0, 90.0, &la) ||
      !parse_double (lon, -180.0, 180.0, &lo))
    return FALSE;
  vg->lat = la;
  vg->lon = lo;
  vg->has_coords = TRUE;
  return TRUE;
}

static void
start_element (GMarkupParseContext  *context,
               const gchar          *element_name,
               const gchar         **attribute_names,
               const gchar         **attribute_values,
               gpointer              user_data,
               GError              **error)
{
  ParseState *st = user_data;
  guint i;

  st->depth++;

  if (st->depth == 1)
    {
      if (g_strcmp0 (element_name, "voice") != 0)
        {
          g_set_error (error, GVS_VOICEGRAM_ERROR, GVS_VOICEGRAM_ERROR_INVALID,
                       "root element must be <voice>, not <%s>", element_name);
          return;
        }
      for (i = 0; attribute_names[i]; i++)
        if (g_str_equal (attribute_names[i], "version"))
          set_str (&st->vg->version, attribute_values[i]);
      return;
    }

  if (st->depth == 2 && g_str_equal (element_name, "station"))
    {
      /* A .voice file from the client describes one voicegram; extra
       * stations (multi-player files) are accepted but ignored. */
      if (st->seen_station)
        return;
      st->seen_station = TRUE;
      st->in_first_station = TRUE;
      for (i = 0; attribute_names[i]; i++)
        {
          if (g_str_equal (attribute_names[i], "name"))
            set_str (&st->vg->name, attribute_values[i]);
          else if (g_str_equal (attribute_names[i], "uri"))
            set_str (&st->vg->uri, attribute_values[i]);
          else if (g_str_equal (attribute_names[i], "id"))
            set_str (&st->vg->id, attribute_values[i]);
          else if (g_str_equal (attribute_names[i], "date"))
            set_str (&st->vg->date, attribute_values[i]);
        }
      return;
    }

  if (st->depth == 3 && st->in_first_station)
    {
      if (g_str_equal (element_name, "location"))
        {
          const gchar *lat = NULL, *lon = NULL;

          for (i = 0; attribute_names[i]; i++)
            {
              if (g_str_equal (attribute_names[i], "lat"))
                lat = attribute_values[i];
              else if (g_str_equal (attribute_names[i], "lon"))
                lon = attribute_values[i];
            }
          /* Invalid or missing coordinates are dropped, not fatal. */
          if (lat && lon)
            gvs_voicegram_set_coords (st->vg, lat, lon);
          st->target = IN_LOCATION;
          g_string_truncate (st->text, 0);
        }
      else if (g_str_equal (element_name, "stream"))
        {
          st->target = IN_STREAM;
          g_string_truncate (st->text, 0);
        }
    }
}

static void
end_element (GMarkupParseContext  *context,
             const gchar          *element_name,
             gpointer              user_data,
             GError              **error)
{
  ParseState *st = user_data;

  if (st->depth == 3 && st->target != IN_NONE)
    {
      gchar *value = g_strstrip (g_strdup (st->text->str));

      if (st->target == IN_LOCATION)
        set_str (&st->vg->location, *value ? value : NULL);
      else
        set_str (&st->vg->stream, *value ? value : NULL);
      g_free (value);
      st->target = IN_NONE;
    }
  if (st->depth == 2 && st->in_first_station && g_str_equal (element_name, "station"))
    st->in_first_station = FALSE;

  st->depth--;
}

static void
text (GMarkupParseContext  *context,
      const gchar          *txt,
      gsize                 text_len,
      gpointer              user_data,
      GError              **error)
{
  ParseState *st = user_data;

  if (st->target != IN_NONE && st->text->len + text_len <= 4096)
    g_string_append_len (st->text, txt, text_len);
}

static const GMarkupParser voice_parser = {
  start_element, end_element, text, NULL, NULL
};

GvsVoicegram *
gvs_voicegram_parse (const gchar  *xml,
                     gssize        length,
                     GError      **error)
{
  g_autoptr(GvsVoicegram) vg = gvs_voicegram_new ();
  g_autoptr(GMarkupParseContext) ctx = NULL;
  ParseState st = { 0 };
  gboolean ok;

  g_return_val_if_fail (xml != NULL, NULL);

  if (length < 0)
    length = strlen (xml);

  if (!g_utf8_validate (xml, length, NULL))
    {
      g_set_error_literal (error, GVS_VOICEGRAM_ERROR, GVS_VOICEGRAM_ERROR_INVALID,
                           "voicegram metadata is not valid UTF-8");
      return NULL;
    }

  /* GMarkup rejects the <?xml ...?> prolog only if malformed; it
   * passes it to the passthrough handler, which we ignore. */
  st.vg = vg;
  st.text = g_string_new (NULL);
  ctx = g_markup_parse_context_new (&voice_parser, G_MARKUP_PREFIX_ERROR_POSITION, &st, NULL);

  ok = g_markup_parse_context_parse (ctx, xml, length, error) &&
       g_markup_parse_context_end_parse (ctx, error);
  g_string_free (st.text, TRUE);

  if (!ok)
    return NULL;

  if (!st.seen_station)
    {
      g_set_error_literal (error, GVS_VOICEGRAM_ERROR, GVS_VOICEGRAM_ERROR_NO_STATION,
                           "<voice> contains no <station>");
      return NULL;
    }

  return g_steal_pointer (&vg);
}

GvsVoicegram *
gvs_voicegram_load (const gchar  *path,
                    GError      **error)
{
  g_autofree gchar *contents = NULL;
  gsize len = 0;

  if (!g_file_get_contents (path, &contents, &len, error))
    return NULL;
  return gvs_voicegram_parse (contents, len, error);
}

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */

static void
append_attr (GString *s, const gchar *name, const gchar *value)
{
  g_autofree gchar *esc = NULL;

  if (value == NULL)
    return;
  esc = g_markup_escape_text (value, -1);
  g_string_append_printf (s, " %s=\"%s\"", name, esc);
}

gchar *
gvs_voicegram_station_xml (const GvsVoicegram *vg,
                           guint               indent)
{
  GString *s = g_string_new (NULL);
  g_autofree gchar *pad = g_strnfill (indent, ' ');

  g_string_append_printf (s, "%s<station", pad);
  append_attr (s, "id", vg->id);
  append_attr (s, "name", vg->name ? vg->name : "Anonymous");
  append_attr (s, "uri", vg->uri);
  append_attr (s, "date", vg->date);
  g_string_append (s, ">\n");

  if (vg->location || vg->has_coords)
    {
      g_string_append_printf (s, "%s  <location", pad);
      if (vg->has_coords)
        {
          gchar lat[G_ASCII_DTOSTR_BUF_SIZE], lon[G_ASCII_DTOSTR_BUF_SIZE];

          g_ascii_formatd (lat, sizeof lat, "%.6f", vg->lat);
          g_ascii_formatd (lon, sizeof lon, "%.6f", vg->lon);
          g_string_append_printf (s, " lat=\"%s\" lon=\"%s\"", lat, lon);
        }
      if (vg->location)
        {
          g_autofree gchar *esc = g_markup_escape_text (vg->location, -1);
          g_string_append_printf (s, ">%s</location>\n", esc);
        }
      else
        g_string_append (s, "/>\n");
    }

  if (vg->stream)
    {
      g_autofree gchar *esc = g_markup_escape_text (vg->stream, -1);
      g_string_append_printf (s, "%s  <stream>%s</stream>\n", pad, esc);
    }

  g_string_append_printf (s, "%s</station>\n", pad);
  return g_string_free (s, FALSE);
}

gchar *
gvs_voicegram_to_xml (const GvsVoicegram *vg)
{
  GString *s = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  g_autofree gchar *station = gvs_voicegram_station_xml (vg, 2);

  g_string_append (s, "<voice");
  append_attr (s, "version", vg->version ? vg->version : PACKAGE_VERSION);
  g_string_append (s, ">\n");
  g_string_append (s, station);
  g_string_append (s, "</voice>\n");
  return g_string_free (s, FALSE);
}
