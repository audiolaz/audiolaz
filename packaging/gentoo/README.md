# Gentoo ebuilds

This folder is a small Portage overlay (repository name `audiolaz`) that builds the `v1.3.0` tag's GitHub tarball
(amd64 and arm64, so GentooPlayer and other Gentoo-based systems too):

| Package | What | Licence |
| --- | --- | --- |
| `media-sound/audiolaz-player` | the player, the audiolaz Link receiver, the `audiolaz` ALSA output, an OpenRC service and a systemd unit | MIT (an embedded font: OFL-1.1) |
| `media-sound/snd-audiolaz` | the `audiolaz` sound card (kernel module `snd-audiolaz`), built with `linux-mod-r1` for your kernel | GPL-2 |

```
packaging/gentoo/
├── metadata/layout.conf          masters = gentoo
├── profiles/repo_name            audiolaz
└── media-sound/
    ├── audiolaz-player/          audiolaz-player-1.3.0.ebuild, metadata.xml, Manifest
    │   └── files/                audiolaz-player.initd, audiolaz-player.confd, audiolaz-player.service, 50-audiolaz.conf
    └── snd-audiolaz/             snd-audiolaz-1.3.0.ebuild, metadata.xml, Manifest
```

Distributions with their own overlay (GentooPlayer…) can copy the two `media-sound/` folders into it as they are: both
ebuilds share the distfile `audiolaz-1.3.0.tar.gz`, and nothing in them refers to this overlay.

## Add the overlay

The overlay is a folder of this repository, not a repository of its own (so `eselect repository add`, which syncs a
whole repository, does not fit): it is added as a local overlay.

```sh
git clone https://github.com/audiolaz/audiolaz.git /var/db/repos/audiolaz-src
ln -s /var/db/repos/audiolaz-src/packaging/gentoo /var/db/repos/audiolaz
```

and `/etc/portage/repos.conf/audiolaz.conf`:

```ini
[audiolaz]
location = /var/db/repos/audiolaz
auto-sync = no
```

To update: `git -C /var/db/repos/audiolaz-src pull`.

## Install

The packages are keyworded `~amd64 ~arm64`; on a stable system, accept them first:

```sh
echo 'media-sound/audiolaz-player ~amd64' >> /etc/portage/package.accept_keywords/audiolaz   # ~arm64 on arm64
echo 'media-sound/snd-audiolaz ~amd64'    >> /etc/portage/package.accept_keywords/audiolaz
emerge --ask media-sound/audiolaz-player
emerge --ask media-sound/snd-audiolaz      # optional: needs your kernel's sources or build tree (/usr/src/linux)
```

USE flags of `audiolaz-player`:

| Flag | |
| --- | --- |
| `flac` (on) | pulls in `media-libs/flac`, which the player opens at run time (`dlopen`) to play FLAC streams: Roon's and LMS's default. The player is the same either way; `media-libs/flac`'s headers are needed to build it in both cases. Without it, turn FLAC off for the zone in Roon. |
| `test` | `FEATURES=test` runs the unit tests (no sound card; 127.0.0.1 only). The audiolaz Link receiver's mDNS discovery check needs multicast, which Portage's network sandbox lacks: it is skipped there, the rest runs. |

## What gets installed

| Path | |
| --- | --- |
| `/usr/bin/audiolaz-player` | the player (`--help`, `--list-devices`, `--who`, `--version`) |
| `/usr/bin/audiolaz-recv` | the audiolaz Link receiver, for the machine beside the DAC (`audiolaz-recv --dac auto`) |
| `/usr/lib64/alsa-lib/libasound_module_pcm_azdrv.so` | the `audiolaz` ALSA output plug-in (ALSA type `azdrv`) |
| `/usr/share/alsa/alsa.conf.d/50-audiolaz.conf`, linked from `/etc/alsa/conf.d/` | defines the PCM `audiolaz`: `aplay -D audiolaz …` plays into the player |
| `/etc/init.d/audiolaz-player`, `/etc/conf.d/audiolaz-player` | the OpenRC service (supervise-daemon) and its settings, **not added to any runlevel** |
| `/usr/lib/systemd/system/audiolaz-player.service` | the systemd unit, **not enabled** |
| `/usr/share/doc/audiolaz-player-1.3.0/` | README, PLAYER.md (every option, the control file, the page's API), LINK-PROTOCOL.md, BIT-PERFECT.md |
| `/lib/modules/<kernel>/extra/snd-audiolaz.ko` | (`snd-audiolaz`) the kernel module |

AirPlay ([shairport-sync](https://github.com/mikebrady/shairport-sync)) and Spotify Connect
([librespot](https://github.com/librespot-org/librespot)) are not in the Gentoo tree: when you install them, they play
into the player through the ALSA Loopback (`snd-aloop`, see PLAYER.md).

## Start it

Roon: enable *Settings › Extensions › Squeezebox support*. Find your DAC with `audiolaz-player --list-devices`
(prefer `hw:CARD=<id>,DEV=0`).

**OpenRC.** Set `NAME`, `DAC` (and `PAGE`, `OPTIONS`) in `/etc/conf.d/audiolaz-player`, then:

```sh
rc-update add audiolaz-player default
rc-service audiolaz-player start
```

For a second DAC: `ln -s audiolaz-player /etc/init.d/audiolaz-player.kitchen`, its settings in
`/etc/conf.d/audiolaz-player.kitchen` (another `PAGE`); its state goes to `/var/lib/audiolaz-player.kitchen`.
The player's messages go to the file named by `error_log` in the conf.d file (none by default).

**systemd.** The same variables, in a drop-in:

```sh
systemctl edit audiolaz-player
#   [Service]
#   Environment="NAME=Living room" "DAC=hw:CARD=DAC,DEV=0"
systemctl enable --now audiolaz-player
```

Either way the player runs as root (real-time threads, memory locked; `rc_ulimit` and `LimitRTPRIO=`/`LimitMEMLOCK=`
allow them), keeps its state in `/var/lib/audiolaz-player`, and its page is `http://<this machine>:8765`.
`OPTIONS` defaults to `--loop none --squeeze auto` (Roon/LMS and UPnP): the player refuses `--loop none` without
`--squeeze`.

The kernel module: `modprobe snd-audiolaz` (its options: `modinfo snd-audiolaz`); to load it at boot with OpenRC, add
`modules="snd-audiolaz"` to `/etc/conf.d/modules` (systemd: `echo snd-audiolaz > /etc/modules-load.d/audiolaz.conf`).

## Notes for packagers

- The tag's `VERSION` file still says 1.2.6: the ebuild writes `${PV}` into it, so `--version` says 1.3.0.
- `build.sh` compiles and links in one command, so the ebuild passes `LDFLAGS` along with `CFLAGS`. It runs the built
  player (`--version`) at its end; the ebuild drops that line when cross-compiling.
- ALSA loads `libasound_module_pcm_<type>.so` for `type <type>`: the plug-in is built as
  `libasound_module_pcm_audiolaz.so` but its type is `azdrv`, hence the name it is installed under.
- `src/linux/kmod/dkms.conf` (`PACKAGE_VERSION="0.3"`, an obsolete `CLEAN`) is for DKMS and not used here:
  `linux-mod-r1` builds the module with the `Makefile` (`KDIR=${KV_OUT_DIR}`) and installs it under `extra/`.
