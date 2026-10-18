# audiolaz Link — protocol

audiolaz Link carries a stereo PCM stream from a **sender** (the audiolaz player, `src/player/anet_send.c`) to a
**receiver** beside the DAC (`src/player/audiolaz-recv.c`), bit for bit, the DAC's clock pacing the sender. The
definitions are in `src/player/anet.h`; this page is enough to write either side.

Everything is little-endian. Protocol 3 is described; it stays compatible with version 2 peers (below).

## Discovery

mDNS, service `_audiolaz._tcp.local` (`anet_mdns.c`): instance `<host>`, SRV its TCP port (default **4890**), TXT
`v=3 id=<host> dac=<the DAC's name> max=<highest rate, Hz> hw=<the DAC's identity, optional>`. Without mDNS, an INFO
request (a stream header with rate 0) on the TCP port answers the same.

## One stream

1. **TCP connection**, sender → receiver port P, one per stream. The sender writes the 32-byte header:

   | Offset | Type | |
   | --- | --- | --- |
   | 0 | u32 | magic `AZN1` (0x314E5A41) |
   | 4 | u32 | version: **2** (always) |
   | 8 | u32 | stream id |
   | 12 | u32 | rate, Hz (8000 … 1 536 000; 0: an INFO request) |
   | 16 | u16 | channels: 2 |
   | 18 | u16 | format: 1 S32, 2 S24 in S32, 3 DoP |
   | 20 | u32 | the sender's capabilities (protocol 3; 0 for a version-2 sender) |
   | 24 | u32 | frames per packet (nominal) |
   | 28 | u32 | packet interval, µs |

   INFO: the receiver answers 192 bytes, `AZI2` then `id=…;dac=…;max=…;v=…;hw=…`, NUL-padded, and closes.

2. **Data**: UDP datagrams, sender → receiver port P, each a 24-byte header then the frames:

   | Offset | Type | |
   | --- | --- | --- |
   | 0 | u32 | magic `AZD2` (0x32445A41) |
   | 4 | u32 | stream id |
   | 8 | u64 | index: the stream's frame number of the first frame |
   | 16 | u32 | frames (at most 176: 24 + 176 × 8 = 1432 bytes, one Ethernet frame, never fragmented) |
   | 20 | u32 | flags: 0; with `CAP_STAMP`, the time the packet was due to leave (µs, the sender's monotonic clock, mod 2³²) |
   | 24 | s32 × 2 × frames | the samples, left then right |

   Packets leave at a steady rhythm: one every 2 ms at most (176 frames at 88.2 kHz and above, fewer below), on the
   sender's own timer. A gap is played as zeros (counted); a late or repeated packet is dropped.

3. **Control messages**, sender → receiver on the TCP connection: u32 type, u32 length, then the payload:
   `END` (2: play out what is held), `STOP` (3: drop it now), `PAUSE` (4), `RESUME` (5), `PING` (6), `META` (7),
   `HELLO` (8). With protocol 3, `STOP` and `END` carry a u32 reason (below). A connection closed without `END` is a
   `STOP`.

4. **Reports**, receiver → sender on the TCP connection: one at once (accepted or refused), then every **50 ms**,
   timed by the DAC's own periods. Version 2, 40 bytes:

   | Offset | Type | |
   | --- | --- | --- |
   | 0 | u32 | magic `AZR1` (0x31525A41) |
   | 4 | u32 | stream id |
   | 8 | u32 | flags: 1 accepted, 2 refused, 4 playing, 8 drained, 16 paused, 32 failed, 64 protocol 3, 128 stopped by the receiver |
   | 12 | u32 | the DAC's rate |
   | 16 | u64 | played: the stream's frames heard (given to the DAC, less its delay) |
   | 24 | u64 | received: the highest index + frames |
   | 32 | u32 | ring: frames waiting in the receiver (the DAC's own buffer not counted) |
   | 36 | u32 | refused: why (1 header, 2 rate, 3 DAC, 4 taken, 5 busy); accepted: the ring's target (first report), then frames lost so far |

**The clock.** The sender keeps the receiver's ring at its target: the frames it puts in each packet follow what the
DAC really played (`played`, between reports followed by a small control loop), plus a gentle correction of the ring's
distance to its target. The receiver never resamples, and pads with zeros only on an underrun (counted).

## Protocol 3

Offered by the sender in the header's capabilities word, used only once the receiver's first report has flag 64:

- `CAP_STAMP` (1): each packet's flags word is the time it was due to leave.
- `CAP_ADAPT` (2): the sender follows the ring target the receiver asks for.
- `CAP_STATE` (4): both sides tell their state and why they stop.
- `CAP_WHO` (8): right after its header the sender sends `HELLO`, `player=<unique id>\nname=<name>\ntake=0|1\n`,
  padded with spaces to a multiple of 8 bytes (never a NUL).

**The ring, sized by the network.** The receiver measures each packet's lateness (its one-way delay less the least of
the last 10 s), keeps a high quantile of each second's (0.999) and the largest of those over the last minutes, and asks
for a ring of that × 1.25 + 15 ms for a live source (× 2 + 30 ms for music read ahead), between 50 ms and 500 ms;
raised at once, lowered only by more than 10 % or 10 ms. The sender moves the ring there **without changing a single
sample**: music read ahead is sent a little faster (+25 % at most) or 1 % slower; a live source gets digital silence
added before its first frame or inside a silence, or zeros of a silence left out — never more than it put in.

Reports grow to 80 bytes (magic `AZR3`): the 40 bytes above, then u32 target (frames), u32 late (µs, the last second's
quantile), u32 peak (µs), u32 underruns, u32 adjustments, u32 ring min, u32 ring max, u16 state (0 idle, 1 filling,
2 playing, 3 paused, 4 draining, 5 drained, 6 DAC refused, 7 DAC failed, 8 stopped), u16 the last stop's reason, u32 the
last ping's sequence number, u32 0.

`PING` (8-byte payload: u32 sequence, u32 the sender's state: 1 starting, 2 playing, 3 paused, 4 ending, or'ed with
0x100 for a live source) every 500 ms; the receiver answers each with a report at once. No report for 1 s: each ping is
a question; 5 s of questions unanswered: the receiver is lost. No ping nor packet for 10 s: the receiver ends the stream.

`META` (≤ 4096 bytes): `key=value` lines in UTF-8, `title`, `artist`, `album`, `cover` (an `http://` URL).

Reasons (`STOP`, `END`, reports): 0 none, 1 the music's end, 2 stopped by the source, 3 another rate next, 4 settings
changed, 5 the source changed, 6 the other side silent, 7 taken by another sender, 8 the DAC failed, 9 the program ends,
10 paused too long, 11 another output, 12 closed without a word, 13 the same player again, 14 the receiver backed off.

**One DAC, one player.** The first player that plays keeps the DAC. Another one is refused (reason 5, busy) and told who
plays: 192 bytes after the report, `AZB1` then `by=<name>;player=<id>;for=<s>`, NUL-padded. The same player again (its
id) takes it back at once; `take=1` takes it over; a player silent for 5 s leaves it free.

## Compatibility

A version-2 receiver refuses any header version but 2 (which protocol 3 keeps) and ignores capabilities; a version-2
sender sends 0 there. Neither side sends anything of protocol 3 before both said they follow it.
