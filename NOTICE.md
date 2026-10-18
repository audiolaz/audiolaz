# Third-party notices

The files below are not audiolaz's, or contain work that is not; each keeps its own licence.

| File | What | Licence |
| --- | --- | --- |
| `src/player/stb_image.h` | stb_image v2.30, Sean Barrett | MIT or public domain (its end) |
| `src/player/stb_image_write.h` | stb_image_write v1.16, Sean Barrett | MIT or public domain (its end) |
| `src/player/stb_truetype.h` | stb_truetype v1.26, Sean Barrett / RAD Game Tools | MIT or public domain (its end) |
| `src/player/cover_font.h` | Archivo (latin subset, weights 800 and 600), Copyright 2020 The Archivo Project Authors | SIL Open Font License 1.1 (its header) |
| `src/mac/FLAC/stream_decoder.h` | the names of libFLAC's stream decoder API, Xiph.Org, declared again (no libFLAC code) | BSD-3-Clause (libFLAC's) |
| `src/linux/kmod/` | the audiolaz sound card for Linux, built with the kernel's headers | GPL-2.0 (`COPYING`); its interface `azcard.h`, shared with the player, GPL-2.0 OR MIT |

Used at run time, not included: alsa-lib (LGPL-2.1), libFLAC (BSD-3-Clause, opened at run time on Linux when present),
and, when you install them as the player's inputs and outputs, shairport-sync, librespot, squeeze2raop (their own
licences).

`src/mac/flac.c` is audiolaz's own FLAC decoder, written from the format's specification (RFC 9639), MIT.
