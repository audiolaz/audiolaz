# The audiolaz player

`audiolaz-player` (its `--version`: `audiolaz-roomd <version> <fr|en>`) reads music from its inputs and writes it to
one output, unchanged. One process per output DAC. Everything it keeps lives in its rooms folder (`--rooms`).

## Command line

```
audiolaz-player [--loop <index|id>|none] [--loop-subs N] [--dac <ALSA device | Core Audio UID>] [--rooms <folder>]
                [--squeeze <server|auto> --name <name>] [--page <port> --output <label>] [--upnp on]
                [--net off | --net-port 9001 --slim-port 3484 --airplay-out on --airplay-conf <file>]
                [--rt 0] [--writer-rt 94] [--writer-cpu N] [--mlock 1] | --version | --list-devices | --who
```

| Option | Meaning |
| --- | --- |
| `--squeeze <server|auto>` | the Squeezebox input: Roon (its Squeezebox support on) or Lyrion/LMS; `auto`: the first that answers |
| `--name` | the player's name (Roon's zone, the AirPlay input, the UPnP renderer) |
| `--loop <card>` `--loop-subs N` | Linux: the ALSA Loopback card (`snd-aloop`) whose subdevices 0…N−1 the local sources play into |
| `--loop none` | no Loopback: the Squeezebox input only (and the built-in UPnP renderer with `--upnp on`) |
| `--dac` | the default output: an ALSA device (`hw:CARD=…,DEV=0`; `plughw:` lets ALSA convert what the card refuses) or, on macOS, a device's UID or name |
| `--page <port>` | the player's own web page and its HTTP API (below) |
| `--upnp on` | the built-in UPnP/DLNA renderer |
| `--net off` | no network outputs (no port opened) |
| `--rt`, `--writer-rt`, `--writer-cpu`, `--mlock` | real-time priority of the reading thread and of the output's writer, the writer's core, memory locked |

Local sources on Linux: each plays into its own subdevice of the Loopback's device 1 (AirPlay through shairport-sync's
ALSA output, Spotify Connect through librespot's, Roon Bridge…); `<rooms>/inputs.conf` names them for the page, one
line each, `<subdevice>=<name>` (`1=airplay`, `2=spotify`). The source that started last plays.

The `audiolaz` sound card (`src/linux/kmod`, `modprobe snd-audiolaz`) or the `audiolaz` ALSA output
(`src/player/pcm_azdrv.c`) let any Linux application play straight into the player, paced by the DAC's clock; on
macOS, the `audiolaz` output (`src/mac/driver`) does the same for every app of the Mac.

## The control file

`<rooms>/control`, `key=value` lines, read again whenever it changes (write a new file and rename it in place):

| Key | Values |
| --- | --- |
| `out=` | the output: `alsa:<device>`, `squeeze:<name or MAC>`, `upnp:<UDN>`, `airplay:<MAC or name>`, `audiolaz:<id or address>`; empty: `--dac` |
| `airplay_mode=` | `auto` (by the sender: an Apple TV is video), `video`, `music` |
| `airplay_buffered=` | `full` or `declared`: how AirPlay's buffered streams' offset is set |
| `upnp=` | `1` / `0`: the built-in renderer on or off |
| `swap.<out>=` | `1`: left and right swapped on that output (`<out>` as `out=` names it, `default` for `--dac`); absent: as they come |
| `invert_l.<out>=` `invert_r.<out>=` | `1`: the polarity (phase) of the speaker playing the left (right) channel inverted on that output; absent: normal |

The page writes it for you (`POST /output?to=…`, `POST /airplay?m=…`, `POST /upnp?on=…`, `POST /channels?…`).

### The speakers' channels

Swapping left and right and inverting a speaker's polarity (`src/player/channels.h`) are the very last step before
the output, for every kind of output, and each output keeps its own setting. The swap moves the samples as they are;
an inversion is the exact negation of each sample in the output's format (the most negative value, which has no
opposite, becomes the largest positive one). Nothing is resampled or rounded: the sound is lossless, though no longer the source channel for channel
(`bitPerfectWhy` says `swap` or `polarity`, `bitPerfectLossless` true). « Left speaker » is the one playing the music's
left channel: its inversion follows it when left and right are swapped. A change while music plays is a crossfade of
5 ms (no click). The output is always one stereo pair: these act on it alone.

## The page and its API (`--page <port>`)

| Request | |
| --- | --- |
| `GET /` | the player's page |
| `GET /state` | everything the page shows, JSON (below) |
| `GET /clock` | the player's clock (`{"ns":…}`, CLOCK_MONOTONIC) |
| `POST /output?to=<out=, URL-encoded>` | the output (empty: `--dac`) |
| `POST /channels?swap=0\|1&invert_l=0\|1&invert_r=0\|1[&out=<out=>]` | the speakers' channels of an output (any of the three; `out=` absent: the one playing, `default`: `--dac`); answers `{"ok":true,"outChannels":{…}}` |
| `POST /measure?port=<p>&signal=<name>` | plays the measurement track the asking app serves at `/download/measurement-track.wav?signal=<name>` on its port; `POST /measure/stop` |
| `POST /nowplaying` | what plays (title, artist, album, cover) for receivers with a screen |
| `POST /take` | the next stream to an audiolaz Link receiver takes it over from the player playing there |
| `POST /app?url=…` | the address of the app that drives the player (the page links to it) |

Any other change asked of the free player is answered `403 {"ok":false,"why":"room"}`.

`GET /state` (an excerpt):

```json
{
  "playing": true, "rate": 44100, "state": "playing", "name": "Living room", "version": "1.2.6", "lang": "en",
  "input": "squeeze", "out": "", "outKind": "alsa", "outName": "", "receivers": [ … ], "cards": [ … ],
  "squeeze": true, "format": "flac",
  "variant": "player", "room": false,
  "bitPerfect": true, "bitPerfectWhy": [], "bitPerfectSpans": 64, "bitPerfectLossless": null,
  "outChannels": { "swap": false, "invertL": false, "invertR": false, "now": 0 },
  "soundPath": { "input": "squeeze", "inRate": 44100, "inBits": 24, "channels": 2, "outRate": 44100, "outBits": 32,
                 "deviceRate": null, "dac": "hw:CARD=DAC,DEV=0" },
  "inputLatencyMs": 812, "airplayOffsetMs": null, "link": null
}
```

`outChannels`: the speakers' channels the control keeps for the output playing; `now`: what the output plays with
(bits: 1 swapped, 2 left inverted, 4 right inverted).

`<rooms>/status` holds the same as `key=value` lines, rewritten once a second.
