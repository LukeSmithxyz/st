# Luke's build of st - the simple (suckless) terminal
Forked from [https://github.com/shiva/st](https://github.com/shiva/st) for simplicity's sake, which is the [suckless terminal (st)](https://st.suckless.org/) with some patches added:

+ transparency
+ copy to clipboard (alt-shift-c)
+ Optional compatibility with `Xresources` and `pywal` for dynamic colors
+ Solarized colors (light and dark toggleable)
+ vertcenter
+ scrollback with keyboard
+ scrollback with mouse
+ updated to latest version 0.8.1

## My own additions

+ Default font is system "mono" at 14pt, meaning the font will match your system font.
+ Hold alt and press either ↑/↓ or the vim keys k/j to move up/down in the terminal.
+ Alt-u and Alt-d scroll back/forward in history a page at a time.
+ Alt-PageUp and Alt-PageDown scroll back/forward in history a page at a time.
+ Transparency with solarized colors by default.
+ Zoom in/out with Alt+Shift+k/j or u/d for larger intervals.

## Terminal-specific mappings

(Additions before me.)

+ Scroll through history -- Shift+PageUp/PageDown or Shift+Mouse wheel
+ Increase/decrease font size -- Shift+Alt+PageUp/PageDown
+ Return to default font size -- Shift+Alt+Home
+ Paste -- Shift+Insert

## Installation for newbs

```
make
sudo make install
```

Obviously, `make` is required to build. `fontconfig` is required for the default build, since it asks `fontconfig` for your system monospace font.  It might be obvious, but `libX11` and `libXft` are required as well. Chances are, you have all of this installed already.

## Custom changes (`config.def.h` or `config.h`)

### Solarized

By default, the terminal is transparent with a blackish background. There's a patch file `solarized-alpha-toggle.patch` which you can use to remove the transparency and give it a typical deep, dark blue solarized background. Just run:

```
patch < solarized-alpha-toggle.patch
```

Then, run `make` & `sudo make install` again to install the new build. You make reverse the solarized background by running the same command as above, but giving `patch` the `-R` option as well.

### `Xresources` and `pywal`/`wal` compatibility

If you use `wal` to maintain color schemes across your programs, you can use the `xresources.patch`.

```
patch < xresources.patch
make && sudo make install
```

## Explore `config.h`

+ Change `colorname[]` array values (88 LOC), default colours are solarized.
+ Numbers of 0 - 15 are usual terminal colors. Change them to your liking.
+ Change `bg` to your desired terminal background color.
+ Change `fg` to your desired terminal foreground color.
+ Change `cursor` to your desired terminal cursor color.
