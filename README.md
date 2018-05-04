# My Suckless Terminal Build

[Suckless Terminal](https://st.suckless.org) [Arch](https://www.archlinux.org/) package with a few patches installed to keep things nice:

+ [alpha](https://st.suckless.org/patches/alpha/)
+ [clipboard](https://st.suckless.org/patches/clipboard/)
+ [solarized](https://st.suckless.org/patches/solarized/)
+ [vertcenter](https://st.suckless.org/patches/vertcenter/)
+ [scrollback](https://st.suckless.org/patches/scrollback/)

## Terminal-specific mappings

+ Scroll through history -- Shift+PageUp/PageDown or Shift+Mouse wheel
+ Alt-k and Alt-j scroll back/foward in history one line at a time
+ Alt-u and Alt-d scroll back/foward in history a page at a time
+ Increase/decrease font size -- Shift+Alt+PageUp/PageDown
+ Return to default font size -- Shift+Alt+Home
+ Paste -- Shift+Insert

## Installation

```
makepkg -si
```

## Further Notes

+ Change the transparency value by modifying the `alpha` variable in [config.h](https://github.com/alrayyes/st/blob/master/config.h).
+ Default font is system "mono" at 16pt
+ Forked from [https://github.com/LukeSmithxyz/st](https://github.com/LukeSmithxyz/st)
* When modifying [config.h](https://github.com/alrayyes/st/blob/master/config.h) be sure to run ```updpkgsums``` to update checksums before running ```makepkg -si```

## License

This theme is released under the MIT License. For more information read the [license][license].

[license]: https://github.com/alrayyes/st/blob/master/LICENSE.md
