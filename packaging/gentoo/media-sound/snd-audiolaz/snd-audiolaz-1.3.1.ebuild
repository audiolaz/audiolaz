# Copyright 2026 Gentoo Authors
# Distributed under the terms of the GNU General Public License v2

EAPI=8

inherit linux-mod-r1

DESCRIPTION="The audiolaz sound card: any application plays into the audiolaz player"
HOMEPAGE="
	https://audiolaz.com
	https://github.com/audiolaz/audiolaz
"
SRC_URI="https://github.com/audiolaz/audiolaz/archive/refs/tags/v${PV}.tar.gz -> audiolaz-${PV}.tar.gz"
S="${WORKDIR}/audiolaz-${PV}/src/linux/kmod"

LICENSE="GPL-2"
SLOT="0"
KEYWORDS="~amd64 ~arm64"

CONFIG_CHECK="SND SND_PCM"

src_compile() {
	local modlist=( snd-audiolaz )
	local modargs=( KDIR="${KV_OUT_DIR}" )

	linux-mod-r1_src_compile
}

pkg_postinst() {
	linux-mod-r1_pkg_postinst

	elog "Load it with: modprobe snd-audiolaz (its options: modinfo snd-audiolaz)."
	elog "The player that reads the card: media-sound/audiolaz-player."
}
