# st - simple terminal

Forked from [https://github.com/shiva/st](https://github.com/shiva/st) for simplicity's sake, which is the suckless terminal (st) with some patches added:

+ copy to clipboard
+ solarized colors (light and dark toggleable)
+ vertcenter
+ scrollback with keyboard
+ scrollback with mouse

## My additions

+ Default font is system "mono" at 14pt
+ Toggle light/dark mode now Alt-Tab instead of the frequently conflicting F6
+ Alt-k and Alt-j scroll back/foward in history one line at a time
+ Alt-u and Alt-d scroll back/foward in history a page at a time

### Transparency

I also have redone the transparency patch diff since it would fail to apply with too many of the other patches applied. See below for installation.

## Terminal-specific mappings

(Additions before me.)

+ Scroll through history -- Shift+PageUp/PageDown or Shift+Mouse wheel
+ Increase/decrease font size -- Shift+Alt+PageUp/PageDown
+ Return to default dont size -- Shift+Alt+Home
+ Paste -- Shift+Insert

## Installation for newbs

```
make
sudo make install
```

### Transparency

If you want transparency, run the following.

```
make clean
patch < patches/transparency.diff
make
sudo make install
```

And to remove transparency, just unapply the diff patch and reinstall.

```
make clean
patch -R < patches/transparency.diff
make
sudo make install
```
