# Maintainer: Ryan Kes <alrayyes gmail com>

pkgname=st
pkgver=0.8.1
pkgrel=1
pkgdesc='A simple virtual terminal emulator for X.'
arch=('i686' 'x86_64')
license=('MIT')
depends=('libxft' 'libxext' 'xorg-fonts-misc')
makedepends=('ncurses')
url="http://st.suckless.org"

_patches=("https://st.suckless.org/patches/clipboard/st-clipboard-20180309-c5ba9c0.diff"
          "https://st.suckless.org/patches/scrollback/st-scrollback-0.8.diff"
          "https://st.suckless.org/patches/scrollback/st-scrollback-mouse-0.8.diff"
          "https://st.suckless.org/patches/vertcenter/st-vertcenter-20180320-6ac8c8a.diff"
          "https://st.suckless.org/patches/alpha/st-alpha-20171221-0ac685f.diff"
          "https://st.suckless.org/patches/solarized/st-no_bold_colors-20170623-b331da5.diff"
          "https://st.suckless.org/patches/solarized/st-solarized-dark-20170623-b331da5.diff")

source=("http://dl.suckless.org/st/$pkgname-$pkgver.tar.gz"
        "config.h"
        "${_patches[@]}")

sha256sums=('c4fb0fe2b8d2d3bd5e72763e80a8ae05b7d44dbac8f8e3bb18ef0161c7266926'
            'ece5ef9100be9388ae3d78f34469b8306dec250ae5d49435e62286156fbd7820'
            '4989c03de5165234303d3929e3b60d662828972203561651aa6dc6b8f67feeb8'
            '8279d347c70bc9b36f450ba15e1fd9ff62eedf49ce9258c35d7f1cfe38cca226'
            '3fb38940cc3bad3f9cd1e2a0796ebd0e48950a07860ecf8523a5afd0cd1b5a44'
            '04e6a4696293f668260b2f54a7240e379dbfabbc209de07bd5d4d57e9f513360'
            'bc7949dfb3fb4026db4a2659e291f128ae3fbb302ad5cb9b51fb28b1eb3a5433'
            '71e1211189d9e11da93ee49388379c5f8469fcd3e1f48bb4d791ddaf161f5845'
            '2a0cdd946e420591f39f068753b2f6dab0d076b962512cb850f3ba4492ee7c1b')

prepare() {
  cd $srcdir/$pkgname-$pkgver
  # skip terminfo which conflicts with nsurses
  sed -i '/\ttic -sx st.info/d' Makefile

  # Modify alpha patch to prevent conflicts
  sed -i '1,44d' "$srcdir/$(basename ${_patches[4]})" 
  sed -i 's/size_t colornamelen/unsigned int tabspaces/g' "$srcdir/$(basename ${_patches[4]})"
  sed -i '30,44d' "$srcdir/$(basename ${_patches[4]})" 
  sed -i '1,68d' "$srcdir/$(basename ${_patches[6]})" 

  for patch in "${_patches[@]}"; do
    echo "Applying patch $(basename $patch)..."
    patch -Np1 -i "$srcdir/$(basename $patch)"
  done

  cp $srcdir/config.h config.h
}

build() {
  cd $srcdir/$pkgname-$pkgver
  make X11INC=/usr/include/X11 X11LIB=/usr/lib/X11
}

package() {
  cd $srcdir/$pkgname-$pkgver
  make PREFIX=/usr DESTDIR="$pkgdir" TERMINFO="$pkgdir/usr/share/terminfo" install
  install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
  install -Dm644 README "$pkgdir/usr/share/doc/$pkgname/README"
}
