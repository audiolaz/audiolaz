# Bit-perfect, verified

The player does not only promise to leave the music alone: it checks it, all the time it plays
(`src/player/bitperfect.h`).

## How

- **In**: every frame the source delivers (its first two channels as they came, 32-bit words; a mono source on both)
  is fingerprinted together with its place in the music *m* (frames since the play's start): *f(m, L, R)*, a 64-bit
  mix (splitmix64), summed over spans of 4096 frames. The reading thread puts each span's sum in a small queue.
- **Out**: the writer, as it takes frames out of the player's ring for the output, computes the same sums from the
  frames it actually hands over (their place in the ring, less the play's offset), and compares span by span.
- A sum is tied to each frame's place: a sample changed by one bit, a frame lost, added or moved shows as a span that
  differs (`tests/bitperfect.c` checks each case).
- Nothing waits and nothing is allocated on the way; it costs about 1.6 ms per second of 192 kHz stereo on a desktop
  processor (0.16 % of a core).

## What it says

`GET /state`:

| Key | |
| --- | --- |
| `bitPerfect` | `true`: spans were compared and every one was identical, nothing changed on purpose; `false`: see `bitPerfectWhy`; `null`: not known yet (not playing, no span compared yet) |
| `bitPerfectWhy` | the reasons, any of: `channels` (a source of more than two channels to a stereo output: its first two), `bits` (requantized for an output that keeps fewer bits than the source has), `rate` (a rate the device refused, converted by the output layer; `soundPath.deviceRate` says which), `output` (a device that converts by itself: AirPlay is 16 bits at 44.1 kHz), `dropped` (frames lost: the output stalled), `changed` (spans that differ), and in builds that process the music on purpose `correction`, `level`, `volume`, `upsampling`, `dsd`, `limiter` |
| `bitPerfectSpans` | how many spans were found identical this play |
| `soundPath` | `input`, `inRate`, `inBits` (the source's significant bits seen), `channels`, `outRate`, `outBits` (what the output keeps), `deviceRate` (when the device plays at another rate), `dac` |

What it cannot see beyond the writer is the output's own path: a sound card's driver, an ALSA `plug` device (the
player reads its description and reports a rate conversion), a network receiver. audiolaz Link's receiver plays what
it receives as it came, and the transfer itself is checked bit for bit by `tests/anet-loop.sh`.
