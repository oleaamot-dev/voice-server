/* gvs-voicegram.h - Voicegram Recording 1.0 metadata (.voice XML)
 *
 * Copyright (C) 2026 Ole Kristian Aamot <ole.aamot@aamotinnovation.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define GVS_VOICEGRAM_ERROR (gvs_voicegram_error_quark ())

typedef enum {
  GVS_VOICEGRAM_ERROR_INVALID,
  GVS_VOICEGRAM_ERROR_NO_STATION,
} GvsVoicegramError;

/*
 * One voicegram, as described by a gnome-voice .voice file:
 *
 *   <voice version="...">
 *     <station name="..." uri="...">
 *       <location lat="..." lon="...">City</location>
 *       <stream>http://.../GNOME.ogg</stream>
 *     </station>
 *   </voice>
 *
 * The server adds the optional attributes id="" and date="" on <station>.
 */
typedef struct {
  gchar   *version;      /* client version from <voice version="">   */
  gchar   *id;           /* server-assigned voicegram id             */
  gchar   *date;         /* ISO 8601 UTC creation time               */
  gchar   *name;         /* station name (the speaker)               */
  gchar   *uri;          /* station uri                              */
  gchar   *location;     /* free-text place, e.g. "Oslo"             */
  gboolean has_coords;
  gdouble  lat;
  gdouble  lon;
  gchar   *stream;       /* where the Ogg Vorbis audio can be played */
} GvsVoicegram;

GQuark        gvs_voicegram_error_quark (void);

GvsVoicegram *gvs_voicegram_new         (void);
void          gvs_voicegram_free        (GvsVoicegram *vg);

GvsVoicegram *gvs_voicegram_parse       (const gchar  *xml,
                                         gssize        length,
                                         GError      **error);
GvsVoicegram *gvs_voicegram_load        (const gchar  *path,
                                         GError      **error);

gboolean      gvs_voicegram_set_coords  (GvsVoicegram *vg,
                                         const gchar  *lat,
                                         const gchar  *lon);

/* A single <station> element, indented by @indent spaces. */
gchar        *gvs_voicegram_station_xml (const GvsVoicegram *vg,
                                         guint               indent);
/* A complete .voice document. */
gchar        *gvs_voicegram_to_xml      (const GvsVoicegram *vg);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GvsVoicegram, gvs_voicegram_free)

G_END_DECLS
