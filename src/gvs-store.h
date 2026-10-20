/* gvs-store.h - on-disk voicegram storage
 *
 * Copyright (C) 2026 Ole Kristian Aamot <ole.aamot@aamotinnovation.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/*
 * Layout:
 *
 *   STORAGE_DIR/
 *     voicegrams/<id>.ogg     Ogg Vorbis audio
 *     voicegrams/<id>.voice   Voicegram XML metadata
 *     tmp/                    in-flight uploads (same filesystem → atomic rename)
 *
 * Ids look like 20261015T070800Z-3f9a1c0e: sortable by time, unguessable
 * enough not to collide, and restricted to [0-9A-Za-z-] so they can be
 * used directly as file names.
 */

#define GVS_STORE_ID_MAX 64

typedef struct _GvsStore GvsStore;

GvsStore  *gvs_store_open         (const gchar  *dir,
                                   GError      **error);
void       gvs_store_free         (GvsStore     *store);

const gchar *gvs_store_get_dir    (GvsStore     *store);

gchar     *gvs_store_new_id       (void);
gboolean   gvs_store_id_is_valid  (const gchar  *id);

/* Returns the path of <id>.<ext>; ext is "ogg" or "voice". */
gchar     *gvs_store_path         (GvsStore     *store,
                                   const gchar  *id,
                                   const gchar  *ext);
gboolean   gvs_store_exists       (GvsStore     *store,
                                   const gchar  *id,
                                   const gchar  *ext);

/* Creates an empty temp file for an upload; caller owns the stream. */
GOutputStream *gvs_store_create_tmp (GvsStore     *store,
                                     gchar       **tmp_path,
                                     GError      **error);

/* Atomically moves a finished upload into place as <id>.<ext>. */
gboolean   gvs_store_commit       (GvsStore     *store,
                                   const gchar  *tmp_path,
                                   const gchar  *id,
                                   const gchar  *ext,
                                   GError      **error);

/* Atomically writes <id>.<ext> from memory. */
gboolean   gvs_store_write        (GvsStore     *store,
                                   const gchar  *id,
                                   const gchar  *ext,
                                   const gchar  *data,
                                   gsize         len,
                                   GError      **error);

gboolean   gvs_store_delete       (GvsStore     *store,
                                   const gchar  *id,
                                   GError      **error);

/* Ids that have audio, newest first. Free with g_ptr_array_unref. */
GPtrArray *gvs_store_list         (GvsStore     *store,
                                   GError      **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GvsStore, gvs_store_free)

G_END_DECLS
