/* gvs-main.c - gnome-voice-server daemon
 *
 * Copyright (C) 2026 Ole Kristian Aamot <ole.aamot@aamotinnovation.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <glib-unix.h>
#include <glib/gstdio.h>

#include "gvs-store.h"
#include "gvs-server.h"

#define DEFAULT_PORT        8085
#define DEFAULT_MAX_AUDIO   (32 * 1024 * 1024)
#define DEFAULT_MAX_META    (64 * 1024)
#define DEFAULT_MAX_CLIENTS 32
#define DEFAULT_TIMEOUT     60
#define DEFAULT_CONFIG      SYSCONFDIR "/gnome-voice-server/gnome-voice-server.conf"

static gchar   *opt_config;
static gchar   *opt_address;
static gint     opt_port = -1;
static gchar   *opt_storage;
static gchar   *opt_public_url;
static gchar   *opt_token_file;
static gchar   *opt_port_file;
static gint64   opt_max_audio = -1;
static gint     opt_max_clients = -1;
static gboolean opt_read_only;
static gboolean opt_version;

static const GOptionEntry entries[] = {
  { "config",      'c', 0, G_OPTION_ARG_FILENAME, &opt_config,
    "Configuration file (default " DEFAULT_CONFIG ")", "FILE" },
  { "address",     'a', 0, G_OPTION_ARG_STRING,   &opt_address,
    "Address to listen on (default: all)", "ADDR" },
  { "port",        'p', 0, G_OPTION_ARG_INT,      &opt_port,
    "TCP port (default 8085, 0 = any free port)", "PORT" },
  { "storage-dir", 's', 0, G_OPTION_ARG_FILENAME, &opt_storage,
    "Where voicegrams are stored (default " GVS_STORAGEDIR ")", "DIR" },
  { "public-url",  'u', 0, G_OPTION_ARG_STRING,   &opt_public_url,
    "Public base URL used in <stream> links, e.g. https://voice.example.org", "URL" },
  { "token-file",  't', 0, G_OPTION_ARG_FILENAME, &opt_token_file,
    "File holding the bearer token required for uploads", "FILE" },
  { "max-size",    'm', 0, G_OPTION_ARG_INT64,    &opt_max_audio,
    "Largest accepted voicegram in bytes (default 32 MiB)", "BYTES" },
  { "max-clients", 0,   0, G_OPTION_ARG_INT,      &opt_max_clients,
    "Concurrent connections (default 32)", "N" },
  { "read-only",   'r', 0, G_OPTION_ARG_NONE,     &opt_read_only,
    "Serve stored voicegrams but refuse uploads", NULL },
  { "port-file",   0,   G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_FILENAME, &opt_port_file,
    "Write the bound port number to FILE (for tests)", "FILE" },
  { "version",     'V', 0, G_OPTION_ARG_NONE,     &opt_version,
    "Print version and exit", NULL },
  { NULL }
};

static gboolean
on_quit_signal (gpointer user_data)
{
  g_message ("shutting down");
  g_main_loop_quit (user_data);
  return G_SOURCE_REMOVE;
}

static gchar *
read_token (const gchar *path, GError **error)
{
  g_autofree gchar *contents = NULL;
  GStatBuf st;

  if (!g_file_get_contents (path, &contents, NULL, error))
    return NULL;
  if (g_stat (path, &st) == 0 && (st.st_mode & 0004))
    g_warning ("token file %s is world-readable", path);
  g_strstrip (contents);
  if (*contents == '\0')
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Token file %s is empty", path);
      return NULL;
    }
  return g_steal_pointer (&contents);
}

/* Fills unset options from the [server] group of a key file.
 * Command-line options win. A missing default config file is fine. */
static gboolean
load_config (GvsServerConfig *cfg, gchar **storage, gchar **token_file, GError **error)
{
  g_autoptr(GKeyFile) kf = g_key_file_new ();
  g_autoptr(GError) local = NULL;
  const gchar *path = opt_config ? opt_config : DEFAULT_CONFIG;
  g_autofree gchar *s = NULL;

  if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, &local))
    {
      if (opt_config == NULL && g_error_matches (local, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        return TRUE;
      g_propagate_prefixed_error (error, g_steal_pointer (&local), "%s: ", path);
      return FALSE;
    }

#define KF_STRING(key, dest) \
  if ((dest) == NULL && (s = g_key_file_get_string (kf, "server", key, NULL)) && *g_strstrip (s)) \
    (dest) = g_steal_pointer (&s); \
  g_clear_pointer (&s, g_free);

  KF_STRING ("address", cfg->address);
  KF_STRING ("public-url", cfg->public_url);
  KF_STRING ("storage-dir", *storage);
  KF_STRING ("token-file", *token_file);
#undef KF_STRING

  if (opt_port < 0 && g_key_file_has_key (kf, "server", "port", NULL))
    {
      gint v = g_key_file_get_integer (kf, "server", "port", &local);
      if (local || v < 0 || v > 65535)
        goto bad;
      cfg->port = v;
    }
  if (opt_max_audio < 0 && g_key_file_has_key (kf, "server", "max-size", NULL))
    {
      gint64 v = g_key_file_get_int64 (kf, "server", "max-size", &local);
      if (local || v <= 0)
        goto bad;
      cfg->max_audio_size = v;
    }
  if (opt_max_clients < 0 && g_key_file_has_key (kf, "server", "max-clients", NULL))
    {
      gint v = g_key_file_get_integer (kf, "server", "max-clients", &local);
      if (local || v <= 0)
        goto bad;
      cfg->max_clients = v;
    }
  if (!opt_read_only && g_key_file_has_key (kf, "server", "read-only", NULL))
    {
      cfg->read_only = g_key_file_get_boolean (kf, "server", "read-only", &local);
      if (local)
        goto bad;
    }
  return TRUE;

bad:
  g_set_error (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
               "%s: invalid value in [server]%s%s", path,
               local ? ": " : "", local ? local->message : "");
  return FALSE;
}

int
main (int argc, char **argv)
{
  g_autoptr(GOptionContext) ctx = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GvsStore) store = NULL;
  g_autoptr(GvsServer) server = NULL;
  g_autoptr(GMainLoop) loop = NULL;
  g_autofree gchar *storage = NULL;
  g_autofree gchar *token_file = NULL;
  gboolean have_token;
  GvsServerConfig cfg = {
    .port = DEFAULT_PORT,
    .max_audio_size = DEFAULT_MAX_AUDIO,
    .max_meta_size = DEFAULT_MAX_META,
    .max_clients = DEFAULT_MAX_CLIENTS,
    .timeout = DEFAULT_TIMEOUT,
  };

  setlocale (LC_ALL, "");

  ctx = g_option_context_new ("- store and serve GNOME Voice voicegrams");
  g_option_context_add_main_entries (ctx, entries, NULL);
  g_option_context_set_description (ctx,
      "Accepts Ogg Vorbis voicegrams and their .voice metadata from the\n"
      "gnome-voice client over HTTP, and serves them back for playback.\n"
      "Report bugs to <" PACKAGE_BUGREPORT ">.\n");
  if (!g_option_context_parse (ctx, &argc, &argv, &error))
    {
      g_printerr ("%s\n", error->message);
      return EXIT_FAILURE;
    }
  if (opt_version)
    {
      g_print ("%s %s\n", PACKAGE_NAME, PACKAGE_VERSION);
      return EXIT_SUCCESS;
    }

  /* Command line first, then config file, then built-in defaults. */
  cfg.address = opt_address;
  cfg.public_url = opt_public_url;
  storage = g_strdup (opt_storage);
  token_file = g_strdup (opt_token_file);
  if (opt_port >= 0)
    {
      if (opt_port > 65535)
        {
          g_printerr ("Invalid port %d\n", opt_port);
          return EXIT_FAILURE;
        }
      cfg.port = opt_port;
    }
  if (opt_max_audio > 0)
    cfg.max_audio_size = opt_max_audio;
  if (opt_max_clients > 0)
    cfg.max_clients = opt_max_clients;
  cfg.read_only = opt_read_only;

  if (!load_config (&cfg, &storage, &token_file, &error))
    {
      g_printerr ("%s\n", error->message);
      return EXIT_FAILURE;
    }
  if (storage == NULL)
    storage = g_strdup (GVS_STORAGEDIR);

  if (token_file)
    {
      cfg.token = read_token (token_file, &error);
      if (cfg.token == NULL)
        {
          g_printerr ("%s\n", error->message);
          return EXIT_FAILURE;
        }
    }
  else if (g_getenv ("GVS_TOKEN") && *g_getenv ("GVS_TOKEN"))
    cfg.token = g_strdup (g_getenv ("GVS_TOKEN"));

  store = gvs_store_open (storage, &error);
  if (store == NULL)
    {
      g_printerr ("%s\n", error->message);
      return EXIT_FAILURE;
    }

  server = gvs_server_new (store, &cfg, &error);
  have_token = cfg.token != NULL;
  g_clear_pointer (&cfg.token, g_free);
  if (server == NULL)
    {
      g_printerr ("Cannot listen: %s\n", error->message);
      return EXIT_FAILURE;
    }

  if (opt_port_file)
    {
      g_autofree gchar *p = g_strdup_printf ("%u\n", gvs_server_get_port (server));
      if (!g_file_set_contents (opt_port_file, p, -1, &error))
        {
          g_printerr ("%s\n", error->message);
          return EXIT_FAILURE;
        }
    }

  loop = g_main_loop_new (NULL, FALSE);
  g_unix_signal_add (SIGINT, on_quit_signal, loop);
  g_unix_signal_add (SIGTERM, on_quit_signal, loop);

  gvs_server_start (server);
  g_message ("%s %s listening on %s:%u, storing voicegrams in %s%s%s",
             PACKAGE_NAME, PACKAGE_VERSION,
             cfg.address ? cfg.address : "*", gvs_server_get_port (server),
             gvs_store_get_dir (store),
             cfg.read_only ? " (read-only)" : "",
             have_token || cfg.read_only ? "" : " — uploads are open to anyone");

  g_main_loop_run (loop);
  gvs_server_stop (server);
  return EXIT_SUCCESS;
}
