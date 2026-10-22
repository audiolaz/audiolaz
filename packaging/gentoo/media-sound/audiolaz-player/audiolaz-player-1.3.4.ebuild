# Copyright 2026 Gentoo Authors
# Distributed under the terms of the GNU General Public License v2

EAPI=8

inherit systemd toolchain-funcs

DESCRIPTION="Free bit-perfect network audio player for Roon, LMS, UPnP and AirPlay"
HOMEPAGE="
	https://audiolaz.com/free-audio-player
	https://github.com/audiolaz/audiolaz
"
SRC_URI="https://github.com/audiolaz/audiolaz/archive/refs/tags/v${PV}.tar.gz -> audiolaz-${PV}.tar.gz"
S="${WORKDIR}/audiolaz-${PV}"

# MIT; the embedded Archivo font (src/player/cover_font.h): OFL-1.1
LICENSE="MIT OFL-1.1"
SLOT="0"
KEYWORDS="~amd64 ~arm64"
IUSE="+flac test"
RESTRICT="!test? ( test )"

# libFLAC is not linked: the player dlopen()s libFLAC.so.14 (or .12, .8) at run time
# and plays FLAC streams (Roon's and LMS's default) only when it is there. Its headers
# are needed to build either way.
RDEPEND="
	media-libs/alsa-lib
	flac? ( media-libs/flac )
"
DEPEND="
	media-libs/alsa-lib
	media-libs/flac
"

src_prepare() {
	default

	# the tag's VERSION file still says 1.2.6: the player reports the packaged version
	echo "${PV}" > VERSION || die

	# build.sh runs the built player (--version) at its end: not when cross-compiling
	if tc-is-cross-compiler; then
		sed -i -e '/^  \.\/build\/audiolaz-player --version$/d' build.sh || die
	fi
}

src_compile() {
	tc-export CC
	# build.sh links in the same command it compiles: LDFLAGS go with CFLAGS.
	# Its own -O3 comes first, the user's CFLAGS after it win.
	CFLAGS="${CFLAGS} ${LDFLAGS}" LANG_BUILD=en sh ./build.sh player || die
}

src_test() {
	# No sound card, the network on this machine only. The audiolaz Link receiver's
	# discovery (multicast DNS) needs a multicast route, which the network sandbox
	# (lo alone) lacks: that one check is skipped, the rest of the loop runs.
	sed -i -e '/^if "\$T\/browse"; then/c\echo "  discovery: skipped (no multicast in the build sandbox)"' \
		tests/anet-loop.sh || die
	grep -q "discovery: skipped" tests/anet-loop.sh || die "the mDNS check was not found in tests/anet-loop.sh"

	sh tests/run.sh || die
}

src_install() {
	dobin build/audiolaz-player build/audiolaz-recv

	# ALSA loads libasound_module_pcm_<type>.so for "type <type>": the plug-in's type is azdrv
	exeinto /usr/$(get_libdir)/alsa-lib
	newexe build/libasound_module_pcm_audiolaz.so libasound_module_pcm_azdrv.so

	# the PCM "audiolaz"; alsa-lib reads /etc/alsa/conf.d (as media-video/pipewire does)
	insinto /usr/share/alsa/alsa.conf.d
	doins "${FILESDIR}"/50-audiolaz.conf
	dosym ../../../usr/share/alsa/alsa.conf.d/50-audiolaz.conf /etc/alsa/conf.d/50-audiolaz.conf

	# installed, never enabled
	newinitd "${FILESDIR}"/audiolaz-player.initd audiolaz-player
	newconfd "${FILESDIR}"/audiolaz-player.confd audiolaz-player
	systemd_dounit "${FILESDIR}"/audiolaz-player.service

	dodoc README.md NOTICE.md docs/*.md
}

pkg_postinst() {
	if [[ -z ${REPLACING_VERSIONS} ]]; then
		elog "The service is installed, not enabled. Choose the DAC with audiolaz-player --list-devices."
		elog "OpenRC: set NAME and DAC in ${EROOT}/etc/conf.d/audiolaz-player, then"
		elog "  rc-update add audiolaz-player default && rc-service audiolaz-player start"
		elog "systemd: set them with systemctl edit audiolaz-player (Environment=\"NAME=...\" \"DAC=...\"),"
		elog "  then systemctl enable --now audiolaz-player"
		elog "Its page: http://<this machine>:8765. Roon: Settings > Extensions > Squeezebox support."
		elog
		elog "AirPlay (shairport-sync) and Spotify Connect (librespot) play into the player through"
		elog "the ALSA Loopback (snd-aloop); the kernel module media-sound/snd-audiolaz lets any"
		elog "application play into it. See ${EROOT}/usr/share/doc/${PF}/PLAYER.md*"
	fi
	if ! use flac; then
		ewarn "Built with USE=-flac: without media-libs/flac the player cannot play FLAC streams;"
		ewarn "turn FLAC off for its zone in Roon, or install media-libs/flac."
	fi
}
