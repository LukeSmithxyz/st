/* See LICENSE for licence details. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <X11/XKBlib.h>
#include <fontconfig/fontconfig.h>
#include <wchar.h>

#include "arg.h"

char *argv0;

#define Glyph Glyph_
#define Font Font_

#if   defined(__linux)
 #include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
 #include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
 #include <libutil.h>
#endif


/* XEMBED messages */
#define XEMBED_FOCUS_IN  4
#define XEMBED_FOCUS_OUT 5

/* Arbitrary sizes */
#define UTF_INVALID   0xFFFD
#define UTF_SIZ       4
#define ESC_BUF_SIZ   (128*UTF_SIZ)
#define ESC_ARG_SIZ   16
#define STR_BUF_SIZ   ESC_BUF_SIZ
#define STR_ARG_SIZ   ESC_ARG_SIZ
#define DRAW_BUF_SIZ  20*1024
#define XK_ANY_MOD    UINT_MAX
#define XK_NO_MOD     0
#define XK_SWITCH_MOD (1<<13)

#define REDRAW_TIMEOUT (80*1000) /* 80 ms */

/* macros */
#define MIN(a, b)  ((a) < (b) ? (a) : (b))
#define MAX(a, b)  ((a) < (b) ? (b) : (a))
#define LEN(a)     (sizeof(a) / sizeof(a)[0])
#define DEFAULT(a, b)     (a) = (a) ? (a) : (b)
#define BETWEEN(x, a, b)  ((a) <= (x) && (x) <= (b))
#define ISCONTROLC0(c) (BETWEEN(c, 0, 0x1f) || (c) == '\177')
#define ISCONTROLC1(c) (BETWEEN(c, 0x80, 0x9f))
#define ISCONTROL(c) (ISCONTROLC0(c) || ISCONTROLC1(c))
#define LIMIT(x, a, b)    (x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x)
#define ATTRCMP(a, b) ((a).mode != (b).mode || (a).fg != (b).fg || (a).bg != (b).bg)
#define IS_SET(flag) ((term.mode & (flag)) != 0)
#define TIMEDIFF(t1, t2) ((t1.tv_sec-t2.tv_sec)*1000 + (t1.tv_nsec-t2.tv_nsec)/1E6)
#define MODBIT(x, set, bit) ((set) ? ((x) |= (bit)) : ((x) &= ~(bit)))

#define TRUECOLOR(r,g,b) (1 << 24 | (r) << 16 | (g) << 8 | (b))
#define IS_TRUECOL(x)    (1 << 24 & (x))
#define TRUERED(x)       (((x) & 0xff0000) >> 8)
#define TRUEGREEN(x)     (((x) & 0xff00))
#define TRUEBLUE(x)      (((x) & 0xff) << 8)


enum glyph_attribute {
	ATTR_NULL      = 0,
	ATTR_BOLD      = 1 << 0,
	ATTR_FAINT     = 1 << 1,
	ATTR_ITALIC    = 1 << 2,
	ATTR_UNDERLINE = 1 << 3,
	ATTR_BLINK     = 1 << 4,
	ATTR_REVERSE   = 1 << 5,
	ATTR_INVISIBLE = 1 << 6,
	ATTR_STRUCK    = 1 << 7,
	ATTR_WRAP      = 1 << 8,
	ATTR_WIDE      = 1 << 9,
	ATTR_WDUMMY    = 1 << 10,
};

enum cursor_movement {
	CURSOR_SAVE,
	CURSOR_LOAD
};

enum cursor_state {
	CURSOR_DEFAULT  = 0,
	CURSOR_WRAPNEXT = 1,
	CURSOR_ORIGIN   = 2
};

enum term_mode {
	MODE_WRAP        = 1 << 0,
	MODE_INSERT      = 1 << 1,
	MODE_APPKEYPAD   = 1 << 2,
	MODE_ALTSCREEN   = 1 << 3,
	MODE_CRLF        = 1 << 4,
	MODE_MOUSEBTN    = 1 << 5,
	MODE_MOUSEMOTION = 1 << 6,
	MODE_REVERSE     = 1 << 7,
	MODE_KBDLOCK     = 1 << 8,
	MODE_HIDE        = 1 << 9,
	MODE_ECHO        = 1 << 10,
	MODE_APPCURSOR   = 1 << 11,
	MODE_MOUSESGR    = 1 << 12,
	MODE_8BIT        = 1 << 13,
	MODE_BLINK       = 1 << 14,
	MODE_FBLINK      = 1 << 15,
	MODE_FOCUS       = 1 << 16,
	MODE_MOUSEX10    = 1 << 17,
	MODE_MOUSEMANY   = 1 << 18,
	MODE_BRCKTPASTE  = 1 << 19,
	MODE_PRINT       = 1 << 20,
	MODE_MOUSE       = MODE_MOUSEBTN|MODE_MOUSEMOTION|MODE_MOUSEX10\
	                  |MODE_MOUSEMANY,
};

enum charset {
	CS_GRAPHIC0,
	CS_GRAPHIC1,
	CS_UK,
	CS_USA,
	CS_MULTI,
	CS_GER,
	CS_FIN
};

enum escape_state {
	ESC_START      = 1,
	ESC_CSI        = 2,
	ESC_STR        = 4,  /* DCS, OSC, PM, APC */
	ESC_ALTCHARSET = 8,
	ESC_STR_END    = 16, /* a final string was encountered */
	ESC_TEST       = 32, /* Enter in test mode */
};

enum window_state {
	WIN_VISIBLE = 1,
	WIN_REDRAW  = 2,
	WIN_FOCUSED = 4
};

enum selection_type {
	SEL_REGULAR = 1,
	SEL_RECTANGULAR = 2
};

enum selection_snap {
	SNAP_WORD = 1,
	SNAP_LINE = 2
};

typedef unsigned char uchar;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef unsigned short ushort;

typedef XftDraw *Draw;
typedef XftColor Color;

typedef struct {
	char c[UTF_SIZ]; /* character code */
	ushort mode;      /* attribute flags */
	uint32_t fg;      /* foreground  */
	uint32_t bg;      /* background  */
} Glyph;

typedef Glyph *Line;

typedef struct {
	Glyph attr; /* current char attributes */
	int x;
	int y;
	char state;
} TCursor;

/* CSI Escape sequence structs */
/* ESC '[' [[ [<priv>] <arg> [;]] <mode>] */
typedef struct {
	char buf[ESC_BUF_SIZ]; /* raw string */
	int len;               /* raw string length */
	char priv;
	int arg[ESC_ARG_SIZ];
	int narg;              /* nb of args */
	char mode;
} CSIEscape;

/* STR Escape sequence structs */
/* ESC type [[ [<priv>] <arg> [;]] <mode>] ESC '\' */
typedef struct {
	char type;             /* ESC type ... */
	char buf[STR_BUF_SIZ]; /* raw string */
	int len;               /* raw string length */
	char *args[STR_ARG_SIZ];
	int narg;              /* nb of args */
} STREscape;

/* Internal representation of the screen */
typedef struct {
	int row;      /* nb row */
	int col;      /* nb col */
	Line *line;   /* screen */
	Line *alt;    /* alternate screen */
	bool *dirty;  /* dirtyness of lines */
	TCursor c;    /* cursor */
	int top;      /* top    scroll limit */
	int bot;      /* bottom scroll limit */
	int mode;     /* terminal mode flags */
	int esc;      /* escape state flags */
	char trantbl[4]; /* charset table translation */
	int charset;  /* current charset */
	int icharset; /* selected charset for sequence */
	bool numlock; /* lock numbers in keyboard */
	bool *tabs;
} Term;

/* Purely graphic info */
typedef struct {
	Display *dpy;
	Colormap cmap;
	Window win;
	Drawable buf;
	Atom xembed, wmdeletewin, netwmname, netwmpid;
	XIM xim;
	XIC xic;
	Draw draw;
	Visual *vis;
	XSetWindowAttributes attrs;
	int scr;
	bool isfixed; /* is fixed geometry? */
	int l, t; /* left and top offset */
	int gm; /* geometry mask */
	int tw, th; /* tty width and height */
	int w, h; /* window width and height */
	int ch; /* char height */
	int cw; /* char width  */
	char state; /* focus, redraw, visible */
} XWindow;

typedef struct {
	uint b;
	uint mask;
	char *s;
} Mousekey;

typedef struct {
	KeySym k;
	uint mask;
	char *s;
	/* three valued logic variables: 0 indifferent, 1 on, -1 off */
	signed char appkey;    /* application keypad */
	signed char appcursor; /* application cursor */
	signed char crlf;      /* crlf mode          */
} Key;

typedef struct {
	int mode;
	int type;
	int snap;
	/*
	 * Selection variables:
	 * nb – normalized coordinates of the beginning of the selection
	 * ne – normalized coordinates of the end of the selection
	 * ob – original coordinates of the beginning of the selection
	 * oe – original coordinates of the end of the selection
	 */
	struct {
		int x, y;
	} nb, ne, ob, oe;

	char *clip;
	Atom xtarget;
	bool alt;
	struct timespec tclick1;
	struct timespec tclick2;
} Selection;

typedef union {
	int i;
	uint ui;
	float f;
	const void *v;
} Arg;

typedef struct {
	uint mod;
	KeySym keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Shortcut;

/* function definitions used in config.h */
static void clippaste(const Arg *);
static void numlock(const Arg *);
static void selpaste(const Arg *);
static void xzoom(const Arg *);
static void xzoomabs(const Arg *);
static void xzoomreset(const Arg *);
static void printsel(const Arg *);
static void printscreen(const Arg *) ;
static void toggleprinter(const Arg *);

/* Config.h for applying patches and the configuration. */
#include "config.h"

/* Font structure */
typedef struct {
	int height;
	int width;
	int ascent;
	int descent;
	short lbearing;
	short rbearing;
	XftFont *match;
	FcFontSet *set;
	FcPattern *pattern;
} Font;

/* Drawing Context */
typedef struct {
	Color col[MAX(LEN(colorname), 256)];
	Font font, bfont, ifont, ibfont;
	GC gc;
} DC;

static void die(const char *, ...);
static void draw(void);
static void redraw(int);
static void drawregion(int, int, int, int);
static void execsh(void);
static void sigchld(int);
static void run(void);

static void csidump(void);
static void csihandle(void);
static void csiparse(void);
static void csireset(void);
static int eschandle(uchar ascii);
static void strdump(void);
static void strhandle(void);
static void strparse(void);
static void strreset(void);

static int tattrset(int);
static void tprinter(char *, size_t);
static void tdumpsel(void);
static void tdumpline(int);
static void tdump(void);
static void tclearregion(int, int, int, int);
static void tcursor(int);
static void tdeletechar(int);
static void tdeleteline(int);
static void tinsertblank(int);
static void tinsertblankline(int);
static int tlinelen(int);
static void tmoveto(int, int);
static void tmoveato(int, int);
static void tnew(int, int);
static void tnewline(int);
static void tputtab(int);
static void tputc(char *, int);
static void treset(void);
static void tresize(int, int);
static void tscrollup(int, int);
static void tscrolldown(int, int);
static void tsetattr(int *, int);
static void tsetchar(char *, Glyph *, int, int);
static void tsetscroll(int, int);
static void tswapscreen(void);
static void tsetdirt(int, int);
static void tsetdirtattr(int);
static void tsetmode(bool, bool, int *, int);
static void tfulldirt(void);
static void techo(char *, int);
static void tcontrolcode(uchar );
static void tdectest(char );
static int32_t tdefcolor(int *, int *, int);
static void tdeftran(char);
static inline bool match(uint, uint);
static void ttynew(void);
static void ttyread(void);
static void ttyresize(void);
static void ttysend(char *, size_t);
static void ttywrite(const char *, size_t);
static void tstrsequence(uchar c);

static void xdraws(char *, Glyph, int, int, int, int);
static void xhints(void);
static void xclear(int, int, int, int);
static void xdrawcursor(void);
static void xinit(void);
static void xloadcols(void);
static int xsetcolorname(int, const char *);
static int xgeommasktogravity(int);
static int xloadfont(Font *, FcPattern *);
static void xloadfonts(char *, double);
static int xloadfontset(Font *);
static void xsettitle(char *);
static void xresettitle(void);
static void xsetpointermotion(int);
static void xseturgency(int);
static void xsetsel(char *);
static void xtermclear(int, int, int, int);
static void xunloadfont(Font *);
static void xunloadfonts(void);
static void xresize(int, int);

static void expose(XEvent *);
static void visibility(XEvent *);
static void unmap(XEvent *);
static char *kmap(KeySym, uint);
static void kpress(XEvent *);
static void cmessage(XEvent *);
static void cresize(int, int);
static void resize(XEvent *);
static void focus(XEvent *);
static void brelease(XEvent *);
static void bpress(XEvent *);
static void bmotion(XEvent *);
static void selnotify(XEvent *);
static void selclear(XEvent *);
static void selrequest(XEvent *);

static void selinit(void);
static void selnormalize(void);
static inline bool selected(int, int);
static char *getsel(void);
static void selcopy(void);
static void selscroll(int, int);
static void selsnap(int, int *, int *, int);
static void getbuttoninfo(XEvent *);
static void mousereport(XEvent *);

static size_t utf8decode(char *, long *, size_t);
static long utf8decodebyte(char, size_t *);
static size_t utf8encode(long, char *, size_t);
static char utf8encodebyte(long, size_t);
static size_t utf8len(char *);
static size_t utf8validate(long *, size_t);

static ssize_t xwrite(int, const char *, size_t);
static void *xmalloc(size_t);
static void *xrealloc(void *, size_t);
static char *xstrdup(char *);

static void usage(void);

static void (*handler[LASTEvent])(XEvent *) = {
	[KeyPress] = kpress,
	[ClientMessage] = cmessage,
	[ConfigureNotify] = resize,
	[VisibilityNotify] = visibility,
	[UnmapNotify] = unmap,
	[Expose] = expose,
	[FocusIn] = focus,
	[FocusOut] = focus,
	[MotionNotify] = bmotion,
	[ButtonPress] = bpress,
	[ButtonRelease] = brelease,
	[SelectionClear] = selclear,
	[SelectionNotify] = selnotify,
	[SelectionRequest] = selrequest,
};

/* Globals */
static DC dc;
static XWindow xw;
static Term term;
static CSIEscape csiescseq;
static STREscape strescseq;
static int cmdfd;
static pid_t pid;
static Selection sel;
static int iofd = STDOUT_FILENO;
static char **opt_cmd = NULL;
static char *opt_io = NULL;
static char *opt_title = NULL;
static char *opt_embed = NULL;
static char *opt_class = NULL;
static char *opt_font = NULL;
static int oldbutton = 3; /* button event on startup: 3 = release */

static char *usedfont = NULL;
static double usedfontsize = 0;
static double defaultfontsize = 0;

static uchar utfbyte[UTF_SIZ + 1] = {0x80,    0, 0xC0, 0xE0, 0xF0};
static uchar utfmask[UTF_SIZ + 1] = {0xC0, 0x80, 0xE0, 0xF0, 0xF8};
static long utfmin[UTF_SIZ + 1] = {       0,    0,  0x80,  0x800,  0x10000};
static long utfmax[UTF_SIZ + 1] = {0x10FFFF, 0x7F, 0x7FF, 0xFFFF, 0x10FFFF};

/* Font Ring Cache */
enum {
	FRC_NORMAL,
	FRC_ITALIC,
	FRC_BOLD,
	FRC_ITALICBOLD
};

typedef struct {
	XftFont *font;
	int flags;
} Fontcache;

/* Fontcache is an array now. A new font will be appended to the array. */
static Fontcache frc[16];
static int frclen = 0;

ssize_t
xwrite(int fd, const char *s, size_t len) {
	size_t aux = len;

	while(len > 0) {
		ssize_t r = write(fd, s, len);
		if(r < 0)
			return r;
		len -= r;
		s += r;
	}
	return aux;
}

void *
xmalloc(size_t len) {
	void *p = malloc(len);

	if(!p)
		die("Out of memory\n");

	return p;
}

void *
xrealloc(void *p, size_t len) {
	if((p = realloc(p, len)) == NULL)
		die("Out of memory\n");

	return p;
}

char *
xstrdup(char *s) {
	if((s = strdup(s)) == NULL)
		die("Out of memory\n");

	return s;
}

size_t
utf8decode(char *c, long *u, size_t clen) {
	size_t i, j, len, type;
	long udecoded;

	*u = UTF_INVALID;
	if(!clen)
		return 0;
	udecoded = utf8decodebyte(c[0], &len);
	if(!BETWEEN(len, 1, UTF_SIZ))
		return 1;
	for(i = 1, j = 1; i < clen && j < len; ++i, ++j) {
		udecoded = (udecoded << 6) | utf8decodebyte(c[i], &type);
		if(type != 0)
			return j;
	}
	if(j < len)
		return 0;
	*u = udecoded;
	utf8validate(u, len);
	return len;
}

long
utf8decodebyte(char c, size_t *i) {
	for(*i = 0; *i < LEN(utfmask); ++(*i))
		if(((uchar)c & utfmask[*i]) == utfbyte[*i])
			return (uchar)c & ~utfmask[*i];
	return 0;
}

size_t
utf8encode(long u, char *c, size_t clen) {
	size_t len, i;

	len = utf8validate(&u, 0);
	if(clen < len)
		return 0;
	for(i = len - 1; i != 0; --i) {
		c[i] = utf8encodebyte(u, 0);
		u >>= 6;
	}
	c[0] = utf8encodebyte(u, len);
	return len;
}

char
utf8encodebyte(long u, size_t i) {
	return utfbyte[i] | (u & ~utfmask[i]);
}

size_t
utf8len(char *c) {
	return utf8decode(c, &(long){0}, UTF_SIZ);
}

size_t
utf8validate(long *u, size_t i) {
	if(!BETWEEN(*u, utfmin[i], utfmax[i]) || BETWEEN(*u, 0xD800, 0xDFFF))
		*u = UTF_INVALID;
	for(i = 1; *u > utfmax[i]; ++i)
		;
	return i;
}

static void
selinit(void) {
	memset(&sel.tclick1, 0, sizeof(sel.tclick1));
	memset(&sel.tclick2, 0, sizeof(sel.tclick2));
	sel.mode = 0;
	sel.ob.x = -1;
	sel.clip = NULL;
	sel.xtarget = XInternAtom(xw.dpy, "UTF8_STRING", 0);
	if(sel.xtarget == None)
		sel.xtarget = XA_STRING;
}

static int
x2col(int x) {
	x -= borderpx;
	x /= xw.cw;

	return LIMIT(x, 0, term.col-1);
}

static int
y2row(int y) {
	y -= borderpx;
	y /= xw.ch;

	return LIMIT(y, 0, term.row-1);
}

static int tlinelen(int y) {
	int i = term.col;

	if(term.line[y][i - 1].mode & ATTR_WRAP)
		return i;

	while(i > 0 && term.line[y][i - 1].c[0] == ' ')
		--i;

	return i;
}

static void
selnormalize(void) {
	int i;

	if(sel.ob.y == sel.oe.y || sel.type == SEL_RECTANGULAR) {
		sel.nb.x = MIN(sel.ob.x, sel.oe.x);
		sel.ne.x = MAX(sel.ob.x, sel.oe.x);
	} else {
		sel.nb.x = sel.ob.y < sel.oe.y ? sel.ob.x : sel.oe.x;
		sel.ne.x = sel.ob.y < sel.oe.y ? sel.oe.x : sel.ob.x;
	}
	sel.nb.y = MIN(sel.ob.y, sel.oe.y);
	sel.ne.y = MAX(sel.ob.y, sel.oe.y);

	selsnap(sel.snap, &sel.nb.x, &sel.nb.y, -1);
	selsnap(sel.snap, &sel.ne.x, &sel.ne.y, +1);

	/* expand selection over line breaks */
	if (sel.type == SEL_RECTANGULAR)
		return;
	i = tlinelen(sel.nb.y);
	if (i < sel.nb.x)
		sel.nb.x = i;
	if (tlinelen(sel.ne.y) <= sel.ne.x)
		sel.ne.x = term.col - 1;
}

static inline bool
selected(int x, int y) {
	if(sel.type == SEL_RECTANGULAR)
		return BETWEEN(y, sel.nb.y, sel.ne.y)
		    && BETWEEN(x, sel.nb.x, sel.ne.x);

	return BETWEEN(y, sel.nb.y, sel.ne.y)
	    && (y != sel.nb.y || x >= sel.nb.x)
	    && (y != sel.ne.y || x <= sel.ne.x);
}

void
selsnap(int mode, int *x, int *y, int direction) {
	int newx, newy, xt, yt;
	bool delim, prevdelim;
	Glyph *gp, *prevgp;

	switch(mode) {
	case SNAP_WORD:
		/*
		 * Snap around if the word wraps around at the end or
		 * beginning of a line.
		 */
		prevgp = &term.line[*y][*x];
		prevdelim = strchr(worddelimiters, prevgp->c[0]) != NULL;
		for(;;) {
			newx = *x + direction;
			newy = *y;
			if(!BETWEEN(newx, 0, term.col - 1)) {
				newy += direction;
				newx = (newx + term.col) % term.col;
				if (!BETWEEN(newy, 0, term.row - 1))
					break;

				if(direction > 0)
					yt = *y, xt = *x;
				else
					yt = newy, xt = newx;
				if(!(term.line[yt][xt].mode & ATTR_WRAP))
					break;
			}

			if (newx >= tlinelen(newy))
				break;

			gp = &term.line[newy][newx];
			delim = strchr(worddelimiters, gp->c[0]) != NULL;
			if(!(gp->mode & ATTR_WDUMMY) && (delim != prevdelim
					|| (delim && gp->c[0] != prevgp->c[0])))
				break;

			*x = newx;
			*y = newy;
			prevgp = gp;
			prevdelim = delim;
		}
		break;
	case SNAP_LINE:
		/*
		 * Snap around if the the previous line or the current one
		 * has set ATTR_WRAP at its end. Then the whole next or
		 * previous line will be selected.
		 */
		*x = (direction < 0) ? 0 : term.col - 1;
		if(direction < 0 && *y > 0) {
			for(; *y > 0; *y += direction) {
				if(!(term.line[*y-1][term.col-1].mode
						& ATTR_WRAP)) {
					break;
				}
			}
		} else if(direction > 0 && *y < term.row-1) {
			for(; *y < term.row; *y += direction) {
				if(!(term.line[*y][term.col-1].mode
						& ATTR_WRAP)) {
					break;
				}
			}
		}
		break;
	}
}

void
getbuttoninfo(XEvent *e) {
	int type;
	uint state = e->xbutton.state & ~(Button1Mask | forceselmod);

	sel.alt = IS_SET(MODE_ALTSCREEN);

	sel.oe.x = x2col(e->xbutton.x);
	sel.oe.y = y2row(e->xbutton.y);
	selnormalize();

	sel.type = SEL_REGULAR;
	for(type = 1; type < LEN(selmasks); ++type) {
		if(match(selmasks[type], state)) {
			sel.type = type;
			break;
		}
	}
}

void
mousereport(XEvent *e) {
	int x = x2col(e->xbutton.x), y = y2row(e->xbutton.y),
	    button = e->xbutton.button, state = e->xbutton.state,
	    len;
	char buf[40];
	static int ox, oy;

	/* from urxvt */
	if(e->xbutton.type == MotionNotify) {
		if(x == ox && y == oy)
			return;
		if(!IS_SET(MODE_MOUSEMOTION) && !IS_SET(MODE_MOUSEMANY))
			return;
		/* MOUSE_MOTION: no reporting if no button is pressed */
		if(IS_SET(MODE_MOUSEMOTION) && oldbutton == 3)
			return;

		button = oldbutton + 32;
		ox = x;
		oy = y;
	} else {
		if(!IS_SET(MODE_MOUSESGR) && e->xbutton.type == ButtonRelease) {
			button = 3;
		} else {
			button -= Button1;
			if(button >= 3)
				button += 64 - 3;
		}
		if(e->xbutton.type == ButtonPress) {
			oldbutton = button;
			ox = x;
			oy = y;
		} else if(e->xbutton.type == ButtonRelease) {
			oldbutton = 3;
			/* MODE_MOUSEX10: no button release reporting */
			if(IS_SET(MODE_MOUSEX10))
				return;
			if (button == 64 || button == 65)
				return;
		}
	}

	if(!IS_SET(MODE_MOUSEX10)) {
		button += (state & ShiftMask   ? 4  : 0)
			+ (state & Mod4Mask    ? 8  : 0)
			+ (state & ControlMask ? 16 : 0);
	}

	len = 0;
	if(IS_SET(MODE_MOUSESGR)) {
		len = snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c",
				button, x+1, y+1,
				e->xbutton.type == ButtonRelease ? 'm' : 'M');
	} else if(x < 223 && y < 223) {
		len = snprintf(buf, sizeof(buf), "\033[M%c%c%c",
				32+button, 32+x+1, 32+y+1);
	} else {
		return;
	}

	ttywrite(buf, len);
}

void
bpress(XEvent *e) {
	struct timespec now;
	Mousekey *mk;

	if(IS_SET(MODE_MOUSE) && !(e->xbutton.state & forceselmod)) {
		mousereport(e);
		return;
	}

	for(mk = mshortcuts; mk < mshortcuts + LEN(mshortcuts); mk++) {
		if(e->xbutton.button == mk->b
				&& match(mk->mask, e->xbutton.state)) {
			ttysend(mk->s, strlen(mk->s));
			return;
		}
	}

	if(e->xbutton.button == Button1) {
		clock_gettime(CLOCK_MONOTONIC, &now);

		/* Clear previous selection, logically and visually. */
		selclear(NULL);
		sel.mode = 1;
		sel.type = SEL_REGULAR;
		sel.oe.x = sel.ob.x = x2col(e->xbutton.x);
		sel.oe.y = sel.ob.y = y2row(e->xbutton.y);

		/*
		 * If the user clicks below predefined timeouts specific
		 * snapping behaviour is exposed.
		 */
		if(TIMEDIFF(now, sel.tclick2) <= tripleclicktimeout) {
			sel.snap = SNAP_LINE;
		} else if(TIMEDIFF(now, sel.tclick1) <= doubleclicktimeout) {
			sel.snap = SNAP_WORD;
		} else {
			sel.snap = 0;
		}
		selnormalize();

		/*
		 * Draw selection, unless it's regular and we don't want to
		 * make clicks visible
		 */
		if(sel.snap != 0) {
			sel.mode++;
			tsetdirt(sel.nb.y, sel.ne.y);
		}
		sel.tclick2 = sel.tclick1;
		sel.tclick1 = now;
	}
}

char *
getsel(void) {
	char *str, *ptr;
	int y, bufsize, size, lastx, linelen;
	Glyph *gp, *last;

	if(sel.ob.x == -1)
		return NULL;

	bufsize = (term.col+1) * (sel.ne.y-sel.nb.y+1) * UTF_SIZ;
	ptr = str = xmalloc(bufsize);

	/* append every set & selected glyph to the selection */
	for(y = sel.nb.y; y < sel.ne.y + 1; y++) {
		linelen = tlinelen(y);

		if(sel.type == SEL_RECTANGULAR) {
			gp = &term.line[y][sel.nb.x];
			lastx = sel.ne.x;
		} else {
			gp = &term.line[y][sel.nb.y == y ? sel.nb.x : 0];
			lastx = (sel.ne.y == y) ? sel.ne.x : term.col-1;
		}
		last = &term.line[y][MIN(lastx, linelen-1)];
		while(last >= gp && last->c[0] == ' ')
			--last;

		for( ; gp <= last; ++gp) {
			if(gp->mode & ATTR_WDUMMY)
				continue;

			size = utf8len(gp->c);
			memcpy(ptr, gp->c, size);
			ptr += size;
		}

		/*
		 * Copy and pasting of line endings is inconsistent
		 * in the inconsistent terminal and GUI world.
		 * The best solution seems like to produce '\n' when
		 * something is copied from st and convert '\n' to
		 * '\r', when something to be pasted is received by
		 * st.
		 * FIXME: Fix the computer world.
		 */
		if((y < sel.ne.y || lastx >= linelen) && !(last->mode & ATTR_WRAP))
			*ptr++ = '\n';
	}
	*ptr = 0;
	return str;
}

void
selcopy(void) {
	xsetsel(getsel());
}

void
selnotify(XEvent *e) {
	ulong nitems, ofs, rem;
	int format;
	uchar *data, *last, *repl;
	Atom type;

	ofs = 0;
	do {
		if(XGetWindowProperty(xw.dpy, xw.win, XA_PRIMARY, ofs, BUFSIZ/4,
					False, AnyPropertyType, &type, &format,
					&nitems, &rem, &data)) {
			fprintf(stderr, "Clipboard allocation failed\n");
			return;
		}

		/*
		 * As seen in getsel:
		 * Line endings are inconsistent in the terminal and GUI world
		 * copy and pasting. When receiving some selection data,
		 * replace all '\n' with '\r'.
		 * FIXME: Fix the computer world.
		 */
		repl = data;
		last = data + nitems * format / 8;
		while((repl = memchr(repl, '\n', last - repl))) {
			*repl++ = '\r';
		}

		if(IS_SET(MODE_BRCKTPASTE))
			ttywrite("\033[200~", 6);
		ttysend((char *)data, nitems * format / 8);
		if(IS_SET(MODE_BRCKTPASTE))
			ttywrite("\033[201~", 6);
		XFree(data);
		/* number of 32-bit chunks returned */
		ofs += nitems * format / 32;
	} while(rem > 0);
}

void
selpaste(const Arg *dummy) {
	XConvertSelection(xw.dpy, XA_PRIMARY, sel.xtarget, XA_PRIMARY,
			xw.win, CurrentTime);
}

void
clippaste(const Arg *dummy) {
	Atom clipboard;

	clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
	XConvertSelection(xw.dpy, clipboard, sel.xtarget, XA_PRIMARY,
			xw.win, CurrentTime);
}

void
selclear(XEvent *e) {
	if(sel.ob.x == -1)
		return;
	sel.ob.x = -1;
	tsetdirt(sel.nb.y, sel.ne.y);
}

void
selrequest(XEvent *e) {
	XSelectionRequestEvent *xsre;
	XSelectionEvent xev;
	Atom xa_targets, string;

	xsre = (XSelectionRequestEvent *) e;
	xev.type = SelectionNotify;
	xev.requestor = xsre->requestor;
	xev.selection = xsre->selection;
	xev.target = xsre->target;
	xev.time = xsre->time;
	/* reject */
	xev.property = None;

	xa_targets = XInternAtom(xw.dpy, "TARGETS", 0);
	if(xsre->target == xa_targets) {
		/* respond with the supported type */
		string = sel.xtarget;
		XChangeProperty(xsre->display, xsre->requestor, xsre->property,
				XA_ATOM, 32, PropModeReplace,
				(uchar *) &string, 1);
		xev.property = xsre->property;
	} else if(xsre->target == sel.xtarget && sel.clip != NULL) {
		XChangeProperty(xsre->display, xsre->requestor, xsre->property,
				xsre->target, 8, PropModeReplace,
				(uchar *) sel.clip, strlen(sel.clip));
		xev.property = xsre->property;
	}

	/* all done, send a notification to the listener */
	if(!XSendEvent(xsre->display, xsre->requestor, True, 0, (XEvent *) &xev))
		fprintf(stderr, "Error sending SelectionNotify event\n");
}

void
xsetsel(char *str) {
	/* register the selection for both the clipboard and the primary */
	Atom clipboard;

	free(sel.clip);
	sel.clip = str;

	XSetSelectionOwner(xw.dpy, XA_PRIMARY, xw.win, CurrentTime);

	clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
	XSetSelectionOwner(xw.dpy, clipboard, xw.win, CurrentTime);
}

void
brelease(XEvent *e) {
	if(IS_SET(MODE_MOUSE) && !(e->xbutton.state & forceselmod)) {
		mousereport(e);
		return;
	}

	if(e->xbutton.button == Button2) {
		selpaste(NULL);
	} else if(e->xbutton.button == Button1) {
		if(sel.mode < 2) {
			selclear(NULL);
		} else {
			getbuttoninfo(e);
			selcopy();
		}
		sel.mode = 0;
		tsetdirt(sel.nb.y, sel.ne.y);
	}
}

void
bmotion(XEvent *e) {
	int oldey, oldex, oldsby, oldsey;

	if(IS_SET(MODE_MOUSE) && !(e->xbutton.state & forceselmod)) {
		mousereport(e);
		return;
	}

	if(!sel.mode)
		return;

	sel.mode++;
	oldey = sel.oe.y;
	oldex = sel.oe.x;
	oldsby = sel.nb.y;
	oldsey = sel.ne.y;
	getbuttoninfo(e);

	if(oldey != sel.oe.y || oldex != sel.oe.x)
		tsetdirt(MIN(sel.nb.y, oldsby), MAX(sel.ne.y, oldsey));
}

void
die(const char *errstr, ...) {
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(EXIT_FAILURE);
}

void
execsh(void) {
	char **args, *sh, *prog;
	const struct passwd *pw;
	char buf[sizeof(long) * 8 + 1];

	errno = 0;
	if((pw = getpwuid(getuid())) == NULL) {
		if(errno)
			die("getpwuid:%s\n", strerror(errno));
		else
			die("who are you?\n");
	}

	if (!(sh = getenv("SHELL"))) {
		sh = (pw->pw_shell[0]) ? pw->pw_shell : shell;
	}

	if(opt_cmd)
		prog = opt_cmd[0];
	else if(utmp)
		prog = utmp;
	else
		prog = sh;
	args = (opt_cmd) ? opt_cmd : (char *[]) {prog, NULL};

	snprintf(buf, sizeof(buf), "%lu", xw.win);

	unsetenv("COLUMNS");
	unsetenv("LINES");
	unsetenv("TERMCAP");
	setenv("LOGNAME", pw->pw_name, 1);
	setenv("USER", pw->pw_name, 1);
	setenv("SHELL", sh, 1);
	setenv("HOME", pw->pw_dir, 1);
	setenv("TERM", termname, 1);
	setenv("WINDOWID", buf, 1);

	signal(SIGCHLD, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGALRM, SIG_DFL);

	execvp(prog, args);
	_exit(EXIT_FAILURE);
}

void
sigchld(int a) {
	int stat, ret;

	if(waitpid(pid, &stat, 0) < 0)
		die("Waiting for pid %hd failed: %s\n", pid, strerror(errno));

	ret = WIFEXITED(stat) ? WEXITSTATUS(stat) : EXIT_FAILURE;
	if (ret != EXIT_SUCCESS)
		die("child finished with error '%d'\n", stat);
	exit(EXIT_SUCCESS);
}

void
ttynew(void) {
	int m, s;
	struct winsize w = {term.row, term.col, 0, 0};

	/* seems to work fine on linux, openbsd and freebsd */
	if(openpty(&m, &s, NULL, NULL, &w) < 0)
		die("openpty failed: %s\n", strerror(errno));

	switch(pid = fork()) {
	case -1:
		die("fork failed\n");
		break;
	case 0:
		setsid(); /* create a new process group */
		dup2(s, STDIN_FILENO);
		dup2(s, STDOUT_FILENO);
		dup2(s, STDERR_FILENO);
		if(ioctl(s, TIOCSCTTY, NULL) < 0)
			die("ioctl TIOCSCTTY failed: %s\n", strerror(errno));
		close(s);
		close(m);
		execsh();
		break;
	default:
		close(s);
		cmdfd = m;
		signal(SIGCHLD, sigchld);
		if(opt_io) {
			term.mode |= MODE_PRINT;
			iofd = (!strcmp(opt_io, "-")) ?
				  STDOUT_FILENO :
				  open(opt_io, O_WRONLY | O_CREAT, 0666);
			if(iofd < 0) {
				fprintf(stderr, "Error opening %s:%s\n",
					opt_io, strerror(errno));
			}
		}
		break;
	}
}

void
ttyread(void) {
	static char buf[BUFSIZ];
	static int buflen = 0;
	char *ptr;
	char s[UTF_SIZ];
	int charsize; /* size of utf8 char in bytes */
	long unicodep;
	int ret;

	/* append read bytes to unprocessed bytes */
	if((ret = read(cmdfd, buf+buflen, LEN(buf)-buflen)) < 0)
		die("Couldn't read from shell: %s\n", strerror(errno));

	/* process every complete utf8 char */
	buflen += ret;
	ptr = buf;
	while((charsize = utf8decode(ptr, &unicodep, buflen))) {
		utf8encode(unicodep, s, UTF_SIZ);
		tputc(s, charsize);
		ptr += charsize;
		buflen -= charsize;
	}

	/* keep any uncomplete utf8 char for the next call */
	memmove(buf, ptr, buflen);
}

void
ttywrite(const char *s, size_t n) {
	if(xwrite(cmdfd, s, n) == -1)
		die("write error on tty: %s\n", strerror(errno));
}

void
ttysend(char *s, size_t n) {
	ttywrite(s, n);
	if(IS_SET(MODE_ECHO))
		techo(s, n);
}

void
ttyresize(void) {
	struct winsize w;

	w.ws_row = term.row;
	w.ws_col = term.col;
	w.ws_xpixel = xw.tw;
	w.ws_ypixel = xw.th;
	if(ioctl(cmdfd, TIOCSWINSZ, &w) < 0)
		fprintf(stderr, "Couldn't set window size: %s\n", strerror(errno));
}

int
tattrset(int attr) {
	int i, j;

	for(i = 0; i < term.row-1; i++) {
		for(j = 0; j < term.col-1; j++) {
			if(term.line[i][j].mode & attr)
				return 1;
		}
	}

	return 0;
}

void
tsetdirt(int top, int bot) {
	int i;

	LIMIT(top, 0, term.row-1);
	LIMIT(bot, 0, term.row-1);

	for(i = top; i <= bot; i++)
		term.dirty[i] = 1;
}

void
tsetdirtattr(int attr) {
	int i, j;

	for(i = 0; i < term.row-1; i++) {
		for(j = 0; j < term.col-1; j++) {
			if(term.line[i][j].mode & attr) {
				tsetdirt(i, i);
				break;
			}
		}
	}
}

void
tfulldirt(void) {
	tsetdirt(0, term.row-1);
}

void
tcursor(int mode) {
	static TCursor c[2];
	bool alt = IS_SET(MODE_ALTSCREEN);

	if(mode == CURSOR_SAVE) {
		c[alt] = term.c;
	} else if(mode == CURSOR_LOAD) {
		term.c = c[alt];
		tmoveto(c[alt].x, c[alt].y);
	}
}

void
treset(void) {
	uint i;

	term.c = (TCursor){{
		.mode = ATTR_NULL,
		.fg = defaultfg,
		.bg = defaultbg
	}, .x = 0, .y = 0, .state = CURSOR_DEFAULT};

	memset(term.tabs, 0, term.col * sizeof(*term.tabs));
	for(i = tabspaces; i < term.col; i += tabspaces)
		term.tabs[i] = 1;
	term.top = 0;
	term.bot = term.row - 1;
	term.mode = MODE_WRAP;
	memset(term.trantbl, sizeof(term.trantbl), CS_USA);
	term.charset = 0;

	for(i = 0; i < 2; i++) {
		tmoveto(0, 0);
		tcursor(CURSOR_SAVE);
		tclearregion(0, 0, term.col-1, term.row-1);
		tswapscreen();
	}
}

void
tnew(int col, int row) {
	term = (Term){ .c = { .attr = { .fg = defaultfg, .bg = defaultbg } } };
	tresize(col, row);
	term.numlock = 1;

	treset();
}

void
tswapscreen(void) {
	Line *tmp = term.line;

	term.line = term.alt;
	term.alt = tmp;
	term.mode ^= MODE_ALTSCREEN;
	tfulldirt();
}

void
tscrolldown(int orig, int n) {
	int i;
	Line temp;

	LIMIT(n, 0, term.bot-orig+1);

	tsetdirt(orig, term.bot-n);
	tclearregion(0, term.bot-n+1, term.col-1, term.bot);

	for(i = term.bot; i >= orig+n; i--) {
		temp = term.line[i];
		term.line[i] = term.line[i-n];
		term.line[i-n] = temp;
	}

	selscroll(orig, n);
}

void
tscrollup(int orig, int n) {
	int i;
	Line temp;

	LIMIT(n, 0, term.bot-orig+1);

	tclearregion(0, orig, term.col-1, orig+n-1);
	tsetdirt(orig+n, term.bot);

	for(i = orig; i <= term.bot-n; i++) {
		temp = term.line[i];
		term.line[i] = term.line[i+n];
		term.line[i+n] = temp;
	}

	selscroll(orig, -n);
}

void
selscroll(int orig, int n) {
	if(sel.ob.x == -1)
		return;

	if(BETWEEN(sel.ob.y, orig, term.bot) || BETWEEN(sel.oe.y, orig, term.bot)) {
		if((sel.ob.y += n) > term.bot || (sel.oe.y += n) < term.top) {
			selclear(NULL);
			return;
		}
		if(sel.type == SEL_RECTANGULAR) {
			if(sel.ob.y < term.top)
				sel.ob.y = term.top;
			if(sel.oe.y > term.bot)
				sel.oe.y = term.bot;
		} else {
			if(sel.ob.y < term.top) {
				sel.ob.y = term.top;
				sel.ob.x = 0;
			}
			if(sel.oe.y > term.bot) {
				sel.oe.y = term.bot;
				sel.oe.x = term.col;
			}
		}
		selnormalize();
	}
}

void
tnewline(int first_col) {
	int y = term.c.y;

	if(y == term.bot) {
		tscrollup(term.top, 1);
	} else {
		y++;
	}
	tmoveto(first_col ? 0 : term.c.x, y);
}

void
csiparse(void) {
	char *p = csiescseq.buf, *np;
	long int v;

	csiescseq.narg = 0;
	if(*p == '?') {
		csiescseq.priv = 1;
		p++;
	}

	csiescseq.buf[csiescseq.len] = '\0';
	while(p < csiescseq.buf+csiescseq.len) {
		np = NULL;
		v = strtol(p, &np, 10);
		if(np == p)
			v = 0;
		if(v == LONG_MAX || v == LONG_MIN)
			v = -1;
		csiescseq.arg[csiescseq.narg++] = v;
		p = np;
		if(*p != ';' || csiescseq.narg == ESC_ARG_SIZ)
			break;
		p++;
	}
	csiescseq.mode = *p;
}

/* for absolute user moves, when decom is set */
void
tmoveato(int x, int y) {
	tmoveto(x, y + ((term.c.state & CURSOR_ORIGIN) ? term.top: 0));
}

void
tmoveto(int x, int y) {
	int miny, maxy;

	if(term.c.state & CURSOR_ORIGIN) {
		miny = term.top;
		maxy = term.bot;
	} else {
		miny = 0;
		maxy = term.row - 1;
	}
	LIMIT(x, 0, term.col-1);
	LIMIT(y, miny, maxy);
	term.c.state &= ~CURSOR_WRAPNEXT;
	term.c.x = x;
	term.c.y = y;
}

void
tsetchar(char *c, Glyph *attr, int x, int y) {
	static char *vt100_0[62] = { /* 0x41 - 0x7e */
		"↑", "↓", "→", "←", "█", "▚", "☃", /* A - G */
		0, 0, 0, 0, 0, 0, 0, 0, /* H - O */
		0, 0, 0, 0, 0, 0, 0, 0, /* P - W */
		0, 0, 0, 0, 0, 0, 0, " ", /* X - _ */
		"◆", "▒", "␉", "␌", "␍", "␊", "°", "±", /* ` - g */
		"␤", "␋", "┘", "┐", "┌", "└", "┼", "⎺", /* h - o */
		"⎻", "─", "⎼", "⎽", "├", "┤", "┴", "┬", /* p - w */
		"│", "≤", "≥", "π", "≠", "£", "·", /* x - ~ */
	};

	/*
	 * The table is proudly stolen from rxvt.
	 */
	if(term.trantbl[term.charset] == CS_GRAPHIC0) {
		if(BETWEEN(c[0], 0x41, 0x7e) && vt100_0[c[0] - 0x41]) {
			c = vt100_0[c[0] - 0x41];
		}
	}

	if(term.line[y][x].mode & ATTR_WIDE) {
		if(x+1 < term.col) {
			term.line[y][x+1].c[0] = ' ';
			term.line[y][x+1].mode &= ~ATTR_WDUMMY;
		}
	} else if(term.line[y][x].mode & ATTR_WDUMMY) {
		term.line[y][x-1].c[0] = ' ';
		term.line[y][x-1].mode &= ~ATTR_WIDE;
	}

	term.dirty[y] = 1;
	term.line[y][x] = *attr;
	memcpy(term.line[y][x].c, c, UTF_SIZ);
}

void
tclearregion(int x1, int y1, int x2, int y2) {
	int x, y, temp;
	Glyph *gp;

	if(x1 > x2)
		temp = x1, x1 = x2, x2 = temp;
	if(y1 > y2)
		temp = y1, y1 = y2, y2 = temp;

	LIMIT(x1, 0, term.col-1);
	LIMIT(x2, 0, term.col-1);
	LIMIT(y1, 0, term.row-1);
	LIMIT(y2, 0, term.row-1);

	for(y = y1; y <= y2; y++) {
		term.dirty[y] = 1;
		for(x = x1; x <= x2; x++) {
			gp = &term.line[y][x];
			if(selected(x, y))
				selclear(NULL);
			gp->fg = term.c.attr.fg;
			gp->bg = term.c.attr.bg;
			gp->mode = 0;
			memcpy(gp->c, " ", 2);
		}
	}
}

void
tdeletechar(int n) {
	int dst, src, size;
	Glyph *line;

	LIMIT(n, 0, term.col - term.c.x);

	dst = term.c.x;
	src = term.c.x + n;
	size = term.col - src;
	line = term.line[term.c.y];

	memmove(&line[dst], &line[src], size * sizeof(Glyph));
	tclearregion(term.col-n, term.c.y, term.col-1, term.c.y);
}

void
tinsertblank(int n) {
	int dst, src, size;
	Glyph *line;

	LIMIT(n, 0, term.col - term.c.x);

	dst = term.c.x + n;
	src = term.c.x;
	size = term.col - dst;
	line = term.line[term.c.y];

	memmove(&line[dst], &line[src], size * sizeof(Glyph));
	tclearregion(src, term.c.y, dst - 1, term.c.y);
}

void
tinsertblankline(int n) {
	if(BETWEEN(term.c.y, term.top, term.bot))
		tscrolldown(term.c.y, n);
}

void
tdeleteline(int n) {
	if(BETWEEN(term.c.y, term.top, term.bot))
		tscrollup(term.c.y, n);
}

int32_t
tdefcolor(int *attr, int *npar, int l) {
	int32_t idx = -1;
	uint r, g, b;

	switch (attr[*npar + 1]) {
	case 2: /* direct color in RGB space */
		if (*npar + 4 >= l) {
			fprintf(stderr,
				"erresc(38): Incorrect number of parameters (%d)\n",
				*npar);
			break;
		}
		r = attr[*npar + 2];
		g = attr[*npar + 3];
		b = attr[*npar + 4];
		*npar += 4;
		if(!BETWEEN(r, 0, 255) || !BETWEEN(g, 0, 255) || !BETWEEN(b, 0, 255))
			fprintf(stderr, "erresc: bad rgb color (%d,%d,%d)\n",
				r, g, b);
		else
			idx = TRUECOLOR(r, g, b);
		break;
	case 5: /* indexed color */
		if (*npar + 2 >= l) {
			fprintf(stderr,
				"erresc(38): Incorrect number of parameters (%d)\n",
				*npar);
			break;
		}
		*npar += 2;
		if(!BETWEEN(attr[*npar], 0, 255))
			fprintf(stderr, "erresc: bad fgcolor %d\n", attr[*npar]);
		else
			idx = attr[*npar];
		break;
	case 0: /* implemented defined (only foreground) */
	case 1: /* transparent */
	case 3: /* direct color in CMY space */
	case 4: /* direct color in CMYK space */
	default:
		fprintf(stderr,
		        "erresc(38): gfx attr %d unknown\n", attr[*npar]);
		break;
	}

	return idx;
}

void
tsetattr(int *attr, int l) {
	int i;
	int32_t idx;

	for(i = 0; i < l; i++) {
		switch(attr[i]) {
		case 0:
			term.c.attr.mode &= ~(
				ATTR_BOLD       |
				ATTR_FAINT      |
				ATTR_ITALIC     |
				ATTR_UNDERLINE  |
				ATTR_BLINK      |
				ATTR_REVERSE    |
				ATTR_INVISIBLE  |
				ATTR_STRUCK     );
			term.c.attr.fg = defaultfg;
			term.c.attr.bg = defaultbg;
			break;
		case 1:
			term.c.attr.mode |= ATTR_BOLD;
			break;
		case 2:
			term.c.attr.mode |= ATTR_FAINT;
			break;
		case 3:
			term.c.attr.mode |= ATTR_ITALIC;
			break;
		case 4:
			term.c.attr.mode |= ATTR_UNDERLINE;
			break;
		case 5: /* slow blink */
			/* FALLTHROUGH */
		case 6: /* rapid blink */
			term.c.attr.mode |= ATTR_BLINK;
			break;
		case 7:
			term.c.attr.mode |= ATTR_REVERSE;
			break;
		case 8:
			term.c.attr.mode |= ATTR_INVISIBLE;
			break;
		case 9:
			term.c.attr.mode |= ATTR_STRUCK;
			break;
		case 22:
			term.c.attr.mode &= ~(ATTR_BOLD | ATTR_FAINT);
			break;
		case 23:
			term.c.attr.mode &= ~ATTR_ITALIC;
			break;
		case 24:
			term.c.attr.mode &= ~ATTR_UNDERLINE;
			break;
		case 25:
			term.c.attr.mode &= ~ATTR_BLINK;
			break;
		case 27:
			term.c.attr.mode &= ~ATTR_REVERSE;
			break;
		case 28:
			term.c.attr.mode &= ~ATTR_INVISIBLE;
			break;
		case 29:
			term.c.attr.mode &= ~ATTR_STRUCK;
			break;
		case 38:
			if ((idx = tdefcolor(attr, &i, l)) >= 0)
				term.c.attr.fg = idx;
			break;
		case 39:
			term.c.attr.fg = defaultfg;
			break;
		case 48:
			if ((idx = tdefcolor(attr, &i, l)) >= 0)
				term.c.attr.bg = idx;
			break;
		case 49:
			term.c.attr.bg = defaultbg;
			break;
		default:
			if(BETWEEN(attr[i], 30, 37)) {
				term.c.attr.fg = attr[i] - 30;
			} else if(BETWEEN(attr[i], 40, 47)) {
				term.c.attr.bg = attr[i] - 40;
			} else if(BETWEEN(attr[i], 90, 97)) {
				term.c.attr.fg = attr[i] - 90 + 8;
			} else if(BETWEEN(attr[i], 100, 107)) {
				term.c.attr.bg = attr[i] - 100 + 8;
			} else {
				fprintf(stderr,
					"erresc(default): gfx attr %d unknown\n",
					attr[i]), csidump();
			}
			break;
		}
	}
}

void
tsetscroll(int t, int b) {
	int temp;

	LIMIT(t, 0, term.row-1);
	LIMIT(b, 0, term.row-1);
	if(t > b) {
		temp = t;
		t = b;
		b = temp;
	}
	term.top = t;
	term.bot = b;
}

void
tsetmode(bool priv, bool set, int *args, int narg) {
	int *lim, mode;
	bool alt;

	for(lim = args + narg; args < lim; ++args) {
		if(priv) {
			switch(*args) {
			case 1: /* DECCKM -- Cursor key */
				MODBIT(term.mode, set, MODE_APPCURSOR);
				break;
			case 5: /* DECSCNM -- Reverse video */
				mode = term.mode;
				MODBIT(term.mode, set, MODE_REVERSE);
				if(mode != term.mode)
					redraw(REDRAW_TIMEOUT);
				break;
			case 6: /* DECOM -- Origin */
				MODBIT(term.c.state, set, CURSOR_ORIGIN);
				tmoveato(0, 0);
				break;
			case 7: /* DECAWM -- Auto wrap */
				MODBIT(term.mode, set, MODE_WRAP);
				break;
			case 0:  /* Error (IGNORED) */
			case 2:  /* DECANM -- ANSI/VT52 (IGNORED) */
			case 3:  /* DECCOLM -- Column  (IGNORED) */
			case 4:  /* DECSCLM -- Scroll (IGNORED) */
			case 8:  /* DECARM -- Auto repeat (IGNORED) */
			case 18: /* DECPFF -- Printer feed (IGNORED) */
			case 19: /* DECPEX -- Printer extent (IGNORED) */
			case 42: /* DECNRCM -- National characters (IGNORED) */
			case 12: /* att610 -- Start blinking cursor (IGNORED) */
				break;
			case 25: /* DECTCEM -- Text Cursor Enable Mode */
				MODBIT(term.mode, !set, MODE_HIDE);
				break;
			case 9:    /* X10 mouse compatibility mode */
				xsetpointermotion(0);
				MODBIT(term.mode, 0, MODE_MOUSE);
				MODBIT(term.mode, set, MODE_MOUSEX10);
				break;
			case 1000: /* 1000: report button press */
				xsetpointermotion(0);
				MODBIT(term.mode, 0, MODE_MOUSE);
				MODBIT(term.mode, set, MODE_MOUSEBTN);
				break;
			case 1002: /* 1002: report motion on button press */
				xsetpointermotion(0);
				MODBIT(term.mode, 0, MODE_MOUSE);
				MODBIT(term.mode, set, MODE_MOUSEMOTION);
				break;
			case 1003: /* 1003: enable all mouse motions */
				xsetpointermotion(set);
				MODBIT(term.mode, 0, MODE_MOUSE);
				MODBIT(term.mode, set, MODE_MOUSEMANY);
				break;
			case 1004: /* 1004: send focus events to tty */
				MODBIT(term.mode, set, MODE_FOCUS);
				break;
			case 1006: /* 1006: extended reporting mode */
				MODBIT(term.mode, set, MODE_MOUSESGR);
				break;
			case 1034:
				MODBIT(term.mode, set, MODE_8BIT);
				break;
			case 1049: /* swap screen & set/restore cursor as xterm */
				if (!allowaltscreen)
					break;
				tcursor((set) ? CURSOR_SAVE : CURSOR_LOAD);
				/* FALLTHROUGH */
			case 47: /* swap screen */
			case 1047:
				if (!allowaltscreen)
					break;
				alt = IS_SET(MODE_ALTSCREEN);
				if(alt) {
					tclearregion(0, 0, term.col-1,
							term.row-1);
				}
				if(set ^ alt) /* set is always 1 or 0 */
					tswapscreen();
				if(*args != 1049)
					break;
				/* FALLTHROUGH */
			case 1048:
				tcursor((set) ? CURSOR_SAVE : CURSOR_LOAD);
				break;
			case 2004: /* 2004: bracketed paste mode */
				MODBIT(term.mode, set, MODE_BRCKTPASTE);
				break;
			/* Not implemented mouse modes. See comments there. */
			case 1001: /* mouse highlight mode; can hang the
				      terminal by design when implemented. */
			case 1005: /* UTF-8 mouse mode; will confuse
				      applications not supporting UTF-8
				      and luit. */
			case 1015: /* urxvt mangled mouse mode; incompatible
				      and can be mistaken for other control
				      codes. */
			default:
				fprintf(stderr,
					"erresc: unknown private set/reset mode %d\n",
					*args);
				break;
			}
		} else {
			switch(*args) {
			case 0:  /* Error (IGNORED) */
				break;
			case 2:  /* KAM -- keyboard action */
				MODBIT(term.mode, set, MODE_KBDLOCK);
				break;
			case 4:  /* IRM -- Insertion-replacement */
				MODBIT(term.mode, set, MODE_INSERT);
				break;
			case 12: /* SRM -- Send/Receive */
				MODBIT(term.mode, !set, MODE_ECHO);
				break;
			case 20: /* LNM -- Linefeed/new line */
				MODBIT(term.mode, set, MODE_CRLF);
				break;
			default:
				fprintf(stderr,
					"erresc: unknown set/reset mode %d\n",
					*args);
				break;
			}
		}
	}
}

void
csihandle(void) {
	char buf[40];
	int len;

	switch(csiescseq.mode) {
	default:
	unknown:
		fprintf(stderr, "erresc: unknown csi ");
		csidump();
		/* die(""); */
		break;
	case '@': /* ICH -- Insert <n> blank char */
		DEFAULT(csiescseq.arg[0], 1);
		tinsertblank(csiescseq.arg[0]);
		break;
	case 'A': /* CUU -- Cursor <n> Up */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term.c.x, term.c.y-csiescseq.arg[0]);
		break;
	case 'B': /* CUD -- Cursor <n> Down */
	case 'e': /* VPR --Cursor <n> Down */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term.c.x, term.c.y+csiescseq.arg[0]);
		break;
	case 'i': /* MC -- Media Copy */
		switch(csiescseq.arg[0]) {
		case 0:
			tdump();
			break;
		case 1:
			tdumpline(term.c.y);
			break;
		case 2:
			tdumpsel();
			break;
		case 4:
			term.mode &= ~MODE_PRINT;
			break;
		case 5:
			term.mode |= MODE_PRINT;
			break;
		}
		break;
	case 'c': /* DA -- Device Attributes */
		if(csiescseq.arg[0] == 0)
			ttywrite(vtiden, sizeof(vtiden) - 1);
		break;
	case 'C': /* CUF -- Cursor <n> Forward */
	case 'a': /* HPR -- Cursor <n> Forward */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term.c.x+csiescseq.arg[0], term.c.y);
		break;
	case 'D': /* CUB -- Cursor <n> Backward */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term.c.x-csiescseq.arg[0], term.c.y);
		break;
	case 'E': /* CNL -- Cursor <n> Down and first col */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(0, term.c.y+csiescseq.arg[0]);
		break;
	case 'F': /* CPL -- Cursor <n> Up and first col */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(0, term.c.y-csiescseq.arg[0]);
		break;
	case 'g': /* TBC -- Tabulation clear */
		switch(csiescseq.arg[0]) {
		case 0: /* clear current tab stop */
			term.tabs[term.c.x] = 0;
			break;
		case 3: /* clear all the tabs */
			memset(term.tabs, 0, term.col * sizeof(*term.tabs));
			break;
		default:
			goto unknown;
		}
		break;
	case 'G': /* CHA -- Move to <col> */
	case '`': /* HPA */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(csiescseq.arg[0]-1, term.c.y);
		break;
	case 'H': /* CUP -- Move to <row> <col> */
	case 'f': /* HVP */
		DEFAULT(csiescseq.arg[0], 1);
		DEFAULT(csiescseq.arg[1], 1);
		tmoveato(csiescseq.arg[1]-1, csiescseq.arg[0]-1);
		break;
	case 'I': /* CHT -- Cursor Forward Tabulation <n> tab stops */
		DEFAULT(csiescseq.arg[0], 1);
		tputtab(csiescseq.arg[0]);
		break;
	case 'J': /* ED -- Clear screen */
		selclear(NULL);
		switch(csiescseq.arg[0]) {
		case 0: /* below */
			tclearregion(term.c.x, term.c.y, term.col-1, term.c.y);
			if(term.c.y < term.row-1) {
				tclearregion(0, term.c.y+1, term.col-1,
						term.row-1);
			}
			break;
		case 1: /* above */
			if(term.c.y > 1)
				tclearregion(0, 0, term.col-1, term.c.y-1);
			tclearregion(0, term.c.y, term.c.x, term.c.y);
			break;
		case 2: /* all */
			tclearregion(0, 0, term.col-1, term.row-1);
			break;
		default:
			goto unknown;
		}
		break;
	case 'K': /* EL -- Clear line */
		switch(csiescseq.arg[0]) {
		case 0: /* right */
			tclearregion(term.c.x, term.c.y, term.col-1,
					term.c.y);
			break;
		case 1: /* left */
			tclearregion(0, term.c.y, term.c.x, term.c.y);
			break;
		case 2: /* all */
			tclearregion(0, term.c.y, term.col-1, term.c.y);
			break;
		}
		break;
	case 'S': /* SU -- Scroll <n> line up */
		DEFAULT(csiescseq.arg[0], 1);
		tscrollup(term.top, csiescseq.arg[0]);
		break;
	case 'T': /* SD -- Scroll <n> line down */
		DEFAULT(csiescseq.arg[0], 1);
		tscrolldown(term.top, csiescseq.arg[0]);
		break;
	case 'L': /* IL -- Insert <n> blank lines */
		DEFAULT(csiescseq.arg[0], 1);
		tinsertblankline(csiescseq.arg[0]);
		break;
	case 'l': /* RM -- Reset Mode */
		tsetmode(csiescseq.priv, 0, csiescseq.arg, csiescseq.narg);
		break;
	case 'M': /* DL -- Delete <n> lines */
		DEFAULT(csiescseq.arg[0], 1);
		tdeleteline(csiescseq.arg[0]);
		break;
	case 'X': /* ECH -- Erase <n> char */
		DEFAULT(csiescseq.arg[0], 1);
		tclearregion(term.c.x, term.c.y,
				term.c.x + csiescseq.arg[0] - 1, term.c.y);
		break;
	case 'P': /* DCH -- Delete <n> char */
		DEFAULT(csiescseq.arg[0], 1);
		tdeletechar(csiescseq.arg[0]);
		break;
	case 'Z': /* CBT -- Cursor Backward Tabulation <n> tab stops */
		DEFAULT(csiescseq.arg[0], 1);
		tputtab(-csiescseq.arg[0]);
		break;
	case 'd': /* VPA -- Move to <row> */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveato(term.c.x, csiescseq.arg[0]-1);
		break;
	case 'h': /* SM -- Set terminal mode */
		tsetmode(csiescseq.priv, 1, csiescseq.arg, csiescseq.narg);
		break;
	case 'm': /* SGR -- Terminal attribute (color) */
		tsetattr(csiescseq.arg, csiescseq.narg);
		break;
	case 'n': /* DSR – Device Status Report (cursor position) */
		if (csiescseq.arg[0] == 6) {
			len = snprintf(buf, sizeof(buf),"\033[%i;%iR",
					term.c.y+1, term.c.x+1);
			ttywrite(buf, len);
		}
		break;
	case 'r': /* DECSTBM -- Set Scrolling Region */
		if(csiescseq.priv) {
			goto unknown;
		} else {
			DEFAULT(csiescseq.arg[0], 1);
			DEFAULT(csiescseq.arg[1], term.row);
			tsetscroll(csiescseq.arg[0]-1, csiescseq.arg[1]-1);
			tmoveato(0, 0);
		}
		break;
	case 's': /* DECSC -- Save cursor position (ANSI.SYS) */
		tcursor(CURSOR_SAVE);
		break;
	case 'u': /* DECRC -- Restore cursor position (ANSI.SYS) */
		tcursor(CURSOR_LOAD);
		break;
	}
}

void
csidump(void) {
	int i;
	uint c;

	printf("ESC[");
	for(i = 0; i < csiescseq.len; i++) {
		c = csiescseq.buf[i] & 0xff;
		if(isprint(c)) {
			putchar(c);
		} else if(c == '\n') {
			printf("(\\n)");
		} else if(c == '\r') {
			printf("(\\r)");
		} else if(c == 0x1b) {
			printf("(\\e)");
		} else {
			printf("(%02x)", c);
		}
	}
	putchar('\n');
}

void
csireset(void) {
	memset(&csiescseq, 0, sizeof(csiescseq));
}

void
strhandle(void) {
	char *p = NULL;
	int j, narg, par;

	term.esc &= ~(ESC_STR_END|ESC_STR);
	strparse();
	narg = strescseq.narg;
	par = atoi(strescseq.args[0]);

	switch(strescseq.type) {
	case ']': /* OSC -- Operating System Command */
		switch(par) {
		case 0:
		case 1:
		case 2:
			if(narg > 1)
				xsettitle(strescseq.args[1]);
			return;
		case 4: /* color set */
			if(narg < 3)
				break;
			p = strescseq.args[2];
			/* FALLTHROUGH */
		case 104: /* color reset, here p = NULL */
			j = (narg > 1) ? atoi(strescseq.args[1]) : -1;
			if(xsetcolorname(j, p)) {
				fprintf(stderr, "erresc: invalid color %s\n", p);
			} else {
				/*
				 * TODO if defaultbg color is changed, borders
				 * are dirty
				 */
				redraw(0);
			}
			return;
		}
		break;
	case 'k': /* old title set compatibility */
		xsettitle(strescseq.args[0]);
		return;
	case 'P': /* DCS -- Device Control String */
	case '_': /* APC -- Application Program Command */
	case '^': /* PM -- Privacy Message */
		return;
	}

	fprintf(stderr, "erresc: unknown str ");
	strdump();
}

void
strparse(void) {
	char *p = strescseq.buf;

	strescseq.narg = 0;
	strescseq.buf[strescseq.len] = '\0';
	while(p && strescseq.narg < STR_ARG_SIZ)
		strescseq.args[strescseq.narg++] = strsep(&p, ";");
}

void
strdump(void) {
	int i;
	uint c;

	printf("ESC%c", strescseq.type);
	for(i = 0; i < strescseq.len; i++) {
		c = strescseq.buf[i] & 0xff;
		if(c == '\0') {
			return;
		} else if(isprint(c)) {
			putchar(c);
		} else if(c == '\n') {
			printf("(\\n)");
		} else if(c == '\r') {
			printf("(\\r)");
		} else if(c == 0x1b) {
			printf("(\\e)");
		} else {
			printf("(%02x)", c);
		}
	}
	printf("ESC\\\n");
}

void
strreset(void) {
	memset(&strescseq, 0, sizeof(strescseq));
}

void
tprinter(char *s, size_t len) {
	if(iofd != -1 && xwrite(iofd, s, len) < 0) {
		fprintf(stderr, "Error writing in %s:%s\n",
			opt_io, strerror(errno));
		close(iofd);
		iofd = -1;
	}
}

void
toggleprinter(const Arg *arg) {
	term.mode ^= MODE_PRINT;
}

void
printscreen(const Arg *arg) {
	tdump();
}

void
printsel(const Arg *arg) {
	tdumpsel();
}

void
tdumpsel(void) {
	char *ptr;

	if((ptr = getsel())) {
		tprinter(ptr, strlen(ptr));
		free(ptr);
	}
}

void
tdumpline(int n) {
	Glyph *bp, *end;

	bp = &term.line[n][0];
	end = &bp[MIN(tlinelen(n), term.col) - 1];
	if(bp != end || bp->c[0] != ' ') {
		for( ;bp <= end; ++bp)
			tprinter(bp->c, utf8len(bp->c));
	}
	tprinter("\n", 1);
}

void
tdump(void) {
	int i;

	for(i = 0; i < term.row; ++i)
		tdumpline(i);
}

void
tputtab(int n) {
	uint x = term.c.x;

	if(n > 0) {
		while(x < term.col && n--)
			for(++x; x < term.col && !term.tabs[x]; ++x)
				/* nothing */ ;
	} else if(n < 0) {
		while(x > 0 && n++)
			for(--x; x > 0 && !term.tabs[x]; --x)
				/* nothing */ ;
	}
	tmoveto(x, term.c.y);
}

void
techo(char *buf, int len) {
	for(; len > 0; buf++, len--) {
		char c = *buf;

		if(ISCONTROL((uchar) c)) { /* control code */
			if(c & 0x80) {
				c &= 0x7f;
				tputc("^", 1);
				tputc("[", 1);
			} else if(c != '\n' && c != '\r' && c != '\t') {
				c ^= 0x40;
				tputc("^", 1);
			}
			tputc(&c, 1);
		} else {
			break;
		}
	}
	if(len)
		tputc(buf, len);
}

void
tdeftran(char ascii) {
	static char cs[] = "0B";
	static int vcs[] = {CS_GRAPHIC0, CS_USA};
	char *p;

	if((p = strchr(cs, ascii)) == NULL) {
		fprintf(stderr, "esc unhandled charset: ESC ( %c\n", ascii);
	} else {
		term.trantbl[term.icharset] = vcs[p - cs];
	}
}

void
tdectest(char c) {
	static char E[UTF_SIZ] = "E";
	int x, y;

	if(c == '8') { /* DEC screen alignment test. */
		for(x = 0; x < term.col; ++x) {
			for(y = 0; y < term.row; ++y)
				tsetchar(E, &term.c.attr, x, y);
		}
	}
}

void
tstrsequence(uchar c) {
	if (c & 0x80) {
		switch (c) {
		case 0x90:   /* DCS -- Device Control String */
			c = 'P';
			break;
		case 0x9f:   /* APC -- Application Program Command */
			c = '_';
			break;
		case 0x9e:   /* PM -- Privacy Message */
			c = '^';
			break;
		case 0x9d:   /* OSC -- Operating System Command */
			c = ']';
			break;
		}
	}
	strreset();
	strescseq.type = c;
	term.esc |= ESC_STR;
	return;
}

void
tcontrolcode(uchar ascii) {
	static char question[UTF_SIZ] = "?";

	switch(ascii) {
	case '\t':   /* HT */
		tputtab(1);
		return;
	case '\b':   /* BS */
		tmoveto(term.c.x-1, term.c.y);
		return;
	case '\r':   /* CR */
		tmoveto(0, term.c.y);
		return;
	case '\f':   /* LF */
	case '\v':   /* VT */
	case '\n':   /* LF */
		/* go to first col if the mode is set */
		tnewline(IS_SET(MODE_CRLF));
		return;
	case '\a':   /* BEL */
		if(term.esc & ESC_STR_END) {
			/* backwards compatibility to xterm */
			strhandle();
		} else {
			if(!(xw.state & WIN_FOCUSED))
				xseturgency(1);
			if (bellvolume)
				XkbBell(xw.dpy, xw.win, bellvolume, (Atom)NULL);
		}
		break;
	case '\033': /* ESC */
		csireset();
		term.esc &= ~(ESC_CSI|ESC_ALTCHARSET|ESC_TEST);
		term.esc |= ESC_START;
		return;
	case '\016': /* SO (LS1 -- Locking shift 1) */
	case '\017': /* SI (LS0 -- Locking shift 0) */
		term.charset = 1 - (ascii - '\016');
		return;
	case '\032': /* SUB */
		tsetchar(question, &term.c.attr, term.c.x, term.c.y);
	case '\030': /* CAN */
		csireset();
		break;
	case '\005': /* ENQ (IGNORED) */
	case '\000': /* NUL (IGNORED) */
	case '\021': /* XON (IGNORED) */
	case '\023': /* XOFF (IGNORED) */
	case 0177:   /* DEL (IGNORED) */
		return;
	case 0x84:   /* TODO: IND */
		break;
	case 0x85:   /* NEL -- Next line */
		tnewline(1); /* always go to first col */
		break;
	case 0x88:   /* HTS -- Horizontal tab stop */
		term.tabs[term.c.x] = 1;
		break;
	case 0x8d:   /* TODO: RI */
	case 0x8e:   /* TODO: SS2 */
	case 0x8f:   /* TODO: SS3 */
	case 0x98:   /* TODO: SOS */
		break;
	case 0x9a:   /* DECID -- Identify Terminal */
		ttywrite(vtiden, sizeof(vtiden) - 1);
		break;
	case 0x9b:   /* TODO: CSI */
	case 0x9c:   /* TODO: ST */
		break;
	case 0x90:   /* DCS -- Device Control String */
	case 0x9f:   /* APC -- Application Program Command */
	case 0x9e:   /* PM -- Privacy Message */
	case 0x9d:   /* OSC -- Operating System Command */
		tstrsequence(ascii);
		return;
	}
	/* only CAN, SUB, \a and C1 chars interrupt a sequence */
	term.esc &= ~(ESC_STR_END|ESC_STR);
	return;
}

/*
 * returns 1 when the sequence is finished and it hasn't to read
 * more characters for this sequence, otherwise 0
 */
int
eschandle(uchar ascii) {
	switch(ascii) {
	case '[':
		term.esc |= ESC_CSI;
		return 0;
	case '#':
		term.esc |= ESC_TEST;
		return 0;
	case 'P': /* DCS -- Device Control String */
	case '_': /* APC -- Application Program Command */
	case '^': /* PM -- Privacy Message */
	case ']': /* OSC -- Operating System Command */
	case 'k': /* old title set compatibility */
		tstrsequence(ascii);
		return 0;
	case 'n': /* LS2 -- Locking shift 2 */
	case 'o': /* LS3 -- Locking shift 3 */
		term.charset = 2 + (ascii - 'n');
		break;
	case '(': /* GZD4 -- set primary charset G0 */
	case ')': /* G1D4 -- set secondary charset G1 */
	case '*': /* G2D4 -- set tertiary charset G2 */
	case '+': /* G3D4 -- set quaternary charset G3 */
		term.icharset = ascii - '(';
		term.esc |= ESC_ALTCHARSET;
		return 0;
	case 'D': /* IND -- Linefeed */
		if(term.c.y == term.bot) {
			tscrollup(term.top, 1);
		} else {
			tmoveto(term.c.x, term.c.y+1);
		}
		break;
	case 'E': /* NEL -- Next line */
		tnewline(1); /* always go to first col */
		break;
	case 'H': /* HTS -- Horizontal tab stop */
		term.tabs[term.c.x] = 1;
		break;
	case 'M': /* RI -- Reverse index */
		if(term.c.y == term.top) {
			tscrolldown(term.top, 1);
		} else {
			tmoveto(term.c.x, term.c.y-1);
		}
		break;
	case 'Z': /* DECID -- Identify Terminal */
		ttywrite(vtiden, sizeof(vtiden) - 1);
		break;
	case 'c': /* RIS -- Reset to inital state */
		treset();
		xresettitle();
		xloadcols();
		break;
	case '=': /* DECPAM -- Application keypad */
		term.mode |= MODE_APPKEYPAD;
		break;
	case '>': /* DECPNM -- Normal keypad */
		term.mode &= ~MODE_APPKEYPAD;
		break;
	case '7': /* DECSC -- Save Cursor */
		tcursor(CURSOR_SAVE);
		break;
	case '8': /* DECRC -- Restore Cursor */
		tcursor(CURSOR_LOAD);
		break;
	case '\\': /* ST -- String Terminator */
		if(term.esc & ESC_STR_END)
			strhandle();
		break;
	default:
		fprintf(stderr, "erresc: unknown sequence ESC 0x%02X '%c'\n",
			(uchar) ascii, isprint(ascii)? ascii:'.');
		break;
	}
	return 1;
}

void
tputc(char *c, int len) {
	uchar ascii;
	bool control;
	long unicodep;
	int width;
	Glyph *gp;

	if(len == 1) {
		width = 1;
		unicodep = ascii = *c;
	} else {
		utf8decode(c, &unicodep, UTF_SIZ);
		if ((width = wcwidth(unicodep)) == -1) {
			c = "\357\277\275";	/* UTF_INVALID */
			width = 1;
		}
		control = ISCONTROLC1(unicodep);
		ascii = unicodep;
	}

	if(IS_SET(MODE_PRINT))
		tprinter(c, len);
	control = ISCONTROL(unicodep);

	/*
	 * STR sequence must be checked before anything else
	 * because it uses all following characters until it
	 * receives a ESC, a SUB, a ST or any other C1 control
	 * character.
	 */
	if(term.esc & ESC_STR) {
		if(width == 1 &&
		   (ascii == '\a' || ascii == 030 ||
		    ascii == 032  || ascii == 033 ||
		    ISCONTROLC1(unicodep))) {
			term.esc &= ~(ESC_START|ESC_STR);
			term.esc |= ESC_STR_END;
		} else if(strescseq.len + len < sizeof(strescseq.buf) - 1) {
			memmove(&strescseq.buf[strescseq.len], c, len);
			strescseq.len += len;
			return;
		} else {
		/*
		 * Here is a bug in terminals. If the user never sends
		 * some code to stop the str or esc command, then st
		 * will stop responding. But this is better than
		 * silently failing with unknown characters. At least
		 * then users will report back.
		 *
		 * In the case users ever get fixed, here is the code:
		 */
		/*
		 * term.esc = 0;
		 * strhandle();
		 */
			return;
		}
	}

	/*
	 * Actions of control codes must be performed as soon they arrive
	 * because they can be embedded inside a control sequence, and
	 * they must not cause conflicts with sequences.
	 */
	if(control) {
		tcontrolcode(ascii);
		/*
		 * control codes are not shown ever
		 */
		return;
	} else if(term.esc & ESC_START) {
		if(term.esc & ESC_CSI) {
			csiescseq.buf[csiescseq.len++] = ascii;
			if(BETWEEN(ascii, 0x40, 0x7E)
					|| csiescseq.len >= \
					sizeof(csiescseq.buf)-1) {
				term.esc = 0;
				csiparse();
				csihandle();
			}
			return;
		} else if(term.esc & ESC_ALTCHARSET) {
			tdeftran(ascii);
		} else if(term.esc & ESC_TEST) {
			tdectest(ascii);
		} else {
			if (!eschandle(ascii))
				return;
			/* sequence already finished */
		}
		term.esc = 0;
		/*
		 * All characters which form part of a sequence are not
		 * printed
		 */
		return;
	}
	if(sel.ob.x != -1 && BETWEEN(term.c.y, sel.ob.y, sel.oe.y))
		selclear(NULL);

	gp = &term.line[term.c.y][term.c.x];
	if(IS_SET(MODE_WRAP) && (term.c.state & CURSOR_WRAPNEXT)) {
		gp->mode |= ATTR_WRAP;
		tnewline(1);
	}

	if(IS_SET(MODE_INSERT) && term.c.x+1 < term.col)
		memmove(gp+1, gp, (term.col - term.c.x - 1) * sizeof(Glyph));

	if(term.c.x+width > term.col)
		tnewline(1);

	tsetchar(c, &term.c.attr, term.c.x, term.c.y);

	if(width == 2) {
		gp->mode |= ATTR_WIDE;
		if(term.c.x+1 < term.col) {
			gp[1].c[0] = '\0';
			gp[1].mode = ATTR_WDUMMY;
		}
	}
	if(term.c.x+width < term.col) {
		tmoveto(term.c.x+width, term.c.y);
	} else {
		term.c.state |= CURSOR_WRAPNEXT;
	}
}

void
tresize(int col, int row) {
	int i;
	int minrow = MIN(row, term.row);
	int mincol = MIN(col, term.col);
	int slide = term.c.y - row + 1;
	bool *bp;
	TCursor c;

	if(col < 1 || row < 1) {
		fprintf(stderr,
		        "tresize: error resizing to %dx%d\n", col, row);
		return;
	}

	/* free unneeded rows */
	i = 0;
	if(slide > 0) {
		/*
		 * slide screen to keep cursor where we expect it -
		 * tscrollup would work here, but we can optimize to
		 * memmove because we're freeing the earlier lines
		 */
		for(/* i = 0 */; i < slide; i++) {
			free(term.line[i]);
			free(term.alt[i]);
		}
		memmove(term.line, term.line + slide, row * sizeof(Line));
		memmove(term.alt, term.alt + slide, row * sizeof(Line));
	}
	for(i += row; i < term.row; i++) {
		free(term.line[i]);
		free(term.alt[i]);
	}

	/* resize to new height */
	term.line = xrealloc(term.line, row * sizeof(Line));
	term.alt  = xrealloc(term.alt,  row * sizeof(Line));
	term.dirty = xrealloc(term.dirty, row * sizeof(*term.dirty));
	term.tabs = xrealloc(term.tabs, col * sizeof(*term.tabs));

	/* resize each row to new width, zero-pad if needed */
	for(i = 0; i < minrow; i++) {
		term.line[i] = xrealloc(term.line[i], col * sizeof(Glyph));
		term.alt[i]  = xrealloc(term.alt[i],  col * sizeof(Glyph));
	}

	/* allocate any new rows */
	for(/* i == minrow */; i < row; i++) {
		term.line[i] = xmalloc(col * sizeof(Glyph));
		term.alt[i] = xmalloc(col * sizeof(Glyph));
	}
	if(col > term.col) {
		bp = term.tabs + term.col;

		memset(bp, 0, sizeof(*term.tabs) * (col - term.col));
		while(--bp > term.tabs && !*bp)
			/* nothing */ ;
		for(bp += tabspaces; bp < term.tabs + col; bp += tabspaces)
			*bp = 1;
	}
	/* update terminal size */
	term.col = col;
	term.row = row;
	/* reset scrolling region */
	tsetscroll(0, row-1);
	/* make use of the LIMIT in tmoveto */
	tmoveto(term.c.x, term.c.y);
	/* Clearing both screens (it makes dirty all lines) */
	c = term.c;
	for(i = 0; i < 2; i++) {
		if(mincol < col && 0 < minrow) {
			tclearregion(mincol, 0, col - 1, minrow - 1);
		}
		if(0 < col && minrow < row) {
			tclearregion(0, minrow, col - 1, row - 1);
		}
		tswapscreen();
		tcursor(CURSOR_LOAD);
	}
	term.c = c;
}

void
xresize(int col, int row) {
	xw.tw = MAX(1, col * xw.cw);
	xw.th = MAX(1, row * xw.ch);

	XFreePixmap(xw.dpy, xw.buf);
	xw.buf = XCreatePixmap(xw.dpy, xw.win, xw.w, xw.h,
			DefaultDepth(xw.dpy, xw.scr));
	XftDrawChange(xw.draw, xw.buf);
	xclear(0, 0, xw.w, xw.h);
}

static inline ushort
sixd_to_16bit(int x) {
	return x == 0 ? 0 : 0x3737 + 0x2828 * x;
}

void
xloadcols(void) {
	int i;
	XRenderColor color = { .alpha = 0xffff };
	static bool loaded;
	Color *cp;

	if(loaded) {
		for (cp = dc.col; cp < dc.col + LEN(dc.col); ++cp)
			XftColorFree(xw.dpy, xw.vis, xw.cmap, cp);
	}

	/* load colors [0-15] and [256-LEN(colorname)] (config.h) */
	for(i = 0; i < LEN(colorname); i++) {
		if(!colorname[i])
			continue;
		if(!XftColorAllocName(xw.dpy, xw.vis, xw.cmap, colorname[i], &dc.col[i])) {
			die("Could not allocate color '%s'\n", colorname[i]);
		}
	}

	/* load colors [16-231] ; same colors as xterm */
	for(i = 16; i < 6*6*6+16; i++) {
		color.red   = sixd_to_16bit( ((i-16)/36)%6 );
		color.green = sixd_to_16bit( ((i-16)/6) %6 );
		color.blue  = sixd_to_16bit( ((i-16)/1) %6 );
		if(!XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &color, &dc.col[i]))
			die("Could not allocate color %d\n", i);
	}

	/* load colors [232-255] ; grayscale */
	for(; i < 256; i++) {
		color.red = color.green = color.blue = 0x0808 + 0x0a0a * (i-(6*6*6+16));
		if(!XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &color, &dc.col[i]))
			die("Could not allocate color %d\n", i);
	}
	loaded = true;
}

int
xsetcolorname(int x, const char *name) {
	XRenderColor color = { .alpha = 0xffff };
	Color ncolor;

	if(!BETWEEN(x, 0, LEN(colorname)))
		return 1;

	if(!name) {
		if(BETWEEN(x, 16, 16 + 215)) { /* 256 color */
			color.red   = sixd_to_16bit( ((x-16)/36)%6 );
			color.green = sixd_to_16bit( ((x-16)/6) %6 );
			color.blue  = sixd_to_16bit( ((x-16)/1) %6 );
			if(!XftColorAllocValue(xw.dpy, xw.vis,
						xw.cmap, &color, &ncolor)) {
				return 1;
			}

			XftColorFree(xw.dpy, xw.vis, xw.cmap, &dc.col[x]);
			dc.col[x] = ncolor;
			return 0;
		} else if(BETWEEN(x, 16 + 216, 255)) { /* greyscale */
			color.red = color.green = color.blue = \
				    0x0808 + 0x0a0a * (x - (16 + 216));
			if(!XftColorAllocValue(xw.dpy, xw.vis,
						xw.cmap, &color, &ncolor)) {
				return 1;
			}

			XftColorFree(xw.dpy, xw.vis, xw.cmap, &dc.col[x]);
			dc.col[x] = ncolor;
			return 0;
		} else { /* system colors */
			name = colorname[x];
		}
	}
	if(!XftColorAllocName(xw.dpy, xw.vis, xw.cmap, name, &ncolor))
		return 1;

	XftColorFree(xw.dpy, xw.vis, xw.cmap, &dc.col[x]);
	dc.col[x] = ncolor;
	return 0;
}

void
xtermclear(int col1, int row1, int col2, int row2) {
	XftDrawRect(xw.draw,
			&dc.col[IS_SET(MODE_REVERSE) ? defaultfg : defaultbg],
			borderpx + col1 * xw.cw,
			borderpx + row1 * xw.ch,
			(col2-col1+1) * xw.cw,
			(row2-row1+1) * xw.ch);
}

/*
 * Absolute coordinates.
 */
void
xclear(int x1, int y1, int x2, int y2) {
	XftDrawRect(xw.draw,
			&dc.col[IS_SET(MODE_REVERSE)? defaultfg : defaultbg],
			x1, y1, x2-x1, y2-y1);
}

void
xhints(void) {
	XClassHint class = {opt_class ? opt_class : termname, termname};
	XWMHints wm = {.flags = InputHint, .input = 1};
	XSizeHints *sizeh = NULL;

	sizeh = XAllocSizeHints();

	sizeh->flags = PSize | PResizeInc | PBaseSize;
	sizeh->height = xw.h;
	sizeh->width = xw.w;
	sizeh->height_inc = xw.ch;
	sizeh->width_inc = xw.cw;
	sizeh->base_height = 2 * borderpx;
	sizeh->base_width = 2 * borderpx;
	if(xw.isfixed == True) {
		sizeh->flags |= PMaxSize | PMinSize;
		sizeh->min_width = sizeh->max_width = xw.w;
		sizeh->min_height = sizeh->max_height = xw.h;
	}
	if(xw.gm & (XValue|YValue)) {
		sizeh->flags |= USPosition | PWinGravity;
		sizeh->x = xw.l;
		sizeh->y = xw.t;
		sizeh->win_gravity = xgeommasktogravity(xw.gm);
	}

	XSetWMProperties(xw.dpy, xw.win, NULL, NULL, NULL, 0, sizeh, &wm,
			&class);
	XFree(sizeh);
}

int
xgeommasktogravity(int mask) {
	switch(mask & (XNegative|YNegative)) {
	case 0:
		return NorthWestGravity;
	case XNegative:
		return NorthEastGravity;
	case YNegative:
		return SouthWestGravity;
	}
	return SouthEastGravity;
}

int
xloadfont(Font *f, FcPattern *pattern) {
	FcPattern *match;
	FcResult result;

	match = FcFontMatch(NULL, pattern, &result);
	if(!match)
		return 1;

	if(!(f->match = XftFontOpenPattern(xw.dpy, match))) {
		FcPatternDestroy(match);
		return 1;
	}

	f->set = NULL;
	f->pattern = FcPatternDuplicate(pattern);

	f->ascent = f->match->ascent;
	f->descent = f->match->descent;
	f->lbearing = 0;
	f->rbearing = f->match->max_advance_width;

	f->height = f->ascent + f->descent;
	f->width = f->lbearing + f->rbearing;

	return 0;
}

void
xloadfonts(char *fontstr, double fontsize) {
	FcPattern *pattern;
	FcResult r_sz, r_psz;
	double fontval;
	float ceilf(float);

	if(fontstr[0] == '-') {
		pattern = XftXlfdParse(fontstr, False, False);
	} else {
		pattern = FcNameParse((FcChar8 *)fontstr);
	}

	if(!pattern)
		die("st: can't open font %s\n", fontstr);

	if(fontsize > 0) {
		FcPatternDel(pattern, FC_PIXEL_SIZE);
		FcPatternDel(pattern, FC_SIZE);
		FcPatternAddDouble(pattern, FC_PIXEL_SIZE, (double)fontsize);
		usedfontsize = fontsize;
	} else {
		r_psz = FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &fontval);
		r_sz = FcPatternGetDouble(pattern, FC_SIZE, 0, &fontval);
		if(r_psz == FcResultMatch) {
			usedfontsize = fontval;
		} else if(r_sz == FcResultMatch) {
			usedfontsize = -1;
		} else {
			/*
			 * Default font size is 12, if none given. This is to
			 * have a known usedfontsize value.
			 */
			FcPatternAddDouble(pattern, FC_PIXEL_SIZE, 12);
			usedfontsize = 12;
		}
		defaultfontsize = usedfontsize;
	}

	FcConfigSubstitute(0, pattern, FcMatchPattern);
	FcDefaultSubstitute(pattern);

	if(xloadfont(&dc.font, pattern))
		die("st: can't open font %s\n", fontstr);

	if(usedfontsize < 0) {
		FcPatternGetDouble(dc.font.match->pattern,
		                   FC_PIXEL_SIZE, 0, &fontval);
		usedfontsize = fontval;
		if(fontsize == 0)
			defaultfontsize = fontval;
	}

	/* Setting character width and height. */
	xw.cw = ceilf(dc.font.width * cwscale);
	xw.ch = ceilf(dc.font.height * chscale);

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ITALIC);
	if(xloadfont(&dc.ifont, pattern))
		die("st: can't open font %s\n", fontstr);

	FcPatternDel(pattern, FC_WEIGHT);
	FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
	if(xloadfont(&dc.ibfont, pattern))
		die("st: can't open font %s\n", fontstr);

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
	if(xloadfont(&dc.bfont, pattern))
		die("st: can't open font %s\n", fontstr);

	FcPatternDestroy(pattern);
}

int
xloadfontset(Font *f) {
	FcResult result;

	if(!(f->set = FcFontSort(0, f->pattern, FcTrue, 0, &result)))
		return 1;
	return 0;
}

void
xunloadfont(Font *f) {
	XftFontClose(xw.dpy, f->match);
	FcPatternDestroy(f->pattern);
	if(f->set)
		FcFontSetDestroy(f->set);
}

void
xunloadfonts(void) {
	/* Free the loaded fonts in the font cache.  */
	while(frclen > 0)
		XftFontClose(xw.dpy, frc[--frclen].font);

	xunloadfont(&dc.font);
	xunloadfont(&dc.bfont);
	xunloadfont(&dc.ifont);
	xunloadfont(&dc.ibfont);
}

void
xzoom(const Arg *arg) {
	Arg larg;

	larg.i = usedfontsize + arg->i;
	xzoomabs(&larg);
}

void
xzoomabs(const Arg *arg) {
	xunloadfonts();
	xloadfonts(usedfont, arg->i);
	cresize(0, 0);
	redraw(0);
	xhints();
}

void
xzoomreset(const Arg *arg) {
	Arg larg;

	if(defaultfontsize > 0) {
		larg.i = defaultfontsize;
		xzoomabs(&larg);
	}
}

void
xinit(void) {
	XGCValues gcvalues;
	Cursor cursor;
	Window parent;
	pid_t thispid = getpid();

	if(!(xw.dpy = XOpenDisplay(NULL)))
		die("Can't open display\n");
	xw.scr = XDefaultScreen(xw.dpy);
	xw.vis = XDefaultVisual(xw.dpy, xw.scr);

	/* font */
	if(!FcInit())
		die("Could not init fontconfig.\n");

	usedfont = (opt_font == NULL)? font : opt_font;
	xloadfonts(usedfont, 0);

	/* colors */
	xw.cmap = XDefaultColormap(xw.dpy, xw.scr);
	xloadcols();

	/* adjust fixed window geometry */
	xw.w = 2 * borderpx + term.col * xw.cw;
	xw.h = 2 * borderpx + term.row * xw.ch;
	if(xw.gm & XNegative)
		xw.l += DisplayWidth(xw.dpy, xw.scr) - xw.w - 2;
	if(xw.gm & YNegative)
		xw.t += DisplayWidth(xw.dpy, xw.scr) - xw.h - 2;

	/* Events */
	xw.attrs.background_pixel = dc.col[defaultbg].pixel;
	xw.attrs.border_pixel = dc.col[defaultbg].pixel;
	xw.attrs.bit_gravity = NorthWestGravity;
	xw.attrs.event_mask = FocusChangeMask | KeyPressMask
		| ExposureMask | VisibilityChangeMask | StructureNotifyMask
		| ButtonMotionMask | ButtonPressMask | ButtonReleaseMask;
	xw.attrs.colormap = xw.cmap;

	if (!(opt_embed && (parent = strtol(opt_embed, NULL, 0))))
		parent = XRootWindow(xw.dpy, xw.scr);
	xw.win = XCreateWindow(xw.dpy, parent, xw.l, xw.t,
			xw.w, xw.h, 0, XDefaultDepth(xw.dpy, xw.scr), InputOutput,
			xw.vis, CWBackPixel | CWBorderPixel | CWBitGravity
			| CWEventMask | CWColormap, &xw.attrs);

	memset(&gcvalues, 0, sizeof(gcvalues));
	gcvalues.graphics_exposures = False;
	dc.gc = XCreateGC(xw.dpy, parent, GCGraphicsExposures,
			&gcvalues);
	xw.buf = XCreatePixmap(xw.dpy, xw.win, xw.w, xw.h,
			DefaultDepth(xw.dpy, xw.scr));
	XSetForeground(xw.dpy, dc.gc, dc.col[defaultbg].pixel);
	XFillRectangle(xw.dpy, xw.buf, dc.gc, 0, 0, xw.w, xw.h);

	/* Xft rendering context */
	xw.draw = XftDrawCreate(xw.dpy, xw.buf, xw.vis, xw.cmap);

	/* input methods */
	if((xw.xim = XOpenIM(xw.dpy, NULL, NULL, NULL)) == NULL) {
		XSetLocaleModifiers("@im=local");
		if((xw.xim =  XOpenIM(xw.dpy, NULL, NULL, NULL)) == NULL) {
			XSetLocaleModifiers("@im=");
			if((xw.xim = XOpenIM(xw.dpy,
					NULL, NULL, NULL)) == NULL) {
				die("XOpenIM failed. Could not open input"
					" device.\n");
			}
		}
	}
	xw.xic = XCreateIC(xw.xim, XNInputStyle, XIMPreeditNothing
					   | XIMStatusNothing, XNClientWindow, xw.win,
					   XNFocusWindow, xw.win, NULL);
	if(xw.xic == NULL)
		die("XCreateIC failed. Could not obtain input method.\n");

	/* white cursor, black outline */
	cursor = XCreateFontCursor(xw.dpy, XC_xterm);
	XDefineCursor(xw.dpy, xw.win, cursor);
	XRecolorCursor(xw.dpy, cursor,
		&(XColor){.red = 0xffff, .green = 0xffff, .blue = 0xffff},
		&(XColor){.red = 0x0000, .green = 0x0000, .blue = 0x0000});

	xw.xembed = XInternAtom(xw.dpy, "_XEMBED", False);
	xw.wmdeletewin = XInternAtom(xw.dpy, "WM_DELETE_WINDOW", False);
	xw.netwmname = XInternAtom(xw.dpy, "_NET_WM_NAME", False);
	XSetWMProtocols(xw.dpy, xw.win, &xw.wmdeletewin, 1);

	xw.netwmpid = XInternAtom(xw.dpy, "_NET_WM_PID", False);
	XChangeProperty(xw.dpy, xw.win, xw.netwmpid, XA_CARDINAL, 32,
			PropModeReplace, (uchar *)&thispid, 1);

	xresettitle();
	XMapWindow(xw.dpy, xw.win);
	xhints();
	XSync(xw.dpy, False);
}

void
xdraws(char *s, Glyph base, int x, int y, int charlen, int bytelen) {
	int winx = borderpx + x * xw.cw, winy = borderpx + y * xw.ch,
	    width = charlen * xw.cw, xp, i;
	int frcflags;
	int u8fl, u8fblen, u8cblen, doesexist;
	char *u8c, *u8fs;
	long unicodep;
	Font *font = &dc.font;
	FcResult fcres;
	FcPattern *fcpattern, *fontpattern;
	FcFontSet *fcsets[] = { NULL };
	FcCharSet *fccharset;
	Color *fg, *bg, *temp, revfg, revbg, truefg, truebg;
	XRenderColor colfg, colbg;
	XRectangle r;
	int oneatatime;

	frcflags = FRC_NORMAL;

	if(base.mode & ATTR_ITALIC) {
		if(base.fg == defaultfg)
			base.fg = defaultitalic;
		font = &dc.ifont;
		frcflags = FRC_ITALIC;
	} else if((base.mode & ATTR_ITALIC) && (base.mode & ATTR_BOLD)) {
		if(base.fg == defaultfg)
			base.fg = defaultitalic;
		font = &dc.ibfont;
		frcflags = FRC_ITALICBOLD;
	} else if(base.mode & ATTR_UNDERLINE) {
		if(base.fg == defaultfg)
			base.fg = defaultunderline;
	}

	if(IS_TRUECOL(base.fg)) {
		colfg.alpha = 0xffff;
		colfg.red = TRUERED(base.fg);
		colfg.green = TRUEGREEN(base.fg);
		colfg.blue = TRUEBLUE(base.fg);
		XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, &truefg);
		fg = &truefg;
	} else {
		fg = &dc.col[base.fg];
	}

	if(IS_TRUECOL(base.bg)) {
		colbg.alpha = 0xffff;
		colbg.green = TRUEGREEN(base.bg);
		colbg.red = TRUERED(base.bg);
		colbg.blue = TRUEBLUE(base.bg);
		XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colbg, &truebg);
		bg = &truebg;
	} else {
		bg = &dc.col[base.bg];
	}

	if(base.mode & ATTR_BOLD) {
		/*
		 * change basic system colors [0-7]
		 * to bright system colors [8-15]
		 */
		if(BETWEEN(base.fg, 0, 7) && !(base.mode & ATTR_FAINT))
			fg = &dc.col[base.fg + 8];

		if(base.mode & ATTR_ITALIC) {
			font = &dc.ibfont;
			frcflags = FRC_ITALICBOLD;
		} else {
			font = &dc.bfont;
			frcflags = FRC_BOLD;
		}
	}

	if(IS_SET(MODE_REVERSE)) {
		if(fg == &dc.col[defaultfg]) {
			fg = &dc.col[defaultbg];
		} else {
			colfg.red = ~fg->color.red;
			colfg.green = ~fg->color.green;
			colfg.blue = ~fg->color.blue;
			colfg.alpha = fg->color.alpha;
			XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg,
					&revfg);
			fg = &revfg;
		}

		if(bg == &dc.col[defaultbg]) {
			bg = &dc.col[defaultfg];
		} else {
			colbg.red = ~bg->color.red;
			colbg.green = ~bg->color.green;
			colbg.blue = ~bg->color.blue;
			colbg.alpha = bg->color.alpha;
			XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colbg,
					&revbg);
			bg = &revbg;
		}
	}

	if(base.mode & ATTR_REVERSE) {
		temp = fg;
		fg = bg;
		bg = temp;
	}

	if(base.mode & ATTR_FAINT && !(base.mode & ATTR_BOLD)) {
		colfg.red = fg->color.red / 2;
		colfg.green = fg->color.green / 2;
		colfg.blue = fg->color.blue / 2;
		XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, &revfg);
		fg = &revfg;
	}

	if(base.mode & ATTR_BLINK && term.mode & MODE_BLINK)
		fg = bg;

	if(base.mode & ATTR_INVISIBLE)
		fg = bg;

	/* Intelligent cleaning up of the borders. */
	if(x == 0) {
		xclear(0, (y == 0)? 0 : winy, borderpx,
			winy + xw.ch + ((y >= term.row-1)? xw.h : 0));
	}
	if(x + charlen >= term.col) {
		xclear(winx + width, (y == 0)? 0 : winy, xw.w,
			((y >= term.row-1)? xw.h : (winy + xw.ch)));
	}
	if(y == 0)
		xclear(winx, 0, winx + width, borderpx);
	if(y == term.row-1)
		xclear(winx, winy + xw.ch, winx + width, xw.h);

	/* Clean up the region we want to draw to. */
	XftDrawRect(xw.draw, bg, winx, winy, width, xw.ch);

	/* Set the clip region because Xft is sometimes dirty. */
	r.x = 0;
	r.y = 0;
	r.height = xw.ch;
	r.width = width;
	XftDrawSetClipRectangles(xw.draw, winx, winy, &r, 1);

	for(xp = winx; bytelen > 0;) {
		/*
		 * Search for the range in the to be printed string of glyphs
		 * that are in the main font. Then print that range. If
		 * some glyph is found that is not in the font, do the
		 * fallback dance.
		 */
		u8fs = s;
		u8fblen = 0;
		u8fl = 0;
		oneatatime = font->width != xw.cw;
		for(;;) {
			u8c = s;
			u8cblen = utf8decode(s, &unicodep, UTF_SIZ);
			s += u8cblen;
			bytelen -= u8cblen;

			doesexist = XftCharExists(xw.dpy, font->match, unicodep);
			if(doesexist) {
					u8fl++;
					u8fblen += u8cblen;
					if(!oneatatime && bytelen > 0)
							continue;
			}

			if(u8fl > 0) {
				XftDrawStringUtf8(xw.draw, fg,
						font->match, xp,
						winy + font->ascent,
						(FcChar8 *)u8fs,
						u8fblen);
				xp += xw.cw * u8fl;
			}
			break;
		}
		if(doesexist) {
			if(oneatatime)
				continue;
			break;
		}

		/* Search the font cache. */
		for(i = 0; i < frclen; i++) {
			if(XftCharExists(xw.dpy, frc[i].font, unicodep)
					&& frc[i].flags == frcflags) {
				break;
			}
		}

		/* Nothing was found. */
		if(i >= frclen) {
			if(!font->set)
				xloadfontset(font);
			fcsets[0] = font->set;

			/*
			 * Nothing was found in the cache. Now use
			 * some dozen of Fontconfig calls to get the
			 * font for one single character.
			 *
			 * Xft and fontconfig are design failures.
			 */
			fcpattern = FcPatternDuplicate(font->pattern);
			fccharset = FcCharSetCreate();

			FcCharSetAddChar(fccharset, unicodep);
			FcPatternAddCharSet(fcpattern, FC_CHARSET,
					fccharset);
			FcPatternAddBool(fcpattern, FC_SCALABLE,
					FcTrue);

			FcConfigSubstitute(0, fcpattern,
					FcMatchPattern);
			FcDefaultSubstitute(fcpattern);

			fontpattern = FcFontSetMatch(0, fcsets,
					FcTrue, fcpattern, &fcres);

			/*
			 * Overwrite or create the new cache entry.
			 */
			if(frclen >= LEN(frc)) {
				frclen = LEN(frc) - 1;
				XftFontClose(xw.dpy, frc[frclen].font);
			}

			frc[frclen].font = XftFontOpenPattern(xw.dpy,
					fontpattern);
			frc[frclen].flags = frcflags;

			i = frclen;
			frclen++;

			FcPatternDestroy(fcpattern);
			FcCharSetDestroy(fccharset);
		}

		XftDrawStringUtf8(xw.draw, fg, frc[i].font,
				xp, winy + frc[i].font->ascent,
				(FcChar8 *)u8c, u8cblen);

		xp += xw.cw * wcwidth(unicodep);
	}

	/*
	 * This is how the loop above actually should be. Why does the
	 * application have to care about font details?
	 *
	 * I have to repeat: Xft and Fontconfig are design failures.
	 */
	/*
	XftDrawStringUtf8(xw.draw, fg, font->set, winx,
			winy + font->ascent, (FcChar8 *)s, bytelen);
	*/

	if(base.mode & ATTR_UNDERLINE) {
		XftDrawRect(xw.draw, fg, winx, winy + font->ascent + 1,
				width, 1);
	}

	if(base.mode & ATTR_STRUCK) {
		XftDrawRect(xw.draw, fg, winx, winy + 2 * font->ascent / 3,
				width, 1);
	}

	/* Reset clip to none. */
	XftDrawSetClip(xw.draw, 0);
}

void
xdrawcursor(void) {
	static int oldx = 0, oldy = 0;
	int sl, width, curx;
	Glyph g = {{' '}, ATTR_NULL, defaultbg, defaultcs};

	LIMIT(oldx, 0, term.col-1);
	LIMIT(oldy, 0, term.row-1);

	curx = term.c.x;

	/* adjust position if in dummy */
	if(term.line[oldy][oldx].mode & ATTR_WDUMMY)
		oldx--;
	if(term.line[term.c.y][curx].mode & ATTR_WDUMMY)
		curx--;

	memcpy(g.c, term.line[term.c.y][term.c.x].c, UTF_SIZ);

	/* remove the old cursor */
	sl = utf8len(term.line[oldy][oldx].c);
	width = (term.line[oldy][oldx].mode & ATTR_WIDE)? 2 : 1;
	xdraws(term.line[oldy][oldx].c, term.line[oldy][oldx], oldx,
			oldy, width, sl);

	if(IS_SET(MODE_HIDE))
		return;

	/* draw the new one */
	if(xw.state & WIN_FOCUSED) {
		if(IS_SET(MODE_REVERSE)) {
			g.mode |= ATTR_REVERSE;
			g.fg = defaultcs;
			g.bg = defaultfg;
		}

		sl = utf8len(g.c);
		width = (term.line[term.c.y][curx].mode & ATTR_WIDE)\
			? 2 : 1;
		xdraws(g.c, g, term.c.x, term.c.y, width, sl);
	} else {
		XftDrawRect(xw.draw, &dc.col[defaultcs],
				borderpx + curx * xw.cw,
				borderpx + term.c.y * xw.ch,
				xw.cw - 1, 1);
		XftDrawRect(xw.draw, &dc.col[defaultcs],
				borderpx + curx * xw.cw,
				borderpx + term.c.y * xw.ch,
				1, xw.ch - 1);
		XftDrawRect(xw.draw, &dc.col[defaultcs],
				borderpx + (curx + 1) * xw.cw - 1,
				borderpx + term.c.y * xw.ch,
				1, xw.ch - 1);
		XftDrawRect(xw.draw, &dc.col[defaultcs],
				borderpx + curx * xw.cw,
				borderpx + (term.c.y + 1) * xw.ch - 1,
				xw.cw, 1);
	}
	oldx = curx, oldy = term.c.y;
}


void
xsettitle(char *p) {
	XTextProperty prop;

	Xutf8TextListToTextProperty(xw.dpy, &p, 1, XUTF8StringStyle,
			&prop);
	XSetWMName(xw.dpy, xw.win, &prop);
	XSetTextProperty(xw.dpy, xw.win, &prop, xw.netwmname);
	XFree(prop.value);
}

void
xresettitle(void) {
	xsettitle(opt_title ? opt_title : "st");
}

void
redraw(int timeout) {
	struct timespec tv = {0, timeout * 1000};

	tfulldirt();
	draw();

	if(timeout > 0) {
		nanosleep(&tv, NULL);
		XSync(xw.dpy, False); /* necessary for a good tput flash */
	}
}

void
draw(void) {
	drawregion(0, 0, term.col, term.row);
	XCopyArea(xw.dpy, xw.buf, xw.win, dc.gc, 0, 0, xw.w,
			xw.h, 0, 0);
	XSetForeground(xw.dpy, dc.gc,
			dc.col[IS_SET(MODE_REVERSE)?
				defaultfg : defaultbg].pixel);
}

void
drawregion(int x1, int y1, int x2, int y2) {
	int ic, ib, x, y, ox, sl;
	Glyph base, new;
	char buf[DRAW_BUF_SIZ];
	bool ena_sel = sel.ob.x != -1 && sel.alt == IS_SET(MODE_ALTSCREEN);
	long unicodep;

	if(!(xw.state & WIN_VISIBLE))
		return;

	for(y = y1; y < y2; y++) {
		if(!term.dirty[y])
			continue;

		xtermclear(0, y, term.col, y);
		term.dirty[y] = 0;
		base = term.line[y][0];
		ic = ib = ox = 0;
		for(x = x1; x < x2; x++) {
			new = term.line[y][x];
			if(new.mode == ATTR_WDUMMY)
				continue;
			if(ena_sel && selected(x, y))
				new.mode ^= ATTR_REVERSE;
			if(ib > 0 && (ATTRCMP(base, new)
					|| ib >= DRAW_BUF_SIZ-UTF_SIZ)) {
				xdraws(buf, base, ox, y, ic, ib);
				ic = ib = 0;
			}
			if(ib == 0) {
				ox = x;
				base = new;
			}

			sl = utf8decode(new.c, &unicodep, UTF_SIZ);
			memcpy(buf+ib, new.c, sl);
			ib += sl;
			ic += (new.mode & ATTR_WIDE)? 2 : 1;
		}
		if(ib > 0)
			xdraws(buf, base, ox, y, ic, ib);
	}
	xdrawcursor();
}

void
expose(XEvent *ev) {
	XExposeEvent *e = &ev->xexpose;

	if(xw.state & WIN_REDRAW) {
		if(!e->count)
			xw.state &= ~WIN_REDRAW;
	}
	redraw(0);
}

void
visibility(XEvent *ev) {
	XVisibilityEvent *e = &ev->xvisibility;

	if(e->state == VisibilityFullyObscured) {
		xw.state &= ~WIN_VISIBLE;
	} else if(!(xw.state & WIN_VISIBLE)) {
		/* need a full redraw for next Expose, not just a buf copy */
		xw.state |= WIN_VISIBLE | WIN_REDRAW;
	}
}

void
unmap(XEvent *ev) {
	xw.state &= ~WIN_VISIBLE;
}

void
xsetpointermotion(int set) {
	MODBIT(xw.attrs.event_mask, set, PointerMotionMask);
	XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask, &xw.attrs);
}

void
xseturgency(int add) {
	XWMHints *h = XGetWMHints(xw.dpy, xw.win);

	MODBIT(h->flags, add, XUrgencyHint);
	XSetWMHints(xw.dpy, xw.win, h);
	XFree(h);
}

void
focus(XEvent *ev) {
	XFocusChangeEvent *e = &ev->xfocus;

	if(e->mode == NotifyGrab)
		return;

	if(ev->type == FocusIn) {
		XSetICFocus(xw.xic);
		xw.state |= WIN_FOCUSED;
		xseturgency(0);
		if(IS_SET(MODE_FOCUS))
			ttywrite("\033[I", 3);
	} else {
		XUnsetICFocus(xw.xic);
		xw.state &= ~WIN_FOCUSED;
		if(IS_SET(MODE_FOCUS))
			ttywrite("\033[O", 3);
	}
}

static inline bool
match(uint mask, uint state) {
	return mask == XK_ANY_MOD || mask == (state & ~ignoremod);
}

void
numlock(const Arg *dummy) {
	term.numlock ^= 1;
}

char*
kmap(KeySym k, uint state) {
	Key *kp;
	int i;

	/* Check for mapped keys out of X11 function keys. */
	for(i = 0; i < LEN(mappedkeys); i++) {
		if(mappedkeys[i] == k)
			break;
	}
	if(i == LEN(mappedkeys)) {
		if((k & 0xFFFF) < 0xFD00)
			return NULL;
	}

	for(kp = key; kp < key + LEN(key); kp++) {
		if(kp->k != k)
			continue;

		if(!match(kp->mask, state))
			continue;

		if(IS_SET(MODE_APPKEYPAD) ? kp->appkey < 0 : kp->appkey > 0)
			continue;
		if(term.numlock && kp->appkey == 2)
			continue;

		if(IS_SET(MODE_APPCURSOR) ? kp->appcursor < 0 : kp->appcursor > 0)
			continue;

		if(IS_SET(MODE_CRLF) ? kp->crlf < 0 : kp->crlf > 0)
			continue;

		return kp->s;
	}

	return NULL;
}

void
kpress(XEvent *ev) {
	XKeyEvent *e = &ev->xkey;
	KeySym ksym;
	char buf[32], *customkey;
	int len;
	long c;
	Status status;
	Shortcut *bp;

	if(IS_SET(MODE_KBDLOCK))
		return;

	len = XmbLookupString(xw.xic, e, buf, sizeof buf, &ksym, &status);
	/* 1. shortcuts */
	for(bp = shortcuts; bp < shortcuts + LEN(shortcuts); bp++) {
		if(ksym == bp->keysym && match(bp->mod, e->state)) {
			bp->func(&(bp->arg));
			return;
		}
	}

	/* 2. custom keys from config.h */
	if((customkey = kmap(ksym, e->state))) {
		ttysend(customkey, strlen(customkey));
		return;
	}

	/* 3. composed string from input method */
	if(len == 0)
		return;
	if(len == 1 && e->state & Mod1Mask) {
		if(IS_SET(MODE_8BIT)) {
			if(*buf < 0177) {
				c = *buf | 0x80;
				len = utf8encode(c, buf, UTF_SIZ);
			}
		} else {
			buf[1] = buf[0];
			buf[0] = '\033';
			len = 2;
		}
	}
	ttysend(buf, len);
}


void
cmessage(XEvent *e) {
	/*
	 * See xembed specs
	 *  http://standards.freedesktop.org/xembed-spec/xembed-spec-latest.html
	 */
	if(e->xclient.message_type == xw.xembed && e->xclient.format == 32) {
		if(e->xclient.data.l[1] == XEMBED_FOCUS_IN) {
			xw.state |= WIN_FOCUSED;
			xseturgency(0);
		} else if(e->xclient.data.l[1] == XEMBED_FOCUS_OUT) {
			xw.state &= ~WIN_FOCUSED;
		}
	} else if(e->xclient.data.l[0] == xw.wmdeletewin) {
		/* Send SIGHUP to shell */
		kill(pid, SIGHUP);
		exit(EXIT_SUCCESS);
	}
}

void
cresize(int width, int height) {
	int col, row;

	if(width != 0)
		xw.w = width;
	if(height != 0)
		xw.h = height;

	col = (xw.w - 2 * borderpx) / xw.cw;
	row = (xw.h - 2 * borderpx) / xw.ch;

	tresize(col, row);
	xresize(col, row);
	ttyresize();
}

void
resize(XEvent *e) {
	if(e->xconfigure.width == xw.w && e->xconfigure.height == xw.h)
		return;

	cresize(e->xconfigure.width, e->xconfigure.height);
}

void
run(void) {
	XEvent ev;
	int w = xw.w, h = xw.h;
	fd_set rfd;
	int xfd = XConnectionNumber(xw.dpy), xev, blinkset = 0, dodraw = 0;
	struct timespec drawtimeout, *tv = NULL, now, last, lastblink;
	long deltatime;

	/* Waiting for window mapping */
	while(1) {
		XNextEvent(xw.dpy, &ev);
		if(XFilterEvent(&ev, None))
			continue;
		if(ev.type == ConfigureNotify) {
			w = ev.xconfigure.width;
			h = ev.xconfigure.height;
		} else if(ev.type == MapNotify) {
			break;
		}
	}

	ttynew();
	cresize(w, h);

	clock_gettime(CLOCK_MONOTONIC, &last);
	lastblink = last;

	for(xev = actionfps;;) {
		FD_ZERO(&rfd);
		FD_SET(cmdfd, &rfd);
		FD_SET(xfd, &rfd);

		if(pselect(MAX(xfd, cmdfd)+1, &rfd, NULL, NULL, tv, NULL) < 0) {
			if(errno == EINTR)
				continue;
			die("select failed: %s\n", strerror(errno));
		}
		if(FD_ISSET(cmdfd, &rfd)) {
			ttyread();
			if(blinktimeout) {
				blinkset = tattrset(ATTR_BLINK);
				if(!blinkset)
					MODBIT(term.mode, 0, MODE_BLINK);
			}
		}

		if(FD_ISSET(xfd, &rfd))
			xev = actionfps;

		clock_gettime(CLOCK_MONOTONIC, &now);
		drawtimeout.tv_sec = 0;
		drawtimeout.tv_nsec = (1000/xfps) * 1E6;
		tv = &drawtimeout;

		dodraw = 0;
		if(blinktimeout && TIMEDIFF(now, lastblink) > blinktimeout) {
			tsetdirtattr(ATTR_BLINK);
			term.mode ^= MODE_BLINK;
			lastblink = now;
			dodraw = 1;
		}
		deltatime = TIMEDIFF(now, last);
		if(deltatime > (xev? (1000/xfps) : (1000/actionfps))
				|| deltatime < 0) {
			dodraw = 1;
			last = now;
		}

		if(dodraw) {
			while(XPending(xw.dpy)) {
				XNextEvent(xw.dpy, &ev);
				if(XFilterEvent(&ev, None))
					continue;
				if(handler[ev.type])
					(handler[ev.type])(&ev);
			}

			draw();
			XFlush(xw.dpy);

			if(xev && !FD_ISSET(xfd, &rfd))
				xev--;
			if(!FD_ISSET(cmdfd, &rfd) && !FD_ISSET(xfd, &rfd)) {
				if(blinkset) {
					if(TIMEDIFF(now, lastblink) \
							> blinktimeout) {
						drawtimeout.tv_nsec = 1000;
					} else {
						drawtimeout.tv_nsec = (1E6 * \
							(blinktimeout - \
							TIMEDIFF(now,
								lastblink)));
					}
				} else {
					tv = NULL;
				}
			}
		}
	}
}

void
usage(void) {
	die("%s " VERSION " (c) 2010-2014 st engineers\n" \
	"usage: st [-a] [-v] [-c class] [-f font] [-g geometry] [-o file]\n"
	"          [-i] [-t title] [-w windowid] [-e command ...]\n", argv0);
}

int
main(int argc, char *argv[]) {
	char *titles;
	uint cols = 80, rows = 24;

	xw.l = xw.t = 0;
	xw.isfixed = False;

	ARGBEGIN {
	case 'a':
		allowaltscreen = false;
		break;
	case 'c':
		opt_class = EARGF(usage());
		break;
	case 'e':
		/* eat all remaining arguments */
		if(argc > 1) {
			opt_cmd = &argv[1];
			if(argv[1] != NULL && opt_title == NULL) {
				titles = xstrdup(argv[1]);
				opt_title = basename(titles);
			}
		}
		goto run;
	case 'f':
		opt_font = EARGF(usage());
		break;
	case 'g':
		xw.gm = XParseGeometry(EARGF(usage()),
				&xw.l, &xw.t, &cols, &rows);
		break;
	case 'i':
		xw.isfixed = True;
		break;
	case 'o':
		opt_io = EARGF(usage());
		break;
	case 't':
		opt_title = EARGF(usage());
		break;
	case 'w':
		opt_embed = EARGF(usage());
		break;
	case 'v':
	default:
		usage();
	} ARGEND;

run:
	setlocale(LC_CTYPE, "");
	XSetLocaleModifiers("");
	tnew(cols? cols : 1, rows? rows : 1);
	xinit();
	selinit();
	run();

	return 0;
}

