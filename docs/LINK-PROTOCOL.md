# audiolaz Link — protocol

audiolaz Link carries a stereo PCM stream from a **sender** (the audiolaz player, `src/player/anet_send.c`) to a
**receiver** beside the DAC (`src/player/audiolaz-recv.c`: ALSA on Linux, Core Audio on macOS), bit for bit, the
DAC's clock pacing the sender. The definitions are in `src/player/anet.h`; this page is enough to write either side.

A receiver may also play into another player's input instead of a DAC (`audiolaz-recv --dac shm:<name>`: the ring of
`src/player/audiolaz_drv.h`, read by that player at its own output's pace). Its reports then carry that player's pace,
and that player's latency in their last word (`after`, below): a chain of players keeps one clock, the last DAC's.

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

   INFO: the receiver answers 192 bytes, `AZI2` then `id=…;dac=…;max=…;v=…;hw=…`, NUL-padded, and closes. Optional
   keys, each whole or absent (a reader skips the ones it does not know): `rates=44100,48000,…` the PCM rates its DAC
   takes as it is now (a rate it lists but would not go to left out once tried), `err=dac-start` while its DAC refuses
   to start, `room=<name>` the room a receiver that feeds another player serves. An INFO request may say who asks in
   its capabilities word: `0x555xxxxx` (an uplink: bit 0 its system output is its own « audiolaz » device, bit 1 that
   device is there); a receiver before ignores it.

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
- `CAP_RATES` (16): the sender converts rates itself: a rate the DAC does not take as it is is refused (reason 2)
  rather than resampled at the receiver; the sender asks INFO's `rates=` and sends at one of them. Not part of the
  protocol-3 handshake (a receiver that ignores it resamples as before).

A receiver whose DAC refuses to start stops the stream with reason 15 (« the DAC refuses to start: unplug it and plug
it in again ») and refuses new streams a while with reason 6, sooner once the device is seen plugged in again.

**The ring, sized by the network.** The receiver measures each packet's lateness (its one-way delay less the least of
the last 10 s), keeps a high quantile of each second's (0.999) and the largest of those over the last minutes, and asks
for a ring of that × 1.25 + 15 ms for a live source (× 2 + 30 ms for music read ahead), between 50 ms and 500 ms;
raised at once, lowered only by more than 10 % or 10 ms. The sender moves the ring there **without changing a single
sample**: music read ahead is sent a little faster (+25 % at most) or 1 % slower; a live source gets digital silence
added before its first frame or inside a silence, or zeros of a silence left out — never more than it put in.

Since 22/10 the buffers size themselves before they run short, never only after a gap. The receiver starts a stream on
its target (the jitter it remembers from that sender included) once its ring also holds what its DAC's buffer takes at
the start (a stream that follows the last one's tail at the same rate starts on its start, the DAC still playing that
tail); from 176.4 kHz up its start is half as long again; on a Mac it starts at 200 ms. It raises its target at
once (× 1.5, 20 ms more at least) when its ring falls under 30 % of its start while it plays, or runs dry, and adds what
its own DAC's buffer grew by; it lowers it only after long calm (twice its peaks' memory, 10 minutes by default), a tenth
at a time. Its DAC's buffer grows the same way (its lowest under 40 % of it: × 1.5, up to 500 ms), and while its
ring is dry it lets that buffer play on instead of writing silence before the music still to come. A live sender also
leaves out zeros of a silence when the ring stands far above what the receiver asks (40 ms, a fifth of it, averaged
over 2 s, from 3 s after the receiver plays), or adds silence when it stands as far below; a source read at the sender's pace (a sound card whose app follows
its clock) is pulled up to ±1000 ppm, any other ±300 ppm. Each gap is counted in the reports' underruns.

Reports grow to 80 bytes (magic `AZR3`): the 40 bytes above, then u32 target (frames), u32 late (µs, the last second's
quantile), u32 peak (µs), u32 underruns, u32 adjustments, u32 ring min, u32 ring max, u16 state (0 idle, 1 filling,
2 playing, 3 paused, 4 draining, 5 drained, 6 DAC refused, 7 DAC failed, 8 stopped), u16 the last stop's reason, u32 the
last ping's sequence number, u32 `after`: what is heard after the frames `played` counts, beyond the receiver's own DAC
buffer, µs (another player behind the receiver: its own latency; 0 for a receiver beside its DAC, and from every
receiver before it was defined). Never part of `played` (the sender's clock follows `played`); the sender counts it in
what it holds unheard.

`PING` (8-byte payload: u32 sequence, u32 the sender's state: 1 starting, 2 playing, 3 paused, 4 ending, or'ed with
0x100 for a live source) every 500 ms; the receiver answers each with a report at once. No report for 1 s: each ping is
a question; 5 s of questions unanswered: the receiver is lost. No ping nor packet for 10 s: the receiver ends the stream.

`META` (≤ 4096 bytes): `key=value` lines in UTF-8, `title`, `artist`, `album`, `cover` (an `http://` URL),
`volume_cdb` (the sender's volume, hundredths of a dB below full, 6000 and above silence: for a player behind the
receiver to apply after its own processing; a receiver beside its DAC applies none). Sent before the stream's first
packet, then when it changes.

Reasons (`STOP`, `END`, reports): 0 none, 1 the music's end, 2 stopped by the source, 3 another rate next, 4 settings
changed, 5 the source changed, 6 the other side silent, 7 taken by another sender, 8 the DAC failed, 9 the program ends,
10 paused too long, 11 another output, 12 closed without a word, 13 the same player again, 14 the receiver backed off.

**One DAC, one player.** The first player that plays keeps the DAC. Another one is refused (reason 5, busy) and told who
plays: 192 bytes after the report, `AZB1` then `by=<name>;player=<id>;for=<s>`, NUL-padded. The same player again (its
id) takes it back at once; `take=1` takes it over; a player silent for 5 s leaves it free.

## Compatibility

A version-2 receiver refuses any header version but 2 (which protocol 3 keeps) and ignores capabilities; a version-2
sender sends 0 there. Neither side sends anything of protocol 3 before both said they follow it.
