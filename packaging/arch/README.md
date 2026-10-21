# Arch Linux packages

`PKGBUILD` builds two packages from the `v1.3.1` tag's GitHub tarball (x86_64 and aarch64, so Arch Linux ARM and
Arch-based distributions such as Audiolinux too):

| Package | What | Licence |
| --- | --- | --- |
| `audiolaz-player` | the player, the audiolaz Link receiver, the `audiolaz` ALSA output | MIT (an embedded font: OFL-1.1) |
| `snd-audiolaz-dkms` | the `audiolaz` sound card (kernel module `snd-audiolaz`), built by DKMS for each kernel | GPL-2.0-only |

## Build and install

```sh
sudo pacman -S --needed base-devel git
git clone https://github.com/audiolaz/audiolaz.git && cd audiolaz/packaging/arch
makepkg -si                         # both packages; the unit tests run in check() (no sound card, 127.0.0.1 only)
```

`makepkg -si` asks which ones to install; or `sudo pacman -U audiolaz-player-*.pkg.tar.zst`. For the kernel module,
install the headers of your kernel first (`linux-headers`, `linux-lts-headers`, `linux-rt-headers`…): DKMS builds it
at install time and again with each new kernel.

## What gets installed

| Path | |
| --- | --- |
| `/usr/bin/audiolaz-player` | the player (`--help`, `--list-devices`, `--who`, `--version`) |
| `/usr/bin/audiolaz-recv` | the audiolaz Link receiver, for the machine beside the DAC (`audiolaz-recv --dac auto`) |
| `/usr/lib/alsa-lib/libasound_module_pcm_azdrv.so` | the `audiolaz` ALSA output plug-in (ALSA type `azdrv`) |
| `/usr/share/alsa/alsa.conf.d/50-audiolaz.conf`, linked from `/etc/alsa/conf.d/` | defines the PCM `audiolaz`: `aplay -D audiolaz …` plays into the player |
| `/usr/lib/systemd/system/audiolaz-player.service` | a service unit, **not enabled** |
| `/etc/audiolaz-player.conf` | its settings: name, DAC, page port, options (kept on upgrades) |
| `/usr/share/doc/audiolaz-player/` | README, PLAYER.md (every option, the control file, the page's API), LINK-PROTOCOL.md, BIT-PERFECT.md |
| `/usr/src/snd-audiolaz-<version>/` | (`snd-audiolaz-dkms`) the module's source and `dkms.conf` |

Optional, at run time: `flac` (libFLAC, opened when present: FLAC streams from Roon/LMS and UPnP; without it, turn
FLAC off for the zone in Roon), `shairport-sync` (AirPlay) and `librespot` (Spotify Connect), which play into the
player through the ALSA Loopback (`snd-aloop`, see PLAYER.md).

## Start it

Once, by hand (Roon: *Settings › Extensions › Squeezebox support* on):

```sh
audiolaz-player --list-devices
sudo mkdir -p /var/lib/audiolaz-player
sudo audiolaz-player --loop none --squeeze auto --name "Living room" \
    --dac hw:CARD=DAC,DEV=0 --rooms /var/lib/audiolaz-player --page 8765
```

As a service: set `NAME`, `DAC` (and `OPTIONS`) in `/etc/audiolaz-player.conf`, then
`sudo systemctl enable --now audiolaz-player`. It runs as root (real-time threads, memory locked, its state in
`/var/lib/audiolaz-player`). The page: `http://<this machine>:8765`. One player per DAC: for another, copy the unit
and its conf under another name, with another `PAGE`.

The kernel module: `sudo modprobe snd-audiolaz` (its options: `modinfo snd-audiolaz`).
