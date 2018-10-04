# Luke's build of st - the simple (suckless) terminal

The [suckless terminal (st)](https://st.suckless.org/) with some additional features:

+ Adjustable transparency/alpha
+ Compatibility with `Xresources` and `pywal` for dynamic colors
+ Copy is alt-c, paste is alt-v or alt-p pastes from primary selection
+ Default font is system "mono" at 14pt, meaning the font will match your system font.
+ Hold alt and press either ↑/↓ or the vim keys k/j to move up/down in the terminal.
+ Shift+Mouse wheel will as well.
+ Alt-u and Alt-d scroll back/forward in history a page at a time.
+ Alt-PageUp and Alt-PageDown will do the same.
+ Zoom in/out with Alt+Shift+k/j or u/d for larger intervals.
+ Vertcenter
+ Default solarized colors without `pywal`
+ updated to latest version 0.8.1

The following additional bindings were added before I forked this:

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
