#!/usr/bin/env python3
"""A fake Roon for the audiolaz player's tests: enough SlimProto (TCP 3483: HELO, strm, STAT...) and HTTP to drive one
Squeezebox player through tracks of plain PCM (24-bit, stereo, little endian, as Roon sends with FLAC off) or FLAC,
served by a simulated source (a prebuffer, then a download pace). It reports, per track, what was served, when the
player said STMs, STMd and STMu, and the elapsed time it reported. No dependencies. It listens on 3483: run it in a
network namespace of its own (tests/player-intact.sh does), never beside a real Roon.

  python3 tests/fake-roon-squeeze.py [--seconds 60] [--tracks 2] [--rate 44100] [--prebuffer 10] [--speed 3] [--flac]
        [--gapless] [--json out.json] [--quiet] [--timeout S]
then start the player against 127.0.0.1: audiolaz-player --loop none --dac <device> --rooms <folder> --squeeze 127.0.0.1
"""
import argparse, json, math, os, random, shutil, socket, struct, subprocess, sys, tempfile, threading, time, wave

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--bind', default='127.0.0.1')
ap.add_argument('--slim-port', type=int, default=3483)
ap.add_argument('--http-port', type=int, default=9000)
ap.add_argument('--seconds', type=float, default=60, help='each track\'s length')
ap.add_argument('--tracks', type=int, default=2)
ap.add_argument('--rate', type=int, default=44100)
ap.add_argument('--prebuffer', type=float, default=10, help='seconds of the track ready at once')
ap.add_argument('--speed', type=float, default=3, help='then produced at this many times real time')
ap.add_argument('--capacity', type=float, default=0, help='the prebuffer holds this many seconds at most (0: unbounded)')
ap.add_argument('--stall-every', type=float, default=0, help='the download pauses every this many seconds (0: never)')
ap.add_argument('--stall-for', type=float, default=0, help='for this long')
ap.add_argument('--grace', type=float, default=0, help='seconds the sender waits for the download before closing (0: Roon\'s immediate « short read »)')
ap.add_argument('--flac', action='store_true', help='serve FLAC (format f, as Roon does with FLAC on), encoded here by afconvert or flac')
ap.add_argument('--formats', default='', help='per-track formats, comma separated, cycled: f (FLAC) or p (PCM); overrides --flac')
ap.add_argument('--heartbeat', type=float, default=1.0, help='strm t every this many seconds')
ap.add_argument('--elapsed-tolerance', type=float, default=0.75, help='--gapless: a stream\'s last elapsed time this close '
                'to its length at most')
ap.add_argument('--sndbuf', type=int, default=65536, help='the server socket\'s send buffer (bytes)')
ap.add_argument('--timeout', type=float, default=0, help='give up after this many seconds (0: tracks × seconds + 40)')
ap.add_argument('--json', help='write the results there too')
ap.add_argument('--rate2', type=int, default=0, help='tracks 2 and after at this rate')
ap.add_argument('--rates', default='', help='per-track rates, comma separated, cycled (overrides --rate and --rate2)')
ap.add_argument('--pause-at', type=float, default=0, help='strm p this many s after track 1 STMs')
ap.add_argument('--pause-for', type=float, default=0, help='then strm u after this long')
ap.add_argument('--resume-after-stmp', type=float, default=0, help='the player paused itself (STMp unasked: another of its '
                'inputs started, 08/10): strm u this many s later, as a listener pressing play in Roon (0: never)')
ap.add_argument('--skip-at', type=float, default=0, help='strm q then strm s track 2, this many s after track 1 STMs')
ap.add_argument('--quiet', action='store_true', help='no STMt heartbeat lines')
ap.add_argument('--tone', type=float, default=0, help='track 1\'s tone in Hz, a whole number (default 440; each next '
                'track 1.25 times the one before), at -20 dBFS (10/10, tests/mac-roomd-out.py: 1 kHz resampled)')
ap.add_argument('--gapless', action='store_true', help='tracks of the same rate in one HTTP stream, the zone following the '
                'player\'s elapsed time, one track ahead at most (the real Roon)')
ap.add_argument('--stuck-after', type=float, default=30, help='--gapless: STUCK once the stream has waited this long for a '
                'zone that does not move')
ap.add_argument('--stuck-linger', type=float, default=15, help='--gapless: then the results, this many s later')
A = ap.parse_args()

RATE_CODES = {11025: '0', 22050: '1', 32000: '2', 44100: '3', 48000: '4', 96000: '9', 88200: ':', 176400: ';', 192000: '<'}
CH, BYTES = 2, 3
FRAME = CH * BYTES
BPS = A.rate * FRAME                        # bytes per second of music
T0 = time.monotonic()
lock = threading.Lock()
events = []                                 # (t, text)
tracks = {}                                 # n → its record
done = threading.Event()
player = {'sock': None}


def now():
    return time.monotonic() - T0


def log(msg):
    t = now()
    with lock:
        events.append((round(t, 3), msg))
    print(f'{t:8.3f}  {msg}', flush=True)


def track(n):
    with lock:
        return tracks.setdefault(n, {'track': n, 'seconds': SECS.get(n, A.seconds), 'strm_at': None, 'http_at': None, 'served_s': 0.0,
                                     'closed_at': None, 'close': None, 'stmd_at': None, 'stms_at': None, 'requests': 0,
                                     'max_lead_s': 0.0, 'max_player_lead_s': 0.0, 'rx_at_start': None, 'zone_at': None})


# ── The music: a tone per track, one second precomputed (a whole number of cycles: it loops seamlessly) ──
RATES = [int(x) for x in A.rates.split(',')] if A.rates else []


def rate_of(n):
    if RATES:
        return RATES[(n - 1) % len(RATES)]
    return A.rate2 if (A.rate2 and n >= 2) else A.rate


FORMATS = [x.strip() for x in A.formats.split(',')] if A.formats else []


def fmt_of(n):
    """'f' (FLAC) or 'p' (PCM) for track n."""
    if FORMATS:
        return FORMATS[(n - 1) % len(FORMATS)]
    return 'f' if A.flac else 'p'


def second_of(n):
    R = rate_of(n)
    f = (A.tone or 440.0) * (1.25 ** (n - 1))
    amp = 0.1 * (1 << 23)
    out = bytearray(R * FRAME)
    for i in range(R):
        v = int(round(amp * math.sin(2 * math.pi * f * i / R))) & 0xffffff
        b = bytes((v & 0xff, v >> 8 & 0xff, v >> 16 & 0xff))
        out[i * FRAME:i * FRAME + 3] = b
        out[i * FRAME + 3:i * FRAME + 6] = b
    return bytes(out)


SECONDS = {}
WIRE = {}                                   # n → (bytes on the wire per second of music, the whole track's bytes or None for PCM)
SECS = {}                                   # n → the track's length in seconds (--gapless FLAC: whole 4096-frame blocks)
SECS_FRAMES = {}                            # n → the same in frames
BLOCK = 4096


def same_run(n, m):
    """--gapless: tracks n and m (n < m) go in one HTTP stream: every track from n to m at the same rate and format."""
    return A.gapless and all(rate_of(k) == rate_of(n) and fmt_of(k) == fmt_of(n) for k in range(n, m + 1))


def run_tracks():
    """The runs of tracks sharing one stream: [[1, 2, 3], [4]…] (one track each without --gapless)."""
    runs = []
    for n in range(1, A.tracks + 1):
        if runs and same_run(runs[-1][0], n):
            runs[-1].append(n)
        else:
            runs.append([n])
    return runs


def crc8(b):
    c = 0
    for x in b:
        c ^= x
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xff if c & 0x80 else (c << 1) & 0xff
    return c


def flac_frame_at(data, start, number):
    """Where the FLAC frame of this number (fixed block size) begins, from `start` on: its sync, its header's CRC-8 and
    its number checked (a frame's own bytes may hold the sync pattern by chance, never all three)."""
    i = start
    while True:
        i = data.find(b'\xff\xf8', i)
        if i < 0 or i + 16 > len(data):
            raise ValueError(f'FLAC frame {number} not found')
        b0, j = data[i + 4], i + 5
        if b0 < 0x80:
            v, extra = b0, 0
        elif b0 & 0xe0 == 0xc0:
            v, extra = b0 & 0x1f, 1
        elif b0 & 0xf0 == 0xe0:
            v, extra = b0 & 0x0f, 2
        elif b0 & 0xf8 == 0xf0:
            v, extra = b0 & 0x07, 3
        elif b0 & 0xfc == 0xf8:
            v, extra = b0 & 0x03, 4
        elif b0 & 0xfe == 0xfc:
            v, extra = b0 & 0x01, 5
        else:
            i += 1; continue
        ok = True
        for _ in range(extra):
            if data[j] & 0xc0 != 0x80:
                ok = False; break
            v = v << 6 | (data[j] & 0x3f); j += 1
        bs, sr = data[i + 2] >> 4, data[i + 2] & 0x0f
        j += 1 if bs == 6 else 2 if bs == 7 else 0
        j += 1 if sr == 12 else 2 if sr in (13, 14) else 0
        if ok and v == number and crc8(data[i:j]) == data[j]:
            return i
        i += 1


def flac_run(run):
    """--gapless --flac: the run's tracks as ONE FLAC stream (one encoder, its header once, no length in it: as Roon's
    live encoder), cut where each track's first block begins: the first track's bytes carry the header."""
    R = rate_of(run[0])
    pcm = bytearray()
    blocks = []                                 # each track's first block number
    for n in run:
        frames = SECS_FRAMES[n]
        blocks.append(len(pcm) // FRAME // BLOCK)
        one, rnd = bytearray(second_of(n)), random.Random(n)
        for i in range(0, R * CH):              # the tone with noise: a music-like bit rate, one second repeated
            v = int.from_bytes(one[3 * i:3 * i + 3], 'little', signed=True) + rnd.randint(-20000, 20000)
            one[3 * i:3 * i + 3] = max(-(1 << 23), min((1 << 23) - 1, v)).to_bytes(3, 'little', signed=True)
        whole, rest = divmod(frames, R)
        pcm += bytes(one) * whole + bytes(one[:rest * FRAME])
    if shutil.which('flac'):
        cmd = ['flac', '-s', '-c', '--force-raw-format', '--endian=little', '--sign=signed', '--channels=2', '--bps=24',
               f'--sample-rate={R}', f'--blocksize={BLOCK}', '-']
    else:                                       # a Mac (10/10): no flac there; ffmpeg's encoder, to a pipe as Roon's
        cmd = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 's24le', '-ar', str(R), '-ac', '2', '-i', '-',
               '-c:a', 'flac', '-sample_fmt', 's32', '-frame_size', str(BLOCK), '-f', 'flac', '-']
    out = subprocess.run(cmd, input=bytes(pcm), stdout=subprocess.PIPE, check=True).stdout
    cuts = [0] + [flac_frame_at(out, 4, b) for b in blocks[1:]] + [len(out)]
    return {n: out[cuts[k]:cuts[k + 1]] for k, n in enumerate(run)}


# ── --gapless: Roon's zone, from the player's elapsed time ──
zone = {'first': None, 'track': None, 'last_el': None, 'el_since': None, 'frozen': False}


def paused_now():
    return 'pause_sent' in state and ('resume_sent' not in state or now() < state['resume_sent'] + 3)


def zone_follow(el_ms):
    """Which track Roon's zone shows, from the elapsed time (ms since the stream's STMs) the player reports."""
    if zone['first'] is None:
        return
    pos, n, start = el_ms / 1000, zone['first'], 0.0
    while n < A.tracks and same_run(zone['first'], n + 1) and pos >= start + SECS[n]:
        start += SECS[n]; n += 1
    if n != zone['track']:
        zone['track'] = n
        track(n)['zone_at'] = now()
        log(f'zone: track {n} now (elapsed {pos:.2f} s from the stream\'s start, {pos - start:.2f} s into it)')
    if el_ms != zone['last_el']:
        zone['last_el'], zone['el_since'], zone['frozen'] = el_ms, now(), False
    elif not zone['frozen'] and not paused_now() and now() - zone['el_since'] > 5:
        zone['frozen'] = True
        log(f'the player\'s elapsed time has not moved for 5 s: {pos:.2f} s (the zone stays on track {n})')


def flac_of(n):
    """The track as FLAC: the tone with some noise (a music-like bit rate, not a tone's few bytes), encoded here."""
    d = tempfile.mkdtemp(prefix='fake-roon-')
    wav, fl = os.path.join(d, f'{n}.wav'), os.path.join(d, f'{n}.flac')
    one, rnd = second_of(n), random.Random(n)
    with wave.open(wav, 'wb') as w:
        R = rate_of(n)
        w.setnchannels(CH); w.setsampwidth(BYTES); w.setframerate(R)
        for sec in range(int(math.ceil(A.seconds))):
            frames = min(R, int(A.seconds * R) - sec * R)
            b = bytearray(one[:frames * FRAME])
            for i in range(0, frames * CH):
                v = int.from_bytes(b[3 * i:3 * i + 3], 'little', signed=True) + rnd.randint(-20000, 20000)
                b[3 * i:3 * i + 3] = max(-(1 << 23), min((1 << 23) - 1, v)).to_bytes(3, 'little', signed=True)
            w.writeframes(bytes(b))
    if shutil.which('afconvert'):
        subprocess.run(['afconvert', '-f', 'flac', '-d', 'flac', wav, fl], check=True)
    else:
        subprocess.run(['flac', '-s', '-f', '-o', fl, wav], check=True)
    data = open(fl, 'rb').read()
    shutil.rmtree(d, ignore_errors=True)
    return data


# ── SlimProto, server → player: 2-byte length, opcode, payload ──
def send_cmd(op, payload):
    s = player['sock']
    if not s:
        return
    data = struct.pack('>H', 4 + len(payload)) + op.encode() + payload
    try:
        s.sendall(data)
    except OSError:
        pass


def strm(cmd, autostart=b'1', fmt=b'p', ts=0, req=b'', rate=None):
    port = A.http_port if cmd == b's' else 0
    if fmt == b'f':
        p = cmd + autostart + fmt + b'????'                                         # FLAC: its own header says
    else:
        p = cmd + autostart + fmt + b'2' + RATE_CODES[rate or A.rate].encode() + b'2' + b'1'   # 24-bit, the rate, stereo, little endian
    p += bytes((255, 0, 0)) + b'0' + bytes((0, 0, 0)) + struct.pack('>I', ts) + struct.pack('>H', port) + struct.pack('>I', 0)
    send_cmd('strm', p + req)


def start_track(n):
    t = track(n)
    t['strm_at'] = now()
    m = n
    while m < A.tracks and same_run(n, m + 1):
        m += 1
    log(f'← strm s track {n} ({"FLAC" if fmt_of(n) == "f" else "PCM"} 24-bit {rate_of(n)} Hz, {SECS.get(n, A.seconds):g} s)'
        + (f', tracks {n} to {m} to follow in the same stream' if m > n else ''))
    if A.gapless:
        state['pending'] = (n, m)               # the zone moves to it at its STMs: the previous stream plays until then
        state['next'] = m + 1                   # the next strm s: after this stream's STMd
    strm(b's', fmt=b'f' if fmt_of(n) == 'f' else b'p', rate=rate_of(n), req=f'GET /stream.pcm?track={n} HTTP/1.0\r\nHost: {A.bind}:{A.http_port}\r\n\r\n'.encode())


def parse_stat(d):
    ev = d[0:4].decode('ascii', 'replace')
    buf_size, fullness = struct.unpack('>II', d[7:15])
    rx = struct.unpack('>Q', d[15:23])[0]
    elapsed_ms = struct.unpack('>I', d[43:47])[0] if len(d) >= 47 else 0   # after the jiffies, output buffer, seconds, voltage
    return ev, buf_size, fullness, rx, elapsed_ms


state = {'next': 1, 'playing': None, 'stms': 0, 'last_elapsed': 0}
streams = []                                # --gapless: each stream once its STMs came (tracks, length, last elapsed)


def on_stat(d):
    ev, size, full, rx, el = parse_stat(d)
    state['last_elapsed'] = el
    cur = tracks.get(state['stms']) if not A.gapless else None
    if A.gapless and ev == 'STMs' and state.get('pending'):
        first, last = state.pop('pending')
        zone.update(first=first, track=None, last_el=None, el_since=None, frozen=False)
        streams.append({'first': first, 'last': last, 'length': sum(SECS[k] for k in range(first, last + 1)), 'stms_at': now(),
                        'max_el': 0.0})
    if A.gapless and streams and ev != 'STMs':
        streams[-1]['max_el'] = max(streams[-1]['max_el'], el / 1000)
    if A.gapless and ev in ('STMt', 'STMs'):
        zone_follow(0 if ev == 'STMs' else el)
    if ev == 'STMt' and cur is not None and cur['stms_at'] is not None and el < 1e6:
        # how far the player has read ahead of what it played: served minus the elapsed it reports
        cur['max_lead_s'] = round(max(cur['max_lead_s'], cur['served_s'] - el / 1000), 2)
        if cur['rx_at_start'] is not None:          # what the player itself holds: received (not in the sockets) minus played
            # this track's bytes only (the next track's stream may already be open), at this track's own wire rate
            mine = min(rx - cur['rx_at_start'], WIRE[state['stms']][0] * cur['seconds'])
            cur['max_player_lead_s'] = round(max(cur['max_player_lead_s'], mine / WIRE[state['stms']][0] - el / 1000), 2)
    if ev == 'STMt':
        if not A.quiet and int(now()) % 10 == 0:
            served = cur['served_s'] if cur else 0
            log(f'  STMt  track {zone["track"] if A.gapless else state["stms"]} elapsed {el / 1000:7.2f} s, served {served:.1f} s; buffer {size} B, fullness {full} B '
                f'({full / 8 / rate_of(max(1, state["next"] - 1)):.1f} s), received {rx / WIRE[1][0]:.1f} s (at track 1\'s rate)')
        return
    log(f'→ STAT {ev}  buffer size {size} B ({size / 8 / rate_of(max(1, state["next"] - 1)):.1f} s at 32-bit frames of the last track asked), fullness {full} B, '
        f'received {rx / WIRE[1][0]:.1f} s (at track 1\'s rate), elapsed {el / 1000:.2f} s')
    if ev == 'STMc' and state['next'] - 1 in tracks and tracks[state['next'] - 1]['rx_at_start'] is None:
        tracks[state['next'] - 1]['rx_at_start'] = rx    # the player's bytes so far: this track's are counted from here
    if ev == 'STMs':
        state['stms'] += 1
        n = state['stms']
        if A.gapless:
            track(zone['first'])['stms_at'] = now()
        elif n in tracks:
            tracks[n]['stms_at'] = now()
        if n == 1 and A.pause_at > 0:
            threading.Timer(A.pause_at, do_pause).start()
        if n == 1 and A.skip_at > 0:
            threading.Timer(A.skip_at, do_skip).start()
    elif ev == 'STMd':
        n = state['next'] - 1
        if n >= 1:
            t = track(n)
            if t['stmd_at'] is None:
                t['stmd_at'] = now()
        if state['next'] <= A.tracks:
            n = state['next']
            state['next'] += 1
            start_track(n)
    elif ev == 'STMp':
        state['stmp_at'] = now()
        if A.resume_after_stmp > 0 and not state.get('pause_sent'):   # not asked by us: another input took the player
            def resume_other():
                state['resume_sent'] = now()
                log('← strm u (play again in Roon, after the player paused for another input)')
                strm(b'u', autostart=b'0', fmt=b'?')
            threading.Timer(A.resume_after_stmp, resume_other).start()
    elif ev == 'STMr':
        state['stmr_at'] = now()
    elif ev == 'STMf':
        state['stmf_at'] = now()
    elif ev == 'STMu':
        state['stmu_at'] = now()
        log('player says: played to the end (STMu)')
        threading.Timer(1.0, done.set).start()


def do_pause():
    state['pause_sent'] = now()
    log('← strm p (pause)')
    strm(b'p', autostart=b'0', fmt=b'?')
    def resume():
        state['resume_sent'] = now()
        log('← strm u (resume)')
        strm(b'u', autostart=b'0', fmt=b'?')
    threading.Timer(A.pause_for, resume).start()


def do_skip():
    state['skip_sent'] = now()
    log('← strm q (skip: flush)')
    strm(b'q', autostart=b'0', fmt=b'?')
    time.sleep(0.3)
    state['next'] = 3
    start_track(2)


def slim_conn(c):
    player['sock'] = c
    def readn(n):
        b = b''
        while len(b) < n:
            r = c.recv(n - len(b))
            if not r:
                raise EOFError
            b += r
        return b
    try:
        while not done.is_set():
            h = readn(8)
            op, n = h[:4].decode('ascii', 'replace'), struct.unpack('>I', h[4:])[0]
            d = readn(n) if n else b''
            if op == 'HELO':
                caps = d[36:].decode('utf-8', 'replace') if len(d) > 36 else ''
                log(f'→ HELO ({caps})')
                if state['next'] == 1:
                    state['next'] = 2
                    start_track(1)
            elif op == 'STAT':
                on_stat(d)
            elif op == 'RESP':
                log(f'→ RESP ({len(d)} bytes of headers)')
            elif op == 'DSCO':
                log('→ DSCO (the player saw the stream close)')
            elif op == 'SETD':
                pass
            else:
                log(f'→ {op} ({n} bytes)')
    except (EOFError, OSError):
        log('player disconnected')
    player['sock'] = None


def heartbeat():
    while not done.is_set():
        time.sleep(A.heartbeat)
        strm(b't', autostart=b'0', fmt=b'?', ts=int(now() * 1000) & 0xffffffff)
        last = tracks.get(A.tracks)
        began = last and (last['zone_at'] if A.gapless else last['stms_at'])
        if began is not None and last['close'] and now() > began + last['served_s'] + 8:
            log('the last track should have played out 8 s ago and no STMu came: stopping here')
            done.set()
        w = state.get('gate_since')
        if A.gapless and w is not None and now() - w > A.stuck_after and not state.get('stuck_at'):
            state['stuck_at'] = now()
            log(f'STUCK: the stream has waited {now() - w:.0f} s for the zone to reach track {state["gate_for"]}; the zone shows '
                f'track {zone["track"]}, the player\'s elapsed time {(zone["last_el"] or 0) / 1000:.2f} s '
                f'({"frozen" if zone["frozen"] else "moving"}); nothing more will come (Roon, 06/10 22:26-22:29)')
            threading.Timer(A.stuck_linger, done.set).start()


# ── HTTP: the track from the simulated prebuffer ──
class Source:
    """Roon's prebuffer of one track: --prebuffer seconds ready at once, then downloaded at --speed × real time, into a
    prebuffer of --capacity seconds at most (0: unbounded), the download pausing --stall-for s every --stall-every s."""
    def __init__(self, total, t0, bps, align):
        self.total, self.t0, self.t, self.bps, self.align = total, t0, t0, bps, align
        self.downloaded = self.level = min(total, int(A.prebuffer * bps) // align * align)
        self.cap = int(A.capacity * bps) // align * align if A.capacity > 0 else None
    def _advance(self):
        tn = time.monotonic()
        dt, self.t = tn - self.t, tn
        el = tn - self.t0
        if A.stall_every > 0 and el >= A.stall_every and (el % A.stall_every) < A.stall_for:
            return                                  # the download stalls
        add = int(A.speed * self.bps * dt)
        if self.cap is not None:
            add = min(add, self.cap - self.level)
        add = max(0, min(add, self.total - self.downloaded))
        self.downloaded += add
        self.level += add
    def take_ready(self):
        self._advance()
        return self.level - self.level % self.align if self.downloaded < self.total else self.level
    def took(self, k):
        self.level -= k


def describe_source():
    d = f'{A.prebuffer:g} s ready, then {A.speed:g}× real time'
    if A.capacity > 0:
        d += f', prebuffer {A.capacity:g} s at most'
    if A.stall_every > 0 and A.stall_for > 0:
        d += f', download stalls {A.stall_for:g} s every {A.stall_every:g} s'
    return d + (f', waits {A.grace:g} s for the download before closing' if A.grace > 0 else ', closes at once when dry')


def http_conn(c):
    req = b''
    while b'\r\n\r\n' not in req:
        r = c.recv(4096)
        if not r:
            c.close(); return
        req += r
    line = req.split(b'\r\n', 1)[0].decode('ascii', 'replace')
    n = int(line.split('track=')[1].split()[0].split('&')[0]) if 'track=' in line else 1
    t = track(n)
    t['requests'] += 1
    t0 = time.monotonic()
    t['http_at'] = now()
    log(f'HTTP {line}  (source: {describe_source()})')
    c.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, A.sndbuf)
    c.sendall(b'HTTP/1.0 200 OK\r\nServer: fake-roon\r\nContent-Type: audio/L24\r\nConnection: close\r\n\r\n')
    while True:
        sent, reason, bps = send_track(c, n, t, t0)
        t['served_s'] = round(sent / bps, 3)
        t['closed_at'] = now()
        nxt = n + 1
        if reason == 'end' and nxt <= A.tracks and same_run(n, nxt) and not done.is_set():
            # Roon: the next track of the same format into the same stream, once its zone shows this one (one ahead at most)
            t['close'] = 'the next track follows in the stream'
            log(f'HTTP track {n}: all {sent / bps:.2f} s sent in {time.monotonic() - t0:.2f} s; track {nxt} follows in the same stream')
            if not (zone['track'] is not None and zone['track'] >= n):
                state['gate_since'], state['gate_for'] = now(), n
                log(f'HTTP track {nxt} waits: Roon buffers one track ahead of its zone, which shows track {zone["track"]} '
                    f'(the player\'s elapsed time {(zone["last_el"] or 0) / 1000:.2f} s); the stream stays open, nothing sent')
                while not done.is_set() and not (zone['track'] is not None and zone['track'] >= n):
                    time.sleep(0.05)
                if done.is_set():
                    reason = 'stuck: the zone never reached the track before'
                    break
                log(f'HTTP track {nxt}: the zone reached track {n} after {now() - state["gate_since"]:.1f} s: sent now')
                state['gate_since'] = None
            n, t0 = nxt, time.monotonic()
            t = track(n)
            t['requests'] += 1
            t['http_at'] = now()
            continue
        break
    t['close'] = reason
    log(f'HTTP track {n} closed: {reason}, {t["served_s"]:.2f} s of {t["seconds"]:g} s served in {time.monotonic() - t0:.2f} s')
    try:
        c.shutdown(socket.SHUT_WR)
    except OSError:
        pass
    time.sleep(0.5)
    c.close()


def send_track(c, n, t, t0):
    """Track n's bytes from its simulated prebuffer, into the open stream: (bytes sent, why it stopped, wire bytes/s)."""
    bps, data = WIRE[n]
    total = len(data) if data is not None else SECS_FRAMES[n] * FRAME
    align = 1 if data is not None else FRAME
    one = SECONDS.setdefault(n, second_of(n))
    two = one + one
    src = Source(total, t0, bps, align)
    sent, reason = 0, None
    try:
        while True:
            if sent >= total:
                reason = 'end'; break
            if done.is_set():
                reason = 'the harness stopped'; break
            avail = src.take_ready()
            if avail <= 0:
                # (after --grace seconds of waiting for the download, 0 by default)
                end = time.monotonic() + A.grace
                while avail <= 0 and time.monotonic() < end:
                    time.sleep(0.005)
                    avail = src.take_ready()
                if avail <= 0:
                    reason = 'prebuffer dry'; break
            k = min(avail, 65536 - 65536 % align)          # whole frames: the end lands exactly on the track's end
            if data is not None:
                c.sendall(data[sent:sent + k])
            else:
                at = sent % len(one)
                c.sendall(two[at:at + k])
            src.took(k)
            sent += k
            t['served_s'] = round(sent / bps, 3)
    except OSError as e:
        reason = f'player closed ({e.__class__.__name__})'
    return sent, reason, bps


def serve(port, handler):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((A.bind, port))
    s.listen(4)
    def loop():
        while not done.is_set():
            try:
                c, _ = s.accept()
            except OSError:
                return
            threading.Thread(target=handler, args=(c,), daemon=True).start()
    threading.Thread(target=loop, daemon=True).start()
    return s


def main():
    for n in range(1, A.tracks + 1):
        R = rate_of(n)
        SECS_FRAMES[n] = round(A.seconds * R / BLOCK) * BLOCK if A.gapless and fmt_of(n) == 'f' else int(A.seconds * R)
        SECS[n] = SECS_FRAMES[n] / R
    for n in range(1, A.tracks + 1):
        SECONDS[n] = second_of(n)
        if fmt_of(n) == 'f' and not A.gapless:
            data = flac_of(n)
            WIRE[n] = (len(data) / A.seconds, data)
            log(f'track {n}: FLAC {len(data)} bytes, {len(data) / A.seconds / 1000:.0f} kB/s')
        elif fmt_of(n) == 'p':
            WIRE[n] = (rate_of(n) * FRAME, None)
    if A.gapless:
        for run in (r for r in run_tracks() if fmt_of(r[0]) == 'f'):
            for n, data in flac_run(run).items():
                WIRE[n] = (len(data) / SECS[n], data)
                log(f'track {n}: FLAC {len(data)} bytes, {len(data) / SECS[n] / 1000:.0f} kB/s, {SECS[n]:.3f} s'
                    + (' (the stream\'s header in it)' if n == run[0] else f' (frames only: it follows track {n - 1} in the stream)'))
    serve(A.http_port, http_conn)
    serve(A.slim_port, slim_conn)
    threading.Thread(target=heartbeat, daemon=True).start()
    log(f'fake Roon on {A.bind}:{A.slim_port} (SlimProto) and :{A.http_port} (HTTP): {A.tracks} tracks of {A.seconds:g} s, '
        f'source: {describe_source()}')
    limit = A.timeout or A.tracks * A.seconds + 40 + A.pause_for
    done.wait(limit)
    if A.gapless:
        return gapless_results(limit)
    if not done.is_set():
        log(f'timeout ({limit:g} s)')
    done.set()
    rows = []
    prev = None
    for n in range(1, A.tracks + 1):
        t = track(n)
        r = dict(t)
        s0 = t['stms_at']
        r['stmd_after_start_s'] = round(t['stmd_at'] - s0, 2) if t['stmd_at'] is not None and s0 is not None else None
        r['played_s'] = None
        if prev is not None and prev['stms_at'] is not None and s0 is not None:
            prev_row = rows[-1]
            prev_row['played_s'] = round(s0 - prev['stms_at'], 2)
            extra = A.pause_for if prev['track'] == 1 else 0
            prev_row['gapless_next'] = abs(s0 - prev['stms_at'] - prev['seconds'] - extra) < 0.5
        rows.append(r)
        prev = t
    if rows and rows[-1]['stms_at'] is not None and state.get('stmu_at'):
        rows[-1]['played_s'] = round(state['stmu_at'] - rows[-1]['stms_at'], 2)   # includes the output's tail (~2 s)
    print('\nRESULTS', flush=True)
    for r in rows:
        print(f"  track {r['track']}: served {r['served_s']:.2f} s of {r['seconds']:g} s, close: {r['close']}, "
              f"STMs at {r['stms_at'] if r['stms_at'] is None else round(r['stms_at'], 2)} s, "
              f"STMd {r['stmd_after_start_s']} s after its STMs, played {r['played_s']} s, read ahead at most {r['max_lead_s']} s (in the player itself {r['max_player_lead_s']} s, the rest in the sockets)"
              + (f", next track gapless: {r['gapless_next']}" if 'gapless_next' in r else ''), flush=True)
    print(f"  STMu at {round(state['stmu_at'], 2) if state.get('stmu_at') else None} s", flush=True)
    ctl = {k: round(v, 3) for k, v in state.items() if k.endswith(('_sent', '_at')) and isinstance(v, float)}
    print(f'  control: {ctl}', flush=True)
    if A.json:
        with open(A.json, 'w') as f:
            json.dump({'args': vars(A), 'tracks': rows, 'stmu_at': state.get('stmu_at'), 'state': {k: v for k, v in state.items()}, 'events': events}, f, indent=1)


def gapless_results(limit):
    if not done.is_set():
        log(f'timeout ({limit:g} s)')
    done.set()
    print('\nRESULTS (--gapless: one stream per run of tracks at the same rate, the zone from the player\'s elapsed time)', flush=True)
    rows = []
    for n in range(1, A.tracks + 1):
        t = dict(track(n))
        nz = track(n + 1)['zone_at'] if n < A.tracks else None
        t['zone_s'] = round(nz - t['zone_at'], 2) if nz is not None and t['zone_at'] is not None else None
        rows.append(t)
        print(f"  track {n}: served {t['served_s']:.2f} s of {t['seconds']:.2f} s ({t['close']}); the zone showed it "
              f"{'from ' + str(round(t['zone_at'], 2)) + ' s' if t['zone_at'] is not None else 'NEVER'}"
              + (f", for {t['zone_s']} s" if t['zone_s'] is not None else '')
              + (f"; STMd {round(t['stmd_at'], 2)} s" if t['stmd_at'] is not None else ''), flush=True)
    reached = sum(1 for r in rows if r['zone_at'] is not None)
    print(f"  STMu at {round(state['stmu_at'], 2) if state.get('stmu_at') else None} s; the player's elapsed time frozen: "
          f"{zone['frozen']} (last {(zone['last_el'] or 0) / 1000:.2f} s)", flush=True)
    ctl = {k: round(v, 3) for k, v in state.items() if k.endswith(('_sent', '_at')) and isinstance(v, float)}
    print(f'  control: {ctl}', flush=True)
    off = []
    for k, st in enumerate(streams):
        d = st['max_el'] - st['length']
        print(f"  stream {k + 1} (tracks {st['first']} to {st['last']}, {st['length']:.2f} s): the player's elapsed time "
              f"reached {st['max_el']:.2f} s ({d:+.2f} s)", flush=True)
        if abs(d) > A.elapsed_tolerance:
            off.append(f"stream {k + 1} {d:+.2f} s")
    want = len(run_tracks())
    ok = reached == A.tracks and state.get('stmu_at') and not state.get('stuck_at') and not off and len(streams) == want
    print(f"VERDICT: {'OK' if ok else 'STUCK' if state.get('stuck_at') else 'FAIL'}: the zone reached {reached} of {A.tracks} tracks"
          + (", STMu" if state.get('stmu_at') else ', no STMu') + f", {len(streams)} of {want} streams begun (STMs)"
          + (f", elapsed off: {', '.join(off)}" if off else ', elapsed to each stream\'s end'), flush=True)
    if A.json:
        with open(A.json, 'w') as f:
            json.dump({'args': vars(A), 'tracks': rows, 'streams': streams, 'state': {k: v for k, v in state.items()}, 'events': events},
                      f, indent=1)


if __name__ == '__main__':
    main()
