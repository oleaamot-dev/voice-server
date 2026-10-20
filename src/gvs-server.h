/* gvs-server.h - HTTP front end for voicegram upload and playback
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
#include "gvs-store.h"

G_BEGIN_DECLS

typedef struct {
  gchar   *address;         /* bind address, NULL = all interfaces       */
  guint16  port;            /* 0 = pick a free port                      */
  gchar   *public_url;      /* base URL written into <stream>, or NULL   */
  gchar   *token;           /* bearer token for uploads, or NULL         */
  gint64   max_audio_size;  /* bytes                                     */
  gsize    max_meta_size;   /* bytes                                     */
  guint    max_clients;     /* concurrent connections                    */
  guint    timeout;         /* socket idle timeout in seconds            */
  gboolean read_only;       /* refuse all uploads                        */
} GvsServerConfig;

typedef struct _GvsServer GvsServer;

GvsServer *gvs_server_new      (GvsStore               *store,
                                const GvsServerConfig  *config,
                                GError                **error);
void       gvs_server_free     (GvsServer              *server);
guint16    gvs_server_get_port (GvsServer              *server);
void       gvs_server_start    (GvsServer              *server);
void       gvs_server_stop     (GvsServer              *server);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GvsServer, gvs_server_free)

G_END_DECLS
