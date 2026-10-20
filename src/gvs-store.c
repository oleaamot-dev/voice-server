/* gvs-store.c - on-disk voicegram storage
 *
 * Copyright (C) 2026 Ole Kristian Aamot <ole.aamot@aamotinnovation.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <glib/gstdio.h>
#include <gio/gunixoutputstream.h>

#include "gvs-store.h"

struct _GvsStore {
  gchar *dir;
  gchar *vg_dir;
  gchar *tmp_dir;
};

static gboolean
ensure_dir (const gchar *path, GError **error)
{
  if (g_mkdir_with_parents (path, 0750) != 0)
    {
      int err = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (err),
                   "Cannot create directory %s: %s", path, g_strerror (err));
      return FALSE;
    }
  return TRUE;
}

GvsStore *
gvs_store_open (const gchar  *dir,
                GError      **error)
{
  g_autoptr(GvsStore) store = g_new0 (GvsStore, 1);
  g_autoptr(GDir) tmp = NULL;
  const gchar *name;

  store->dir = g_canonicalize_filename (dir, NULL);
  store->vg_dir = g_build_filename (store->dir, "voicegrams", NULL);
  store->tmp_dir = g_build_filename (store->dir, "tmp", NULL);

  if (!ensure_dir (store->vg_dir, error) || !ensure_dir (store->tmp_dir, error))
    return NULL;

  /* Remove uploads left behind by a crash. */
  tmp = g_dir_open (store->tmp_dir, 0, NULL);
  while (tmp && (name = g_dir_read_name (tmp)) != NULL)
    {
      g_autofree gchar *p = g_build_filename (store->tmp_dir, name, NULL);
      g_unlink (p);
    }

  return g_steal_pointer (&store);
}

void
gvs_store_free (GvsStore *store)
{
  if (store == NULL)
    return;
  g_free (store->dir);
  g_free (store->vg_dir);
  g_free (store->tmp_dir);
  g_free (store);
}

const gchar *
gvs_store_get_dir (GvsStore *store)
{
  return store->dir;
}

gchar *
gvs_store_new_id (void)
{
  g_autoptr(GDateTime) now = g_date_time_new_now_utc ();
  g_autofree gchar *stamp = g_date_time_format (now, "%Y%m%dT%H%M%SZ");

  return g_strdup_printf ("%s-%08x", stamp, g_random_int ());
}

gboolean
gvs_store_id_is_valid (const gchar *id)
{
  gsize len;
  const gchar *p;

  if (id == NULL)
    return FALSE;
  len = strlen (id);
  if (len == 0 || len > GVS_STORE_ID_MAX || id[0] == '-')
    return FALSE;
  for (p = id; *p; p++)
    if (!g_ascii_isalnum (*p) && *p != '-')
      return FALSE;
  return TRUE;
}

gchar *
gvs_store_path (GvsStore    *store,
                const gchar *id,
                const gchar *ext)
{
  g_autofree gchar *base = NULL;

  g_return_val_if_fail (gvs_store_id_is_valid (id), NULL);
  base = g_strconcat (id, ".", ext, NULL);
  return g_build_filename (store->vg_dir, base, NULL);
}

gboolean
gvs_store_exists (GvsStore    *store,
                  const gchar *id,
                  const gchar *ext)
{
  g_autofree gchar *path = NULL;

  if (!gvs_store_id_is_valid (id))
    return FALSE;
  path = gvs_store_path (store, id, ext);
  return g_file_test (path, G_FILE_TEST_IS_REGULAR);
}

GOutputStream *
gvs_store_create_tmp (GvsStore  *store,
                      gchar    **tmp_path,
                      GError   **error)
{
  gchar *path = g_build_filename (store->tmp_dir, "upload-XXXXXX", NULL);
  int fd = g_mkstemp_full (path, O_RDWR, 0640);

  if (fd < 0)
    {
      int err = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (err),
                   "Cannot create temporary file: %s", g_strerror (err));
      g_free (path);
      return NULL;
    }
  *tmp_path = path;
  return g_unix_output_stream_new (fd, TRUE);
}

gboolean
gvs_store_commit (GvsStore    *store,
                  const gchar *tmp_path,
                  const gchar *id,
                  const gchar *ext,
                  GError     **error)
{
  g_autofree gchar *dest = gvs_store_path (store, id, ext);

  if (g_rename (tmp_path, dest) != 0)
    {
      int err = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (err),
                   "Cannot store %s: %s", dest, g_strerror (err));
      return FALSE;
    }
  return TRUE;
}

gboolean
gvs_store_write (GvsStore    *store,
                 const gchar *id,
                 const gchar *ext,
                 const gchar *data,
                 gsize        len,
                 GError     **error)
{
  g_autofree gchar *dest = gvs_store_path (store, id, ext);

  return g_file_set_contents_full (dest, data, len,
                                   G_FILE_SET_CONTENTS_CONSISTENT, 0640, error);
}

gboolean
gvs_store_delete (GvsStore    *store,
                  const gchar *id,
                  GError     **error)
{
  g_autofree gchar *ogg = gvs_store_path (store, id, "ogg");
  g_autofree gchar *voice = gvs_store_path (store, id, "voice");
  gboolean found = FALSE;

  if (g_unlink (voice) == 0)
    found = TRUE;
  if (g_unlink (ogg) == 0)
    found = TRUE;

  if (!found)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No voicegram %s", id);
      return FALSE;
    }
  return TRUE;
}

static gint
compare_desc (gconstpointer a, gconstpointer b)
{
  return -g_strcmp0 (*(const gchar * const *) a, *(const gchar * const *) b);
}

GPtrArray *
gvs_store_list (GvsStore  *store,
                GError   **error)
{
  g_autoptr(GDir) dir = g_dir_open (store->vg_dir, 0, error);
  GPtrArray *ids;
  const gchar *name;

  if (dir == NULL)
    return NULL;

  ids = g_ptr_array_new_with_free_func (g_free);
  while ((name = g_dir_read_name (dir)) != NULL)
    {
      if (g_str_has_suffix (name, ".ogg"))
        {
          gchar *id = g_strndup (name, strlen (name) - 4);

          if (gvs_store_id_is_valid (id))
            g_ptr_array_add (ids, id);
          else
            g_free (id);
        }
    }
  g_ptr_array_sort (ids, compare_desc);
  return ids;
}
