# gnome-voice-server

A standalone server for **GNOME Voice** voicegrams. It accepts the Ogg
Vorbis recording and `.voice` metadata that the
[gnome-voice](https://www.gnomevoice.org/) client produces, stores them,
and serves them back so other Voice clients can list, map and play them.

* Written in C, built with autotools
* Only depends on GLib/GIO (≥ 2.66)
* Plain HTTP/1.1 — see [PROTOCOL.md](PROTOCOL.md)
* Validates every upload (Ogg capture pattern, well-formed Voicegram XML,
  size limits) and writes files atomically
* Byte-range support, so GStreamer players can seek
* Optional bearer-token for uploads; reads are public
* Ships `gnome-voice-send`, a command-line uploader for the client side
* systemd unit with sandboxing, man pages, RPM spec, `make check` suite

## Building

```sh
./autogen.sh --prefix=/usr --sysconfdir=/etc --localstatedir=/var
make
make check
sudo make install
```

From a release tarball, use `./configure` instead of `./autogen.sh`.

Configure options:

| Option | Default |
|--------|---------|
| `--with-storagedir=DIR` | `$localstatedir/lib/gnome-voice-server` |
| `--with-systemdsystemunitdir=DIR\|no` | `$prefix/lib/systemd/system` |
| `--disable-warnings` | extra GCC warnings on |

## Running

```sh
gnome-voice-server --port 8085 --storage-dir ~/voicegrams
```

or as a service:

```sh
sudo systemctl enable --now gnome-voice-server
```

Settings live in `/etc/gnome-voice-server/gnome-voice-server.conf`
(see the commented example). Command-line options win over the file.

### Requiring a token for uploads

```sh
openssl rand -hex 32 | sudo tee /etc/gnome-voice-server/token
sudo chmod 640 /etc/gnome-voice-server/token
# then set token-file= in the config
```

Without a token, anyone who can reach the server can upload — fine on a
LAN, not on the open internet. Deletion is only possible with a token.

### Behind a reverse proxy (TLS)

Run on `127.0.0.1` and let nginx, Apache or Caddy terminate TLS. Set
`public-url=https://voice.example.org` so `<stream>` links point at the
public address. nginx needs `client_max_body_size` at least as large as
`max-size`.

## Sending voicegrams from the client

After recording with gnome-voice:

```sh
gnome-voice-send --server https://voice.example.org
```

This uploads `~/Music/GNOME.ogg` and `~/Music/GNOME.voice`. Other
uses:

```sh
gnome-voice-send -s URL -o talk.ogg -n "Ole Aamot" -l Oslo --lat 59.9139 --lon 10.7522
gnome-voice-send -s URL --list          # Voicegram XML of everything stored
gnome-voice-send -s URL -t token --delete 20261015T070800Z-3f9a1c0e
```

`GVS_SERVER_URL` and `GVS_TOKEN` can be set in the environment instead.

To integrate directly in gnome-voice, POST the file after
`stop_recording()` writes it — e.g. with libsoup:

```c
SoupMessage *msg = soup_message_new ("POST", "https://voice.example.org/voicegrams");
soup_message_headers_append (soup_message_get_request_headers (msg),
                             "X-Voicegram-Name", escaped_name);
soup_message_set_request_body_from_bytes (msg, "audio/ogg", ogg_bytes);
```

then `PUT` `GNOME.voice` to `/voicegrams/<X-Voicegram-Id>.voice`.

## Storage layout

```
STORAGE_DIR/voicegrams/<id>.ogg
STORAGE_DIR/voicegrams/<id>.voice
STORAGE_DIR/tmp/                 in-flight uploads, cleaned at startup
```

Backing up the directory backs up the server.

## License

GPL-3.0-or-later. Copyright © 2026 Ole Kristian Aamot.
