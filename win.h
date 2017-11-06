/* See LICENSE for license details. */

/* X modifiers */
#define XK_ANY_MOD    UINT_MAX
#define XK_NO_MOD     0
#define XK_SWITCH_MOD (1<<13)

void draw(void);
void drawregion(int, int, int, int);

void xbell(void);
void xclipcopy(void);
void xhints(void);
void xloadcols(void);
int xsetcolorname(int, const char *);
void xsettitle(char *);
int xsetcursor(int);
void xsetpointermotion(int);
void xsetsel(char *, Time);
