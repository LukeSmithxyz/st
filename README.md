# Luke's build of st - the simple (suckless) terminal

Forked from [https://github.com/shiva/st](https://github.com/shiva/st) for simplicity's sake, which is the [suckless terminal (st)](https://st.suckless.org/) with some patches added:

+ transparency
+ copy to clipboard
+ solarized colors (light and dark toggleable)
+ vertcenter
+ scrollback with keyboard
+ scrollback with mouse

## My own additions

+ Default font is system "mono" at 14pt
+ Fixed transparency patch (see below for installation)
+ Toggle light/dark mode now Alt-Tab instead of the frequently conflicting F6
+ Alt-k and Alt-j scroll back/foward in history one line at a time
+ Alt-u and Alt-d scroll back/foward in history a page at a time

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

### Solarized mode

The terminal is now transparent by default, if you want non-transparent solarized colors, run the following:

```
make clean
patch -R < patches/transparency.diff
make
sudo make install
```

You can change the transparency value by changing the `alpha` variable in the `config.h` file.
