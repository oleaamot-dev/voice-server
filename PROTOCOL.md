# Voicegram Server Protocol 1.0

`gnome-voice-server` speaks plain HTTP/1.1. A voicegram is two files, as
defined by the Voicegram Recording 1.0 Specification used by GNOME Voice:

| Part      | Format                | Client file (default)      |
|-----------|-----------------------|----------------------------|
| Audio     | Ogg Vorbis            | `~/Music/GNOME.ogg`        |
| Metadata  | Voicegram XML `.voice`| `~/Music/GNOME.voice`      |

The server assigns each voicegram an **id** such as
`20261015T070800Z-3f9a1c0e` (UTC time + random suffix, characters
`[0-9A-Za-z-]`, max 64).

## Uploading

Uploading is two requests. The first creates the voicegram; the second
is optional and replaces the generated metadata with the client's own
`.voice` file.

### 1. `POST /voicegrams` — audio

```
POST /voicegrams HTTP/1.1
Host: voice.example.org
Authorization: Bearer <token>          (only if the server has a token)
Content-Type: audio/ogg
Content-Length: 182344
X-Voicegram-Name: Ole%20Aamot           (optional, percent-encoded UTF-8)
X-Voicegram-Location: %C3%85lesund      (optional)
X-Voicegram-Lat: 62.472200              (optional, decimal degrees)
X-Voicegram-Lon: 6.149500               (optional)
X-Voicegram-Uri: https://example.org/   (optional)
X-Voicegram-Version: 8.0.0              (optional, client version)

<Ogg Vorbis bytes>
```

* `Content-Length` is required (no chunked encoding).
* The body must start with the Ogg capture pattern `OggS`.
* Accepted content types: `audio/ogg`, `application/ogg`, `audio/vorbis`,
  `audio/x-vorbis+ogg`, `application/octet-stream`, or none.
* `Expect: 100-continue` is honoured.

Response `201 Created`:

```
Location: /voicegrams/<id>.voice
X-Voicegram-Id: <id>
Content-Type: application/xml; charset=utf-8

<?xml version="1.0" encoding="UTF-8"?>
<voice version="8.0.0">
  <station id="<id>" name="Ole Aamot" date="2026-10-15T05:16:36Z">
    <location lat="62.472200" lon="6.149500">Ålesund</location>
    <stream>https://voice.example.org/voicegrams/<id>.ogg</stream>
  </station>
</voice>
```

### 2. `PUT /voicegrams/<id>.voice` — metadata (optional)

Body: the `.voice` file exactly as gnome-voice wrote it
(`Content-Type: application/xml`, max 64 KiB). The root must be
`<voice>` with at least one `<station>`; only the first station is used.

The server keeps the client's `name`, `uri`, `location`, coordinates and
`version`, and **owns** `id`, `date` and `<stream>` (rewritten to point at
the stored audio). Response `200 OK` with the stored XML.

## Reading

| Request                         | Result                                            |
|---------------------------------|---------------------------------------------------|
| `GET /voicegrams[?limit=N]`     | All voicegrams, newest first (default 100, max 1000), as one `<voice>` document with many `<station>`s — the multi-player format gnome-voice reads from `gnome-voice.xml`. |
| `GET /voicegrams/<id>.ogg`      | The audio, `audio/ogg`. Supports single `Range: bytes=` requests (206/416) so GStreamer can seek. |
| `GET /voicegrams/<id>.voice`    | The metadata (also `GET /voicegrams/<id>`).        |
| `GET /health`                   | `ok`                                              |
| `HEAD` on any of the above      | Headers only.                                     |

Reads never require a token.

## Deleting

`DELETE /voicegrams/<id>` with `Authorization: Bearer <token>` → `204`.
Disabled (`403`) when the server has no token configured.

## Errors

Errors are `text/plain` bodies of the form `413 Content Too Large: …`.

| Code | When |
|------|------|
| 400 | Malformed request, bad XML, upload cut short |
| 401 | Token required and missing/wrong |
| 403 | Server is read-only, or delete without a configured token |
| 404 | Unknown voicegram, or PUT before POST |
| 411 | No `Content-Length` / chunked body |
| 413 | Larger than `max-size` (audio) or 64 KiB (metadata) |
| 415 | Not Ogg |
| 416 | Unsatisfiable range |
| 431 | Headers over 32 KiB / 64 fields |

## Example with curl

```sh
curl -H 'Content-Type: audio/ogg' \
     -H 'X-Voicegram-Name: Ole%20Aamot' \
     --data-binary @"$HOME/Music/GNOME.ogg" \
     https://voice.example.org/voicegrams
```
