# Luke's build of st - the simple (suckless) terminal

Forked from [https://github.com/shiva/st](https://github.com/shiva/st) for simplicity's sake, which is the [suckless terminal (st)](https://st.suckless.org/) with some patches added:

+ transparency
+ copy to clipboard
+ ~~Solarized colors (light and dark toggleable)~~ (Change to your own colors, see no value of toggle)
+ vertcenter
+ scrollback with keyboard
+ scrollback with mouse
+ updated to latest version 0.8.1

## My own additions

+ Default font is system "mono" at 14pt
+ Alt-k and Alt-j scroll back/foward in history one line at a time
+ Alt-u and Alt-d scroll back/foward in history a page at a time
+ Applied colors, insipred by Pop! OS colors.

## Terminal-specific mappings

(Additions before me.)

+ Scroll through history -- Shift+PageUp/PageDown or Shift+Mouse wheel
+ Increase/decrease font size -- Shift+Alt+PageUp/PageDown
+ Return to default font size -- Shift+Alt+Home
+ Paste -- Shift+Insert

## Installation for newbs

## Requirements for build

Install `base-devel` - compiler and stuff (most distros have).

Install code depndencies (voidlinux - most distro should have something similar) 

+ `fontconfig-devel` 
+ `libX11-devel` 
+ `libXft-devel`


```
make
sudo make install
```

## Custom changes (`config.def.h` or `config.h`)

+ change `alpha`
+ chnage colors in `colorname` added comments with numbers `0`-`15` colors and also `bg`, `fg` and `cursor` color. 
