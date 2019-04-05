# Maintainer:

pkgname=st-luke-git
_pkgname=st
_pkgver=0.8.1
pkgver=0.8.1.r1046.1cd0b79
pkgrel=1
pkgdesc="Luke's simple (suckless) terminal with vim-bindings, transparency, xresources, etc. "
url='https://github.com/LukeSmithxyz/st'
arch=('i686' 'x86_64')
license=('MIT')
options=('zipman')
depends=('libxft')
makedepends=('ncurses' 'libxext' 'git')
optdepends=('dmenu: feed urls to dmenu')
source=('git://github.com/LukeSmithxyz/st')
sha1sums=('SKIP')

provides=("${_pkgname}")
conflicts=("${_pkgname}")

pkgver() {
	cd "${_pkgname}"
	printf "${_pkgver}.r%s.%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
}

prepare() {
	cd $srcdir/${_pkgname}
 	# skip terminfo which conflicts with nsurses
	sed -i '/tic /d' Makefile
}

build() {
	cd "${_pkgname}"
	make X11INC=/usr/include/X11 X11LIB=/usr/lib/X11
}

package() {
	cd "${_pkgname}"
	make PREFIX=/usr DESTDIR="${pkgdir}" install
	install -Dm644 LICENSE "${pkgdir}/usr/share/licenses/${pkgname}/LICENSE"
	install -Dm644 README.md "${pkgdir}/usr/share/doc/${pkgname}/README.md"
}
