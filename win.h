/* See LICENSE for license details. */

/* X modifiers */
#define XK_ANY_MOD    UINT_MAX
#define XK_NO_MOD     0
#define XK_SWITCH_MOD (1<<13)

typedef XftGlyphFontSpec GlyphFontSpec;

void draw(void);
void drawregion(int, int, int, int);
void run(void);

void xbell(int);
void xclipcopy(void);
void xclippaste(void);
void xhints(void);
void xinit(void);
void xloadcols(void);
int xsetcolorname(int, const char *);
void xloadfonts(char *, double);
void xsetenv(void);
void xsettitle(char *);
void xsetpointermotion(int);
void xseturgency(int);
void xunloadfonts(void);
void xresize(int, int);
void xselpaste(void);
unsigned long xwinid(void);
void xsetsel(char *, Time);
