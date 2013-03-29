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
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>

#define Glyph Glyph_
#define Font Font_
#define Draw XftDraw *
#define Colour XftColor
#define Colourmap Colormap

#if   defined(__linux)
 #include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
 #include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
 #include <libutil.h>
#endif

#define USAGE \
	"st " VERSION " (c) 2010-2013 st engineers\n" \
	"usage: st [-v] [-c class] [-f font] [-g geometry] [-o file]" \
	" [-t title] [-w windowid] [-e command ...]\n"

/* XEMBED messages */
#define XEMBED_FOCUS_IN  4
#define XEMBED_FOCUS_OUT 5

/* Arbitrary sizes */
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
#define SERRNO strerror(errno)
#define MIN(a, b)  ((a) < (b) ? (a) : (b))
#define MAX(a, b)  ((a) < (b) ? (b) : (a))
#define LEN(a)     (sizeof(a) / sizeof(a[0]))
#define DEFAULT(a, b)     (a) = (a) ? (a) : (b)
#define BETWEEN(x, a, b)  ((a) <= (x) && (x) <= (b))
#define LIMIT(x, a, b)    (x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x)
#define ATTRCMP(a, b) ((a).mode != (b).mode || (a).fg != (b).fg || (a).bg != (b).bg)
#define IS_SET(flag) ((term.mode & (flag)) != 0)
#define TIMEDIFF(t1, t2) ((t1.tv_sec-t2.tv_sec)*1000 + (t1.tv_usec-t2.tv_usec)/1000)

#define VT102ID "\033[?6c"

enum glyph_attribute {
	ATTR_NULL      = 0,
	ATTR_REVERSE   = 1,
	ATTR_UNDERLINE = 2,
	ATTR_BOLD      = 4,
	ATTR_GFX       = 8,
	ATTR_ITALIC    = 16,
	ATTR_BLINK     = 32,
};

enum cursor_movement {
	CURSOR_SAVE,
	CURSOR_LOAD
};

enum cursor_state {
	CURSOR_DEFAULT  = 0,
	CURSOR_WRAPNEXT = 1,
	CURSOR_ORIGIN	= 2
};

enum glyph_state {
	GLYPH_SET   = 1,
	GLYPH_DIRTY = 2
};

enum term_mode {
	MODE_WRAP	 = 1,
	MODE_INSERT      = 2,
	MODE_APPKEYPAD   = 4,
	MODE_ALTSCREEN   = 8,
	MODE_CRLF	 = 16,
	MODE_MOUSEBTN    = 32,
	MODE_MOUSEMOTION = 64,
	MODE_MOUSE       = 32|64,
	MODE_REVERSE     = 128,
	MODE_KBDLOCK     = 256,
	MODE_HIDE	 = 512,
	MODE_ECHO	 = 1024,
	MODE_APPCURSOR	 = 2048,
	MODE_MOUSESGR    = 4096,
};

enum escape_state {
	ESC_START      = 1,
	ESC_CSI	= 2,
	ESC_STR	= 4, /* DSC, OSC, PM, APC */
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

/* bit macro */
#undef B0
enum { B0=1, B1=2, B2=4, B3=8, B4=16, B5=32, B6=64, B7=128 };

typedef unsigned char uchar;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef unsigned short ushort;

typedef struct {
	char c[UTF_SIZ];     /* character code */
	uchar mode;  /* attribute flags */
	ushort fg;   /* foreground  */
	ushort bg;   /* background  */
	uchar state; /* state flags    */
} Glyph;

typedef Glyph *Line;

typedef struct {
	Glyph attr;	 /* current char attributes */
	int x;
	int y;
	char state;
} TCursor;

/* CSI Escape sequence structs */
/* ESC '[' [[ [<priv>] <arg> [;]] <mode>] */
typedef struct {
	char buf[ESC_BUF_SIZ]; /* raw string */
	int len;	       /* raw string length */
	char priv;
	int arg[ESC_ARG_SIZ];
	int narg;	       /* nb of args */
	char mode;
} CSIEscape;

/* STR Escape sequence structs */
/* ESC type [[ [<priv>] <arg> [;]] <mode>] ESC '\' */
typedef struct {
	char type;	     /* ESC type ... */
	char buf[STR_BUF_SIZ]; /* raw string */
	int len;	       /* raw string length */
	char *args[STR_ARG_SIZ];
	int narg;	      /* nb of args */
} STREscape;

/* Internal representation of the screen */
typedef struct {
	int row;	/* nb row */
	int col;	/* nb col */
	Line *line;	/* screen */
	Line *alt;	/* alternate screen */
	bool *dirty;	/* dirtyness of lines */
	TCursor c;	/* cursor */
	int top;	/* top    scroll limit */
	int bot;	/* bottom scroll limit */
	int mode;	/* terminal mode flags */
	int esc;	/* escape state flags */
	bool numlock;	/* lock numbers in keyboard */
	bool *tabs;
} Term;

/* Purely graphic info */
typedef struct {
	Display *dpy;
	Colourmap cmap;
	Window win;
	Drawable buf;
	Atom xembed, wmdeletewin;
	XIM xim;
	XIC xic;
	Draw draw;
	Visual *vis;
	int scr;
	bool isfixed; /* is fixed geometry? */
	int fx, fy, fw, fh; /* fixed geometry */
	int tw, th; /* tty width and height */
	int w, h; /* window width and height */
	int ch; /* char height */
	int cw; /* char width  */
	char state; /* focus, redraw, visible */
} XWindow;

typedef struct {
	KeySym k;
	uint mask;
	char s[ESC_BUF_SIZ];
	/* three valued logic variables: 0 indifferent, 1 on, -1 off */
	signed char appkey;		/* application keypad */
	signed char appcursor;		/* application cursor */
	signed char crlf;		/* crlf mode          */
} Key;

/* TODO: use better name for vars... */
typedef struct {
	int mode;
	int type;
	int bx, by;
	int ex, ey;
	struct {
		int x, y;
	} b, e;
	char *clip;
	Atom xtarget;
	bool alt;
	struct timeval tclick1;
	struct timeval tclick2;
} Selection;

typedef union {
	int i;
	unsigned int ui;
	float f;
	const void *v;
} Arg;

typedef struct {
	unsigned int mod;
	KeySym keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Shortcut;

/* function definitions used in config.h */
static void clippaste(const Arg *);
static void numlock(const Arg *);
static void selpaste(const Arg *);
static void xzoom(const Arg *);

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
	Colour col[LEN(colorname) < 256 ? 256 : LEN(colorname)];
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
static void strdump(void);
static void strhandle(void);
static void strparse(void);
static void strreset(void);

static void tclearregion(int, int, int, int, int);
static void tcursor(int);
static void tdeletechar(int);
static void tdeleteline(int);
static void tinsertblank(int);
static void tinsertblankline(int);
static void tmoveto(int, int);
static void tmoveato(int x, int y);
static void tnew(int, int);
static void tnewline(int);
static void tputtab(bool);
static void tputc(char *, int);
static void treset(void);
static int tresize(int, int);
static void tscrollup(int, int);
static void tscrolldown(int, int);
static void tsetattr(int*, int);
static void tsetchar(char *, Glyph *, int, int);
static void tsetscroll(int, int);
static void tswapscreen(void);
static void tsetdirt(int, int);
static void tsetmode(bool, bool, int *, int);
static void tfulldirt(void);
static void techo(char *, int);

static inline bool match(uint, uint);
static void ttynew(void);
static void ttyread(void);
static void ttyresize(void);
static void ttywrite(const char *, size_t);

static void xdraws(char *, Glyph, int, int, int, int);
static void xhints(void);
static void xclear(int, int, int, int);
static void xdrawcursor(void);
static void xinit(void);
static void xloadcols(void);
static int xsetcolorname(int, const char *);
static int xloadfont(Font *, FcPattern *);
static void xloadfonts(char *, int);
static void xsettitle(char *);
static void xresettitle(void);
static void xseturgency(int);
static void xsetsel(char*);
static void xtermclear(int, int, int, int);
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
static inline bool selected(int, int);
static void selcopy(void);
static void selscroll(int, int);

static int utf8decode(char *, long *);
static int utf8encode(long *, char *);
static int utf8size(char *);
static int isfullutf8(char *, int);

static ssize_t xwrite(int, char *, size_t);
static void *xmalloc(size_t);
static void *xrealloc(void *, size_t);
static void *xcalloc(size_t, size_t);

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
static int iofd = -1;
static char **opt_cmd = NULL;
static char *opt_io = NULL;
static char *opt_title = NULL;
static char *opt_embed = NULL;
static char *opt_class = NULL;
static char *opt_font = NULL;

static char *usedfont = NULL;
static int usedfontsize = 0;

/* Font Ring Cache */
enum {
	FRC_NORMAL,
	FRC_ITALIC,
	FRC_BOLD,
	FRC_ITALICBOLD
};

typedef struct {
	XftFont *font;
	long c;
	int flags;
} Fontcache;

/*
 * Fontcache is a ring buffer, with frccur as current position and frclen as
 * the current length of used elements.
 */

static Fontcache frc[1024];
static int frccur = -1, frclen = 0;

ssize_t
xwrite(int fd, char *s, size_t len) {
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

void *
xcalloc(size_t nmemb, size_t size) {
	void *p = calloc(nmemb, size);

	if(!p)
		die("Out of memory\n");

	return p;
}

int
utf8decode(char *s, long *u) {
	uchar c;
	int i, n, rtn;

	rtn = 1;
	c = *s;
	if(~c & B7) { /* 0xxxxxxx */
		*u = c;
		return rtn;
	} else if((c & (B7|B6|B5)) == (B7|B6)) { /* 110xxxxx */
		*u = c&(B4|B3|B2|B1|B0);
		n = 1;
	} else if((c & (B7|B6|B5|B4)) == (B7|B6|B5)) { /* 1110xxxx */
		*u = c&(B3|B2|B1|B0);
		n = 2;
	} else if((c & (B7|B6|B5|B4|B3)) == (B7|B6|B5|B4)) { /* 11110xxx */
		*u = c & (B2|B1|B0);
		n = 3;
	} else {
		goto invalid;
	}

	for(i = n, ++s; i > 0; --i, ++rtn, ++s) {
		c = *s;
		if((c & (B7|B6)) != B7) /* 10xxxxxx */
			goto invalid;
		*u <<= 6;
		*u |= c & (B5|B4|B3|B2|B1|B0);
	}

	if((n == 1 && *u < 0x80) ||
	   (n == 2 && *u < 0x800) ||
	   (n == 3 && *u < 0x10000) ||
	   (*u >= 0xD800 && *u <= 0xDFFF)) {
		goto invalid;
	}

	return rtn;
invalid:
	*u = 0xFFFD;

	return rtn;
}

int
utf8encode(long *u, char *s) {
	uchar *sp;
	ulong uc;
	int i, n;

	sp = (uchar *)s;
	uc = *u;
	if(uc < 0x80) {
		*sp = uc; /* 0xxxxxxx */
		return 1;
	} else if(*u < 0x800) {
		*sp = (uc >> 6) | (B7|B6); /* 110xxxxx */
		n = 1;
	} else if(uc < 0x10000) {
		*sp = (uc >> 12) | (B7|B6|B5); /* 1110xxxx */
		n = 2;
	} else if(uc <= 0x10FFFF) {
		*sp = (uc >> 18) | (B7|B6|B5|B4); /* 11110xxx */
		n = 3;
	} else {
		goto invalid;
	}

	for(i=n,++sp; i>0; --i,++sp)
		*sp = ((uc >> 6*(i-1)) & (B5|B4|B3|B2|B1|B0)) | B7; /* 10xxxxxx */

	return n+1;
invalid:
	/* U+FFFD */
	*s++ = '\xEF';
	*s++ = '\xBF';
	*s = '\xBD';

	return 3;
}

/* use this if your buffer is less than UTF_SIZ, it returns 1 if you can decode
   UTF-8 otherwise return 0 */
int
isfullutf8(char *s, int b) {
	uchar *c1, *c2, *c3;

	c1 = (uchar *)s;
	c2 = (uchar *)++s;
	c3 = (uchar *)++s;
	if(b < 1) {
		return 0;
	} else if((*c1&(B7|B6|B5)) == (B7|B6) && b == 1) {
		return 0;
	} else if((*c1&(B7|B6|B5|B4)) == (B7|B6|B5) &&
	    ((b == 1) ||
	    ((b == 2) && (*c2&(B7|B6)) == B7))) {
		return 0;
	} else if((*c1&(B7|B6|B5|B4|B3)) == (B7|B6|B5|B4) &&
	    ((b == 1) ||
	    ((b == 2) && (*c2&(B7|B6)) == B7) ||
	    ((b == 3) && (*c2&(B7|B6)) == B7 && (*c3&(B7|B6)) == B7))) {
		return 0;
	} else {
		return 1;
	}
}

int
utf8size(char *s) {
	uchar c = *s;

	if(~c&B7) {
		return 1;
	} else if((c&(B7|B6|B5)) == (B7|B6)) {
		return 2;
	} else if((c&(B7|B6|B5|B4)) == (B7|B6|B5)) {
		return 3;
	} else {
		return 4;
	}
}

void
selinit(void) {
	memset(&sel.tclick1, 0, sizeof(sel.tclick1));
	memset(&sel.tclick2, 0, sizeof(sel.tclick2));
	sel.mode = 0;
	sel.bx = -1;
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

static inline bool
selected(int x, int y) {
	int bx, ex;

	if(sel.ey == y && sel.by == y) {
		bx = MIN(sel.bx, sel.ex);
		ex = MAX(sel.bx, sel.ex);

		return BETWEEN(x, bx, ex);
	}

	if(sel.type == SEL_RECTANGULAR) {
		return ((sel.b.y <= y && y <= sel.e.y)
			&& (sel.b.x <= x && x <= sel.e.x));
	}
	return ((sel.b.y < y && y < sel.e.y)
		|| (y == sel.e.y && x <= sel.e.x))
		|| (y == sel.b.y && x >= sel.b.x
			&& (x <= sel.e.x || sel.b.y != sel.e.y));
}

void
getbuttoninfo(XEvent *e) {
	int type;
	uint state = e->xbutton.state &~Button1Mask;

	sel.alt = IS_SET(MODE_ALTSCREEN);

	sel.ex = x2col(e->xbutton.x);
	sel.ey = y2row(e->xbutton.y);

	sel.b.x = sel.by < sel.ey ? sel.bx : sel.ex;
	sel.b.y = MIN(sel.by, sel.ey);
	sel.e.x = sel.by < sel.ey ? sel.ex : sel.bx;
	sel.e.y = MAX(sel.by, sel.ey);

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
	static int ob, ox, oy;

	/* from urxvt */
	if(e->xbutton.type == MotionNotify) {
		if(!IS_SET(MODE_MOUSEMOTION) || (x == ox && y == oy))
			return;
		button = ob + 32;
		ox = x, oy = y;
	} else if(!IS_SET(MODE_MOUSESGR)
			&& (e->xbutton.type == ButtonRelease
				|| button == AnyButton)) {
		button = 3;
	} else {
		button -= Button1;
		if(button >= 3)
			button += 64 - 3;
		if(e->xbutton.type == ButtonPress) {
			ob = button;
			ox = x, oy = y;
		}
	}

	button += (state & ShiftMask   ? 4  : 0)
		+ (state & Mod4Mask    ? 8  : 0)
		+ (state & ControlMask ? 16 : 0);

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
	if(IS_SET(MODE_MOUSE)) {
		mousereport(e);
	} else if(e->xbutton.button == Button1) {
		if(sel.bx != -1) {
			sel.bx = -1;
			tsetdirt(sel.b.y, sel.e.y);
			draw();
		}
		sel.mode = 1;
		sel.type = SEL_REGULAR;
		sel.ex = sel.bx = x2col(e->xbutton.x);
		sel.ey = sel.by = y2row(e->xbutton.y);
	} else if(e->xbutton.button == Button4) {
		ttywrite("\031", 1);
	} else if(e->xbutton.button == Button5) {
		ttywrite("\005", 1);
	}
}

void
selcopy(void) {
	char *str, *ptr, *p;
	int x, y, bufsize, is_selected = 0, size;
	Glyph *gp, *last;

	if(sel.bx == -1) {
		str = NULL;
	} else {
		bufsize = (term.col+1) * (sel.e.y-sel.b.y+1) * UTF_SIZ;
		ptr = str = xmalloc(bufsize);

		/* append every set & selected glyph to the selection */
		for(y = sel.b.y; y < sel.e.y + 1; y++) {
			is_selected = 0;
			gp = &term.line[y][0];
			last = gp + term.col;

			while(--last >= gp && !(last->state & GLYPH_SET))
				/* nothing */;

			for(x = 0; gp <= last; x++, ++gp) {
				if(!selected(x, y)) {
					continue;
				} else {
					is_selected = 1;
				}

				p = (gp->state & GLYPH_SET) ? gp->c : " ";
				size = utf8size(p);
				memcpy(ptr, p, size);
				ptr += size;
			}
			/* \n at the end of every selected line except for the last one */
			if(is_selected && y < sel.e.y)
				*ptr++ = '\r';
		}
		*ptr = 0;
	}
	xsetsel(str);
}

void
selnotify(XEvent *e) {
	ulong nitems, ofs, rem;
	int format;
	uchar *data;
	Atom type;

	ofs = 0;
	do {
		if(XGetWindowProperty(xw.dpy, xw.win, XA_PRIMARY, ofs, BUFSIZ/4,
					False, AnyPropertyType, &type, &format,
					&nitems, &rem, &data)) {
			fprintf(stderr, "Clipboard allocation failed\n");
			return;
		}
		ttywrite((const char *) data, nitems * format / 8);
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
	if(sel.bx == -1)
		return;
	sel.bx = -1;
	tsetdirt(sel.b.y, sel.e.y);
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
	struct timeval now;

	if(IS_SET(MODE_MOUSE)) {
		mousereport(e);
		return;
	}

	if(e->xbutton.button == Button2) {
		selpaste(NULL);
	} else if(e->xbutton.button == Button1) {
		sel.mode = 0;
		getbuttoninfo(e);
		term.dirty[sel.ey] = 1;
		if(sel.bx == sel.ex && sel.by == sel.ey) {
			sel.bx = -1;
			gettimeofday(&now, NULL);

			if(TIMEDIFF(now, sel.tclick2) <= tripleclicktimeout) {
				/* triple click on the line */
				sel.b.x = sel.bx = 0;
				sel.e.x = sel.ex = term.col;
				sel.b.y = sel.e.y = sel.ey;
				selcopy();
			} else if(TIMEDIFF(now, sel.tclick1) <= doubleclicktimeout) {
				/* double click to select word */
				sel.bx = sel.ex;
				while(sel.bx > 0 && term.line[sel.ey][sel.bx-1].state & GLYPH_SET &&
						term.line[sel.ey][sel.bx-1].c[0] != ' ') {
					sel.bx--;
				}
				sel.b.x = sel.bx;
				while(sel.ex < term.col-1 && term.line[sel.ey][sel.ex+1].state & GLYPH_SET &&
						term.line[sel.ey][sel.ex+1].c[0] != ' ') {
					sel.ex++;
				}
				sel.e.x = sel.ex;
				sel.b.y = sel.e.y = sel.ey;
				selcopy();
			}
		} else {
			selcopy();
		}
	}

	memcpy(&sel.tclick2, &sel.tclick1, sizeof(struct timeval));
	gettimeofday(&sel.tclick1, NULL);
}

void
bmotion(XEvent *e) {
	int oldey, oldex, oldsby, oldsey;

	if(IS_SET(MODE_MOUSE)) {
		mousereport(e);
		return;
	}

	if(!sel.mode)
		return;

	oldey = sel.ey;
	oldex = sel.ex;
	oldsby = sel.b.y;
	oldsey = sel.e.y;
	getbuttoninfo(e);

	if(oldey != sel.ey || oldex != sel.ex) {
		tsetdirt(MIN(sel.b.y, oldsby), MAX(sel.e.y, oldsey));
	}
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
	char **args;
	char *envshell = getenv("SHELL");
	const struct passwd *pass = getpwuid(getuid());
	char buf[sizeof(long) * 8 + 1];

	unsetenv("COLUMNS");
	unsetenv("LINES");
	unsetenv("TERMCAP");

	if(pass) {
		setenv("LOGNAME", pass->pw_name, 1);
		setenv("USER", pass->pw_name, 1);
		setenv("SHELL", pass->pw_shell, 0);
		setenv("HOME", pass->pw_dir, 0);
	}

	snprintf(buf, sizeof(buf), "%lu", xw.win);
	setenv("WINDOWID", buf, 1);

	signal(SIGCHLD, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGALRM, SIG_DFL);

	DEFAULT(envshell, shell);
	setenv("TERM", termname, 1);
	args = opt_cmd ? opt_cmd : (char *[]){envshell, "-i", NULL};
	execvp(args[0], args);
	exit(EXIT_FAILURE);
}

void
sigchld(int a) {
	int stat = 0;

	if(waitpid(pid, &stat, 0) < 0)
		die("Waiting for pid %hd failed: %s\n",	pid, SERRNO);

	if(WIFEXITED(stat)) {
		exit(WEXITSTATUS(stat));
	} else {
		exit(EXIT_FAILURE);
	}
}

void
ttynew(void) {
	int m, s;
	struct winsize w = {term.row, term.col, 0, 0};

	/* seems to work fine on linux, openbsd and freebsd */
	if(openpty(&m, &s, NULL, NULL, &w) < 0)
		die("openpty failed: %s\n", SERRNO);

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
			die("ioctl TIOCSCTTY failed: %s\n", SERRNO);
		close(s);
		close(m);
		execsh();
		break;
	default:
		close(s);
		cmdfd = m;
		signal(SIGCHLD, sigchld);
		if(opt_io) {
			iofd = (!strcmp(opt_io, "-")) ?
				  STDOUT_FILENO :
				  open(opt_io, O_WRONLY | O_CREAT, 0666);
			if(iofd < 0) {
				fprintf(stderr, "Error opening %s:%s\n",
					opt_io, strerror(errno));
			}
		}
	}
}

void
dump(char c) {
	static int col;

	fprintf(stderr, " %02x '%c' ", c, isprint(c)?c:'.');
	if(++col % 10 == 0)
		fprintf(stderr, "\n");
}

void
ttyread(void) {
	static char buf[BUFSIZ];
	static int buflen = 0;
	char *ptr;
	char s[UTF_SIZ];
	int charsize; /* size of utf8 char in bytes */
	long utf8c;
	int ret;

	/* append read bytes to unprocessed bytes */
	if((ret = read(cmdfd, buf+buflen, LEN(buf)-buflen)) < 0)
		die("Couldn't read from shell: %s\n", SERRNO);

	/* process every complete utf8 char */
	buflen += ret;
	ptr = buf;
	while(buflen >= UTF_SIZ || isfullutf8(ptr,buflen)) {
		charsize = utf8decode(ptr, &utf8c);
		utf8encode(&utf8c, s);
		tputc(s, charsize);
		ptr += charsize;
		buflen -= charsize;
	}

	/* keep any uncomplete utf8 char for the next call */
	memmove(buf, ptr, buflen);
}

void
ttywrite(const char *s, size_t n) {
	if(write(cmdfd, s, n) == -1)
		die("write error on tty: %s\n", SERRNO);
}

void
ttyresize(void) {
	struct winsize w;

	w.ws_row = term.row;
	w.ws_col = term.col;
	w.ws_xpixel = xw.tw;
	w.ws_ypixel = xw.th;
	if(ioctl(cmdfd, TIOCSWINSZ, &w) < 0)
		fprintf(stderr, "Couldn't set window size: %s\n", SERRNO);
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
tfulldirt(void) {
	tsetdirt(0, term.row-1);
}

void
tcursor(int mode) {
	static TCursor c;

	if(mode == CURSOR_SAVE) {
		c = term.c;
	} else if(mode == CURSOR_LOAD) {
		term.c = c;
		tmoveto(c.x, c.y);
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

	tclearregion(0, 0, term.col-1, term.row-1, 0);
	tmoveto(0, 0);
	tcursor(CURSOR_SAVE);
}

void
tnew(int col, int row) {
	/* set screen size */
	term.row = row;
	term.col = col;
	term.line = xmalloc(term.row * sizeof(Line));
	term.alt  = xmalloc(term.row * sizeof(Line));
	term.dirty = xmalloc(term.row * sizeof(*term.dirty));
	term.tabs = xmalloc(term.col * sizeof(*term.tabs));

	for(row = 0; row < term.row; row++) {
		term.line[row] = xmalloc(term.col * sizeof(Glyph));
		term.alt [row] = xmalloc(term.col * sizeof(Glyph));
		term.dirty[row] = 0;
	}

	term.numlock = 1;
	memset(term.tabs, 0, term.col * sizeof(*term.tabs));
	/* setup screen */
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

	tclearregion(0, term.bot-n+1, term.col-1, term.bot, 0);

	for(i = term.bot; i >= orig+n; i--) {
		temp = term.line[i];
		term.line[i] = term.line[i-n];
		term.line[i-n] = temp;

		term.dirty[i] = 1;
		term.dirty[i-n] = 1;
	}

	selscroll(orig, n);
}

void
tscrollup(int orig, int n) {
	int i;
	Line temp;
	LIMIT(n, 0, term.bot-orig+1);

	tclearregion(0, orig, term.col-1, orig+n-1, 0);

	for(i = orig; i <= term.bot-n; i++) {
		 temp = term.line[i];
		 term.line[i] = term.line[i+n];
		 term.line[i+n] = temp;

		 term.dirty[i] = 1;
		 term.dirty[i+n] = 1;
	}

	selscroll(orig, -n);
}

void
selscroll(int orig, int n) {
	if(sel.bx == -1)
		return;

	if(BETWEEN(sel.by, orig, term.bot) || BETWEEN(sel.ey, orig, term.bot)) {
		if((sel.by += n) > term.bot || (sel.ey += n) < term.top) {
			sel.bx = -1;
			return;
		}
		if(sel.type == SEL_RECTANGULAR) {
			if(sel.by < term.top)
				sel.by = term.top;
			if(sel.ey > term.bot)
				sel.ey = term.bot;
		} else {
			if(sel.by < term.top) {
				sel.by = term.top;
				sel.bx = 0;
			}
			if(sel.ey > term.bot) {
				sel.ey = term.bot;
				sel.ex = term.col;
			}
		}
		sel.b.y = sel.by, sel.b.x = sel.bx;
		sel.e.y = sel.ey, sel.e.x = sel.ex;
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
	if(attr->mode & ATTR_GFX) {
		if(c[0] >= 0x41 && c[0] <= 0x7e
				&& vt100_0[c[0] - 0x41]) {
			c = vt100_0[c[0] - 0x41];
		}
	}

	term.dirty[y] = 1;
	term.line[y][x] = *attr;
	memcpy(term.line[y][x].c, c, UTF_SIZ);
	term.line[y][x].state |= GLYPH_SET;
}

void
tclearregion(int x1, int y1, int x2, int y2, int bce) {
	int x, y, temp;

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
			if(bce) {
				term.line[y][x] = term.c.attr;
				memcpy(term.line[y][x].c, " ", 2);
				term.line[y][x].state |= GLYPH_SET;
			} else {
				term.line[y][x].state = 0;
			}
		}
	}
}

void
tdeletechar(int n) {
	int src = term.c.x + n;
	int dst = term.c.x;
	int size = term.col - src;

	term.dirty[term.c.y] = 1;

	if(src >= term.col) {
		tclearregion(term.c.x, term.c.y, term.col-1, term.c.y, 0);
		return;
	}

	memmove(&term.line[term.c.y][dst], &term.line[term.c.y][src],
			size * sizeof(Glyph));
	tclearregion(term.col-n, term.c.y, term.col-1, term.c.y, 0);
}

void
tinsertblank(int n) {
	int src = term.c.x;
	int dst = src + n;
	int size = term.col - dst;

	term.dirty[term.c.y] = 1;

	if(dst >= term.col) {
		tclearregion(term.c.x, term.c.y, term.col-1, term.c.y, 0);
		return;
	}

	memmove(&term.line[term.c.y][dst], &term.line[term.c.y][src],
			size * sizeof(Glyph));
	tclearregion(src, term.c.y, dst - 1, term.c.y, 0);
}

void
tinsertblankline(int n) {
	if(term.c.y < term.top || term.c.y > term.bot)
		return;

	tscrolldown(term.c.y, n);
}

void
tdeleteline(int n) {
	if(term.c.y < term.top || term.c.y > term.bot)
		return;

	tscrollup(term.c.y, n);
}

void
tsetattr(int *attr, int l) {
	int i;

	for(i = 0; i < l; i++) {
		switch(attr[i]) {
		case 0:
			term.c.attr.mode &= ~(ATTR_REVERSE | ATTR_UNDERLINE | ATTR_BOLD \
					| ATTR_ITALIC | ATTR_BLINK);
			term.c.attr.fg = defaultfg;
			term.c.attr.bg = defaultbg;
			break;
		case 1:
			term.c.attr.mode |= ATTR_BOLD;
			break;
		case 3:
			term.c.attr.mode |= ATTR_ITALIC;
			break;
		case 4:
			term.c.attr.mode |= ATTR_UNDERLINE;
			break;
		case 5: /* slow blink */
		case 6: /* rapid blink */
			term.c.attr.mode |= ATTR_BLINK;
			break;
		case 7:
			term.c.attr.mode |= ATTR_REVERSE;
			break;
		case 21:
		case 22:
			term.c.attr.mode &= ~ATTR_BOLD;
			break;
		case 23:
			term.c.attr.mode &= ~ATTR_ITALIC;
			break;
		case 24:
			term.c.attr.mode &= ~ATTR_UNDERLINE;
			break;
		case 25:
		case 26:
			term.c.attr.mode &= ~ATTR_BLINK;
			break;
		case 27:
			term.c.attr.mode &= ~ATTR_REVERSE;
			break;
		case 38:
			if(i + 2 < l && attr[i + 1] == 5) {
				i += 2;
				if(BETWEEN(attr[i], 0, 255)) {
					term.c.attr.fg = attr[i];
				} else {
					fprintf(stderr,
						"erresc: bad fgcolor %d\n",
						attr[i]);
				}
			} else {
				fprintf(stderr,
					"erresc(38): gfx attr %d unknown\n",
					attr[i]);
			}
			break;
		case 39:
			term.c.attr.fg = defaultfg;
			break;
		case 48:
			if(i + 2 < l && attr[i + 1] == 5) {
				i += 2;
				if(BETWEEN(attr[i], 0, 255)) {
					term.c.attr.bg = attr[i];
				} else {
					fprintf(stderr,
						"erresc: bad bgcolor %d\n",
						attr[i]);
				}
			} else {
				fprintf(stderr,
					"erresc(48): gfx attr %d unknown\n",
					attr[i]);
			}
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

#define MODBIT(x, set, bit) ((set) ? ((x) |= (bit)) : ((x) &= ~(bit)))

void
tsetmode(bool priv, bool set, int *args, int narg) {
	int *lim, mode;
	bool alt;

	for(lim = args + narg; args < lim; ++args) {
		if(priv) {
			switch(*args) {
				break;
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
			case 1000: /* 1000,1002: enable xterm mouse report */
				MODBIT(term.mode, set, MODE_MOUSEBTN);
				MODBIT(term.mode, 0, MODE_MOUSEMOTION);
				break;
			case 1002:
				MODBIT(term.mode, set, MODE_MOUSEMOTION);
				MODBIT(term.mode, 0, MODE_MOUSEBTN);
				break;
			case 1006:
				MODBIT(term.mode, set, MODE_MOUSESGR);
				break;
			case 1049: /* = 1047 and 1048 */
			case 47:
			case 1047: {
				alt = IS_SET(MODE_ALTSCREEN);
				if(alt) {
					tclearregion(0, 0, term.col-1,
							term.row-1, 0);
				}
				if(set ^ alt)		/* set is always 1 or 0 */
					tswapscreen();
				if(*args != 1049)
					break;
			}
				/* pass through */
			case 1048:
				tcursor((set) ? CURSOR_SAVE : CURSOR_LOAD);
				break;
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
#undef MODBIT


void
csihandle(void) {
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
	case 'c': /* DA -- Device Attributes */
		if(csiescseq.arg[0] == 0)
			ttywrite(VT102ID, sizeof(VT102ID) - 1);
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
		while(csiescseq.arg[0]--)
			tputtab(1);
		break;
	case 'J': /* ED -- Clear screen */
		sel.bx = -1;
		switch(csiescseq.arg[0]) {
		case 0: /* below */
			tclearregion(term.c.x, term.c.y, term.col-1, term.c.y, 1);
			if(term.c.y < term.row-1) {
				tclearregion(0, term.c.y+1, term.col-1,
						term.row-1, 1);
			}
			break;
		case 1: /* above */
			if(term.c.y > 1)
				tclearregion(0, 0, term.col-1, term.c.y-1, 1);
			tclearregion(0, term.c.y, term.c.x, term.c.y, 1);
			break;
		case 2: /* all */
			tclearregion(0, 0, term.col-1, term.row-1, 1);
			break;
		default:
			goto unknown;
		}
		break;
	case 'K': /* EL -- Clear line */
		switch(csiescseq.arg[0]) {
		case 0: /* right */
			tclearregion(term.c.x, term.c.y, term.col-1,
					term.c.y, 1);
			break;
		case 1: /* left */
			tclearregion(0, term.c.y, term.c.x, term.c.y, 1);
			break;
		case 2: /* all */
			tclearregion(0, term.c.y, term.col-1, term.c.y, 1);
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
				term.c.x + csiescseq.arg[0] - 1, term.c.y, 1);
		break;
	case 'P': /* DCH -- Delete <n> char */
		DEFAULT(csiescseq.arg[0], 1);
		tdeletechar(csiescseq.arg[0]);
		break;
	case 'Z': /* CBT -- Cursor Backward Tabulation <n> tab stops */
		DEFAULT(csiescseq.arg[0], 1);
		while(csiescseq.arg[0]--)
			tputtab(0);
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
	int i, j, narg;

	strparse();
	narg = strescseq.narg;

	switch(strescseq.type) {
	case ']': /* OSC -- Operating System Command */
		switch(i = atoi(strescseq.args[0])) {
		case 0:
		case 1:
		case 2:
			if(narg > 1)
				xsettitle(strescseq.args[1]);
			break;
		case 4: /* color set */
			if(narg < 3)
				break;
			p = strescseq.args[2];
			/* fall through */
		case 104: /* color reset, here p = NULL */
			j = (narg > 1) ? atoi(strescseq.args[1]) : -1;
			if (!xsetcolorname(j, p)) {
				fprintf(stderr, "erresc: invalid color %s\n", p);
			} else {
				/*
				 * TODO if defaultbg color is changed, borders
				 * are dirty
				 */
				redraw(0);
			}
			break;
		default:
			fprintf(stderr, "erresc: unknown str ");
			strdump();
			break;
		}
		break;
	case 'k': /* old title set compatibility */
		xsettitle(strescseq.args[0]);
		break;
	case 'P': /* DSC -- Device Control String */
	case '_': /* APC -- Application Program Command */
	case '^': /* PM -- Privacy Message */
	default:
		fprintf(stderr, "erresc: unknown str ");
		strdump();
		/* die(""); */
		break;
	}
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
tputtab(bool forward) {
	uint x = term.c.x;

	if(forward) {
		if(x == term.col)
			return;
		for(++x; x < term.col && !term.tabs[x]; ++x)
			/* nothing */ ;
	} else {
		if(x == 0)
			return;
		for(--x; x > 0 && !term.tabs[x]; --x)
			/* nothing */ ;
	}
	tmoveto(x, term.c.y);
}

void
techo(char *buf, int len) {
	for(; len > 0; buf++, len--) {
		char c = *buf;

		if(c == '\033') {		/* escape */
			tputc("^", 1);
			tputc("[", 1);
		} else if(c < '\x20') {	/* control code */
			if(c != '\n' && c != '\r' && c != '\t') {
				c |= '\x40';
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
tputc(char *c, int len) {
	uchar ascii = *c;
	bool control = ascii < '\x20' || ascii == 0177;

	if(iofd != -1) {
		if(xwrite(iofd, c, len) < 0) {
			fprintf(stderr, "Error writing in %s:%s\n",
				opt_io, strerror(errno));
			close(iofd);
			iofd = -1;
		}
	}

	/*
	 * STR sequences must be checked before anything else
	 * because it can use some control codes as part of the sequence.
	 */
	if(term.esc & ESC_STR) {
		switch(ascii) {
		case '\033':
			term.esc = ESC_START | ESC_STR_END;
			break;
		case '\a': /* backwards compatibility to xterm */
			term.esc = 0;
			strhandle();
			break;
		default:
			if(strescseq.len + len < sizeof(strescseq.buf) - 1) {
				memmove(&strescseq.buf[strescseq.len], c, len);
				strescseq.len += len;
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
			}
		}
		return;
	}

	/*
	 * Actions of control codes must be performed as soon they arrive
	 * because they can be embedded inside a control sequence, and
	 * they must not cause conflicts with sequences.
	 */
	if(control) {
		switch(ascii) {
		case '\t':	/* HT */
			tputtab(1);
			return;
		case '\b':	/* BS */
			tmoveto(term.c.x-1, term.c.y);
			return;
		case '\r':	/* CR */
			tmoveto(0, term.c.y);
			return;
		case '\f':	/* LF */
		case '\v':	/* VT */
		case '\n':	/* LF */
			/* go to first col if the mode is set */
			tnewline(IS_SET(MODE_CRLF));
			return;
		case '\a':	/* BEL */
			if(!(xw.state & WIN_FOCUSED))
				xseturgency(1);
			return;
		case '\033':	/* ESC */
			csireset();
			term.esc = ESC_START;
			return;
		case '\016':	/* SO */
		case '\017':	/* SI */
			/*
			 * Different charsets are hard to handle. Applications
			 * should use the right alt charset escapes for the
			 * only reason they still exist: line drawing. The
			 * rest is incompatible history st should not support.
			 */
			return;
		case '\032':	/* SUB */
		case '\030':	/* CAN */
			csireset();
			return;
		case '\005':	/* ENQ (IGNORED) */
		case '\000':	/* NUL (IGNORED) */
		case '\021':	/* XON (IGNORED) */
		case '\023':	/* XOFF (IGNORED) */
		case 0177:	/* DEL (IGNORED) */
			return;
		}
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
		} else if(term.esc & ESC_STR_END) {
			term.esc = 0;
			if(ascii == '\\')
				strhandle();
		} else if(term.esc & ESC_ALTCHARSET) {
			switch(ascii) {
			case '0': /* Line drawing set */
				term.c.attr.mode |= ATTR_GFX;
				break;
			case 'B': /* USASCII */
				term.c.attr.mode &= ~ATTR_GFX;
				break;
			case 'A': /* UK (IGNORED) */
			case '<': /* multinational charset (IGNORED) */
			case '5': /* Finnish (IGNORED) */
			case 'C': /* Finnish (IGNORED) */
			case 'K': /* German (IGNORED) */
				break;
			default:
				fprintf(stderr, "esc unhandled charset: ESC ( %c\n", ascii);
			}
			term.esc = 0;
		} else if(term.esc & ESC_TEST) {
			if(ascii == '8') { /* DEC screen alignment test. */
				char E[UTF_SIZ] = "E";
				int x, y;

				for(x = 0; x < term.col; ++x) {
					for(y = 0; y < term.row; ++y)
						tsetchar(E, &term.c.attr, x, y);
				}
			}
			term.esc = 0;
		} else {
			switch(ascii) {
			case '[':
				term.esc |= ESC_CSI;
				break;
			case '#':
				term.esc |= ESC_TEST;
				break;
			case 'P': /* DCS -- Device Control String */
			case '_': /* APC -- Application Program Command */
			case '^': /* PM -- Privacy Message */
			case ']': /* OSC -- Operating System Command */
			case 'k': /* old title set compatibility */
				strreset();
				strescseq.type = ascii;
				term.esc |= ESC_STR;
				break;
			case '(': /* set primary charset G0 */
				term.esc |= ESC_ALTCHARSET;
				break;
			case ')': /* set secondary charset G1 (IGNORED) */
			case '*': /* set tertiary charset G2 (IGNORED) */
			case '+': /* set quaternary charset G3 (IGNORED) */
				term.esc = 0;
				break;
			case 'D': /* IND -- Linefeed */
				if(term.c.y == term.bot) {
					tscrollup(term.top, 1);
				} else {
					tmoveto(term.c.x, term.c.y+1);
				}
				term.esc = 0;
				break;
			case 'E': /* NEL -- Next line */
				tnewline(1); /* always go to first col */
				term.esc = 0;
				break;
			case 'H': /* HTS -- Horizontal tab stop */
				term.tabs[term.c.x] = 1;
				term.esc = 0;
				break;
			case 'M': /* RI -- Reverse index */
				if(term.c.y == term.top) {
					tscrolldown(term.top, 1);
				} else {
					tmoveto(term.c.x, term.c.y-1);
				}
				term.esc = 0;
				break;
			case 'Z': /* DECID -- Identify Terminal */
				ttywrite(VT102ID, sizeof(VT102ID) - 1);
				term.esc = 0;
				break;
			case 'c': /* RIS -- Reset to inital state */
				treset();
				term.esc = 0;
				xresettitle();
				break;
			case '=': /* DECPAM -- Application keypad */
				term.mode |= MODE_APPKEYPAD;
				term.esc = 0;
				break;
			case '>': /* DECPNM -- Normal keypad */
				term.mode &= ~MODE_APPKEYPAD;
				term.esc = 0;
				break;
			case '7': /* DECSC -- Save Cursor */
				tcursor(CURSOR_SAVE);
				term.esc = 0;
				break;
			case '8': /* DECRC -- Restore Cursor */
				tcursor(CURSOR_LOAD);
				term.esc = 0;
				break;
			case '\\': /* ST -- Stop */
				term.esc = 0;
				break;
			default:
				fprintf(stderr, "erresc: unknown sequence ESC 0x%02X '%c'\n",
					(uchar) ascii, isprint(ascii)? ascii:'.');
				term.esc = 0;
			}
		}
		/*
		 * All characters which form part of a sequence are not
		 * printed
		 */
		return;
	}
	/*
	 * Display control codes only if we are in graphic mode
	 */
	if(control && !(term.c.attr.mode & ATTR_GFX))
		return;
	if(sel.bx != -1 && BETWEEN(term.c.y, sel.by, sel.ey))
		sel.bx = -1;
	if(IS_SET(MODE_WRAP) && term.c.state & CURSOR_WRAPNEXT)
		tnewline(1); /* always go to first col */

	if(IS_SET(MODE_INSERT) && term.c.x+1 < term.col) {
		memmove(&term.line[term.c.y][term.c.x+1],
			&term.line[term.c.y][term.c.x],
			(term.col - term.c.x - 1) * sizeof(Glyph));
	}

	tsetchar(c, &term.c.attr, term.c.x, term.c.y);
	if(term.c.x+1 < term.col) {
		tmoveto(term.c.x+1, term.c.y);
	} else {
		term.c.state |= CURSOR_WRAPNEXT;
	}
}

int
tresize(int col, int row) {
	int i, x;
	int minrow = MIN(row, term.row);
	int mincol = MIN(col, term.col);
	int slide = term.c.y - row + 1;
	bool *bp;

	if(col < 1 || row < 1)
		return 0;

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
		term.dirty[i] = 1;
		term.line[i] = xrealloc(term.line[i], col * sizeof(Glyph));
		term.alt[i]  = xrealloc(term.alt[i],  col * sizeof(Glyph));
		for(x = mincol; x < col; x++) {
			term.line[i][x].state = 0;
			term.alt[i][x].state = 0;
		}
	}

	/* allocate any new rows */
	for(/* i == minrow */; i < row; i++) {
		term.dirty[i] = 1;
		term.line[i] = xcalloc(col, sizeof(Glyph));
		term.alt [i] = xcalloc(col, sizeof(Glyph));
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

	return (slide > 0);
}

void
xresize(int col, int row) {
	xw.tw = MAX(1, col * xw.cw);
	xw.th = MAX(1, row * xw.ch);

	XFreePixmap(xw.dpy, xw.buf);
	xw.buf = XCreatePixmap(xw.dpy, xw.win, xw.w, xw.h,
			DefaultDepth(xw.dpy, xw.scr));
	XSetForeground(xw.dpy, dc.gc, dc.col[IS_SET(MODE_REVERSE) ? defaultfg : defaultbg].pixel);
	XFillRectangle(xw.dpy, xw.buf, dc.gc, 0, 0, xw.w, xw.h);

	XftDrawChange(xw.draw, xw.buf);
}

static inline ushort
sixd_to_16bit(int x) {
	return x == 0 ? 0 : 0x3737 + 0x2828 * x;
}

void
xloadcols(void) {
	int i, r, g, b;
	XRenderColor color = { .alpha = 0xffff };

	/* load colors [0-15] colors and [256-LEN(colorname)[ (config.h) */
	for(i = 0; i < LEN(colorname); i++) {
		if(!colorname[i])
			continue;
		if(!XftColorAllocName(xw.dpy, xw.vis, xw.cmap, colorname[i], &dc.col[i])) {
			die("Could not allocate color '%s'\n", colorname[i]);
		}
	}

	/* load colors [16-255] ; same colors as xterm */
	for(i = 16, r = 0; r < 6; r++) {
		for(g = 0; g < 6; g++) {
			for(b = 0; b < 6; b++) {
				color.red = sixd_to_16bit(r);
				color.green = sixd_to_16bit(g);
				color.blue = sixd_to_16bit(b);
				if(!XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &color, &dc.col[i])) {
					die("Could not allocate color %d\n", i);
				}
				i++;
			}
		}
	}

	for(r = 0; r < 24; r++, i++) {
		color.red = color.green = color.blue = 0x0808 + 0x0a0a * r;
		if(!XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &color,
					&dc.col[i])) {
			die("Could not allocate color %d\n", i);
		}
	}
}

int
xsetcolorname(int x, const char *name) {
	XRenderColor color = { .alpha = 0xffff };
	Colour colour;
	if (x < 0 || x > LEN(colorname))
		return -1;
	if(!name) {
		if(16 <= x && x < 16 + 216) {
			int r = (x - 16) / 36, g = ((x - 16) % 36) / 6, b = (x - 16) % 6;
			color.red = sixd_to_16bit(r);
			color.green = sixd_to_16bit(g);
			color.blue = sixd_to_16bit(b);
			if(!XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &color, &colour))
				return 0; /* something went wrong */
			dc.col[x] = colour;
			return 1;
		} else if (16 + 216 <= x && x < 256) {
			color.red = color.green = color.blue = 0x0808 + 0x0a0a * (x - (16 + 216));
			if(!XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &color, &colour))
				return 0; /* something went wrong */
			dc.col[x] = colour;
			return 1;
		} else {
			name = colorname[x];
		}
	}
	if(!XftColorAllocName(xw.dpy, xw.vis, xw.cmap, name, &colour))
		return 0;
	dc.col[x] = colour;
	return 1;
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
	if(xw.isfixed == False) {
		sizeh->flags = PSize | PResizeInc | PBaseSize;
		sizeh->height = xw.h;
		sizeh->width = xw.w;
		sizeh->height_inc = xw.ch;
		sizeh->width_inc = xw.cw;
		sizeh->base_height = 2 * borderpx;
		sizeh->base_width = 2 * borderpx;
	} else {
		sizeh->flags = PMaxSize | PMinSize;
		sizeh->min_width = sizeh->max_width = xw.fw;
		sizeh->min_height = sizeh->max_height = xw.fh;
	}

	XSetWMProperties(xw.dpy, xw.win, NULL, NULL, NULL, 0, sizeh, &wm, &class);
	XFree(sizeh);
}

int
xloadfont(Font *f, FcPattern *pattern) {
	FcPattern *match;
	FcResult result;

	match = FcFontMatch(NULL, pattern, &result);
	if(!match)
		return 1;

	if(!(f->set = FcFontSort(0, match, FcTrue, 0, &result))) {
		FcPatternDestroy(match);
		return 1;
	}

	if(!(f->match = XftFontOpenPattern(xw.dpy, match))) {
		FcPatternDestroy(match);
		return 1;
	}

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
xloadfonts(char *fontstr, int fontsize) {
	FcPattern *pattern;
	FcResult result;
	double fontval;

	if(fontstr[0] == '-') {
		pattern = XftXlfdParse(fontstr, False, False);
	} else {
		pattern = FcNameParse((FcChar8 *)fontstr);
	}

	if(!pattern)
		die("st: can't open font %s\n", fontstr);

	if(fontsize > 0) {
		FcPatternDel(pattern, FC_PIXEL_SIZE);
		FcPatternAddDouble(pattern, FC_PIXEL_SIZE, (double)fontsize);
		usedfontsize = fontsize;
	} else {
		result = FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &fontval);
		if(result == FcResultMatch) {
			usedfontsize = (int)fontval;
		} else {
			/*
			 * Default font size is 12, if none given. This is to
			 * have a known usedfontsize value.
			 */
			FcPatternAddDouble(pattern, FC_PIXEL_SIZE, 12);
			usedfontsize = 12;
		}
	}

	FcConfigSubstitute(0, pattern, FcMatchPattern);
	FcDefaultSubstitute(pattern);

	if(xloadfont(&dc.font, pattern))
		die("st: can't open font %s\n", fontstr);

	/* Setting character width and height. */
	xw.cw = dc.font.width;
	xw.ch = dc.font.height;

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

void
xunloadfonts(void) {
	int i, ip;

	/*
	 * Free the loaded fonts in the font cache. This is done backwards
	 * from the frccur.
	 */
	for(i = 0, ip = frccur; i < frclen; i++, ip--) {
		if(ip < 0)
			ip = LEN(frc) - 1;
		XftFontClose(xw.dpy, frc[ip].font);
	}
	frccur = -1;
	frclen = 0;

	XftFontClose(xw.dpy, dc.font.match);
	FcPatternDestroy(dc.font.pattern);
	FcFontSetDestroy(dc.font.set);
	XftFontClose(xw.dpy, dc.bfont.match);
	FcPatternDestroy(dc.bfont.pattern);
	FcFontSetDestroy(dc.bfont.set);
	XftFontClose(xw.dpy, dc.ifont.match);
	FcPatternDestroy(dc.ifont.pattern);
	FcFontSetDestroy(dc.ifont.set);
	XftFontClose(xw.dpy, dc.ibfont.match);
	FcPatternDestroy(dc.ibfont.pattern);
	FcFontSetDestroy(dc.ibfont.set);
}

void
xzoom(const Arg *arg) {
	xunloadfonts();
	xloadfonts(usedfont, usedfontsize + arg->i);
	cresize(0, 0);
	redraw(0);
}

void
xinit(void) {
	XSetWindowAttributes attrs;
	XGCValues gcvalues;
	Cursor cursor;
	Window parent;
	int sw, sh;

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
	if(xw.isfixed) {
		sw = DisplayWidth(xw.dpy, xw.scr);
		sh = DisplayHeight(xw.dpy, xw.scr);
		if(xw.fx < 0)
			xw.fx = sw + xw.fx - xw.fw - 1;
		if(xw.fy < 0)
			xw.fy = sh + xw.fy - xw.fh - 1;

		xw.h = xw.fh;
		xw.w = xw.fw;
	} else {
		/* window - default size */
		xw.h = 2 * borderpx + term.row * xw.ch;
		xw.w = 2 * borderpx + term.col * xw.cw;
		xw.fx = 0;
		xw.fy = 0;
	}

	/* Events */
	attrs.background_pixel = dc.col[defaultbg].pixel;
	attrs.border_pixel = dc.col[defaultbg].pixel;
	attrs.bit_gravity = NorthWestGravity;
	attrs.event_mask = FocusChangeMask | KeyPressMask
		| ExposureMask | VisibilityChangeMask | StructureNotifyMask
		| ButtonMotionMask | ButtonPressMask | ButtonReleaseMask;
	attrs.colormap = xw.cmap;

	parent = opt_embed ? strtol(opt_embed, NULL, 0) : \
			XRootWindow(xw.dpy, xw.scr);
	xw.win = XCreateWindow(xw.dpy, parent, xw.fx, xw.fy,
			xw.w, xw.h, 0, XDefaultDepth(xw.dpy, xw.scr), InputOutput,
			xw.vis,
			CWBackPixel | CWBorderPixel | CWBitGravity | CWEventMask
			| CWColormap,
			&attrs);

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
	if((xw.xim =  XOpenIM(xw.dpy, NULL, NULL, NULL)) == NULL) {
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
	XSetWMProtocols(xw.dpy, xw.win, &xw.wmdeletewin, 1);

	xresettitle();
	XMapWindow(xw.dpy, xw.win);
	xhints();
	XSync(xw.dpy, 0);
}

void
xdraws(char *s, Glyph base, int x, int y, int charlen, int bytelen) {
	int winx = borderpx + x * xw.cw, winy = borderpx + y * xw.ch,
	    width = charlen * xw.cw, xp, i;
	int frp, frcflags;
	int u8fl, u8fblen, u8cblen, doesexist;
	char *u8c, *u8fs;
	long u8char;
	Font *font = &dc.font;
	FcResult fcres;
	FcPattern *fcpattern, *fontpattern;
	FcFontSet *fcsets[] = { NULL };
	FcCharSet *fccharset;
	Colour *fg, *bg, *temp, revfg, revbg;
	XRenderColor colfg, colbg;

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
	fg = &dc.col[base.fg];
	bg = &dc.col[base.bg];

	if(base.mode & ATTR_BOLD) {
		if(BETWEEN(base.fg, 0, 7)) {
			/* basic system colors */
			fg = &dc.col[base.fg + 8];
		} else if(BETWEEN(base.fg, 16, 195)) {
			/* 256 colors */
			fg = &dc.col[base.fg + 36];
		} else if(BETWEEN(base.fg, 232, 251)) {
			/* greyscale */
			fg = &dc.col[base.fg + 4];
		}
		/*
		 * Those ranges will not be brightened:
		 *	8 - 15 – bright system colors
		 *	196 - 231 – highest 256 color cube
		 *	252 - 255 – brightest colors in greyscale
		 */
		font = &dc.bfont;
		frcflags = FRC_BOLD;
	}

	if(IS_SET(MODE_REVERSE)) {
		if(fg == &dc.col[defaultfg]) {
			fg = &dc.col[defaultbg];
		} else {
			colfg.red = ~fg->color.red;
			colfg.green = ~fg->color.green;
			colfg.blue = ~fg->color.blue;
			colfg.alpha = fg->color.alpha;
			XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, &revfg);
			fg = &revfg;
		}

		if(bg == &dc.col[defaultbg]) {
			bg = &dc.col[defaultfg];
		} else {
			colbg.red = ~bg->color.red;
			colbg.green = ~bg->color.green;
			colbg.blue = ~bg->color.blue;
			colbg.alpha = bg->color.alpha;
			XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colbg, &revbg);
			bg = &revbg;
		}
	}

	if(base.mode & ATTR_REVERSE) {
		temp = fg;
		fg = bg;
		bg = temp;
	}

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

	fcsets[0] = font->set;
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
		for(;;) {
			u8c = s;
			u8cblen = utf8decode(s, &u8char);
			s += u8cblen;
			bytelen -= u8cblen;

			doesexist = XftCharIndex(xw.dpy, font->match, u8char);
			if(!doesexist || bytelen <= 0) {
				if(bytelen <= 0) {
					if(doesexist) {
						u8fl++;
						u8fblen += u8cblen;
					}
				}

				if(u8fl > 0) {
					XftDrawStringUtf8(xw.draw, fg,
							font->match, xp,
							winy + font->ascent,
							(FcChar8 *)u8fs,
							u8fblen);
					xp += font->width * u8fl;
				}
				break;
			}

			u8fl++;
			u8fblen += u8cblen;
		}
		if(doesexist)
			break;

		frp = frccur;
		/* Search the font cache. */
		for(i = 0; i < frclen; i++, frp--) {
			if(frp <= 0)
				frp = LEN(frc) - 1;

			if(frc[frp].c == u8char
					&& frc[frp].flags == frcflags) {
				break;
			}
		}

		/* Nothing was found. */
		if(i >= frclen) {
			/*
			 * Nothing was found in the cache. Now use
			 * some dozen of Fontconfig calls to get the
			 * font for one single character.
			 */
			fcpattern = FcPatternDuplicate(font->pattern);
			fccharset = FcCharSetCreate();

			FcCharSetAddChar(fccharset, u8char);
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
			frccur++;
			frclen++;
			if(frccur >= LEN(frc))
				frccur = 0;
			if(frclen > LEN(frc)) {
				frclen = LEN(frc);
				XftFontClose(xw.dpy, frc[frccur].font);
			}

			frc[frccur].font = XftFontOpenPattern(xw.dpy,
					fontpattern);
			frc[frccur].c = u8char;
			frc[frccur].flags = frcflags;

			FcPatternDestroy(fcpattern);
			FcCharSetDestroy(fccharset);

			frp = frccur;
		}

		XftDrawStringUtf8(xw.draw, fg, frc[frp].font,
				xp, winy + frc[frp].font->ascent,
				(FcChar8 *)u8c, u8cblen);

		xp += font->width;
	}

	/*
	XftDrawStringUtf8(xw.draw, fg, font->set, winx,
			winy + font->ascent, (FcChar8 *)s, bytelen);
	*/

	if(base.mode & ATTR_UNDERLINE) {
		XftDrawRect(xw.draw, fg, winx, winy + font->ascent + 1,
				width, 1);
	}
}

void
xdrawcursor(void) {
	static int oldx = 0, oldy = 0;
	int sl;
	Glyph g = {{' '}, ATTR_NULL, defaultbg, defaultcs, 0};

	LIMIT(oldx, 0, term.col-1);
	LIMIT(oldy, 0, term.row-1);

	if(term.line[term.c.y][term.c.x].state & GLYPH_SET)
		memcpy(g.c, term.line[term.c.y][term.c.x].c, UTF_SIZ);

	/* remove the old cursor */
	if(term.line[oldy][oldx].state & GLYPH_SET) {
		sl = utf8size(term.line[oldy][oldx].c);
		xdraws(term.line[oldy][oldx].c, term.line[oldy][oldx], oldx,
				oldy, 1, sl);
	} else {
		xtermclear(oldx, oldy, oldx, oldy);
	}

	/* draw the new one */
	if(!(IS_SET(MODE_HIDE))) {
		if(!(xw.state & WIN_FOCUSED))
			g.bg = defaultucs;

		if(IS_SET(MODE_REVERSE))
			g.mode |= ATTR_REVERSE, g.fg = defaultcs, g.bg = defaultfg;

		sl = utf8size(g.c);
		xdraws(g.c, g, term.c.x, term.c.y, 1, sl);
		oldx = term.c.x, oldy = term.c.y;
	}
}


void
xsettitle(char *p) {
	XTextProperty prop;

	Xutf8TextListToTextProperty(xw.dpy, &p, 1, XUTF8StringStyle,
			&prop);
	XSetWMName(xw.dpy, xw.win, &prop);
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
	bool ena_sel = sel.bx != -1;

	if(sel.alt ^ IS_SET(MODE_ALTSCREEN))
		ena_sel = 0;

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
			if(ena_sel && *(new.c) && selected(x, y))
				new.mode ^= ATTR_REVERSE;
			if(ib > 0 && (!(new.state & GLYPH_SET)
					|| ATTRCMP(base, new)
					|| ib >= DRAW_BUF_SIZ-UTF_SIZ)) {
				xdraws(buf, base, ox, y, ic, ib);
				ic = ib = 0;
			}
			if(new.state & GLYPH_SET) {
				if(ib == 0) {
					ox = x;
					base = new;
				}

				sl = utf8size(new.c);
				memcpy(buf+ib, new.c, sl);
				ib += sl;
				++ic;
			}
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
xseturgency(int add) {
	XWMHints *h = XGetWMHints(xw.dpy, xw.win);

	h->flags = add ? (h->flags | XUrgencyHint) : (h->flags & ~XUrgencyHint);
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
	} else {
		XUnsetICFocus(xw.xic);
		xw.state &= ~WIN_FOCUSED;
	}
}

inline bool
match(uint mask, uint state) {
	state &= ~(ignoremod);

	if(mask == XK_NO_MOD && state)
		return false;
	if(mask != XK_ANY_MOD && mask != XK_NO_MOD && !state)
		return false;
	if((state & mask) != state)
		return false;
	return true;
}

void
numlock(const Arg *dummy) {
	term.numlock ^= 1;
}

char*
kmap(KeySym k, uint state) {
	uint mask;
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
		mask = kp->mask;

		if(kp->k != k)
			continue;

		if(!match(mask, state))
			continue;

		if(kp->appkey > 0) {
			if(!IS_SET(MODE_APPKEYPAD))
				continue;
			if(term.numlock && kp->appkey == 2)
				continue;
		} else if(kp->appkey < 0 && IS_SET(MODE_APPKEYPAD)) {
			continue;
		}

		if((kp->appcursor < 0 && IS_SET(MODE_APPCURSOR)) ||
				(kp->appcursor > 0
				 && !IS_SET(MODE_APPCURSOR))) {
			continue;
		}

		if((kp->crlf < 0 && IS_SET(MODE_CRLF)) ||
				(kp->crlf > 0 && !IS_SET(MODE_CRLF))) {
			continue;
		}

		return kp->s;
	}

	return NULL;
}

void
kpress(XEvent *ev) {
	XKeyEvent *e = &ev->xkey;
	KeySym ksym;
	char xstr[31], buf[32], *customkey, *cp = buf;
	int len;
	Status status;
	Shortcut *bp;

	if(IS_SET(MODE_KBDLOCK))
		return;

	len = XmbLookupString(xw.xic, e, xstr, sizeof(xstr), &ksym, &status);
	e->state &= ~Mod2Mask;
	/* 1. shortcuts */
	for(bp = shortcuts; bp < shortcuts + LEN(shortcuts); bp++) {
		if(ksym == bp->keysym && match(bp->mod, e->state)) {
			bp->func(&(bp->arg));
			return;
		}
	}

	/* 2. custom keys from config.h */
	if((customkey = kmap(ksym, e->state))) {
		len = strlen(customkey);
		memcpy(buf, customkey, len);
	/* 2. hardcoded (overrides X lookup) */
	} else {
		if(len == 0)
			return;

		if(len == 1 && e->state & Mod1Mask)
			*cp++ = '\033';

		memcpy(cp, xstr, len);
		len = cp - buf + len;
	}

	ttywrite(buf, len);
	if(IS_SET(MODE_ECHO))
		techo(buf, len);
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
	fd_set rfd;
	int xfd = XConnectionNumber(xw.dpy), xev;
	struct timeval drawtimeout, *tv = NULL, now, last;

	gettimeofday(&last, NULL);

	for(xev = actionfps;;) {
		FD_ZERO(&rfd);
		FD_SET(cmdfd, &rfd);
		FD_SET(xfd, &rfd);
		if(select(MAX(xfd, cmdfd)+1, &rfd, NULL, NULL, tv) < 0) {
			if(errno == EINTR)
				continue;
			die("select failed: %s\n", SERRNO);
		}

		gettimeofday(&now, NULL);
		drawtimeout.tv_sec = 0;
		drawtimeout.tv_usec = (1000/xfps) * 1000;
		tv = &drawtimeout;

		if(FD_ISSET(cmdfd, &rfd))
			ttyread();

		if(FD_ISSET(xfd, &rfd))
			xev = actionfps;

		if(TIMEDIFF(now, last) > \
				(xev ? (1000/xfps) : (1000/actionfps))) {
			while(XPending(xw.dpy)) {
				XNextEvent(xw.dpy, &ev);
				if(XFilterEvent(&ev, None))
					continue;
				if(handler[ev.type])
					(handler[ev.type])(&ev);
			}

			draw();
			XFlush(xw.dpy);
			last = now;

			if(xev && !FD_ISSET(xfd, &rfd))
				xev--;
			if(!FD_ISSET(cmdfd, &rfd) && !FD_ISSET(xfd, &rfd))
				tv = NULL;
		}
	}
}

int
main(int argc, char *argv[]) {
	int i, bitm, xr, yr;
	uint wr, hr;

	xw.fw = xw.fh = xw.fx = xw.fy = 0;
	xw.isfixed = False;

	for(i = 1; i < argc; i++) {
		switch(argv[i][0] != '-' || argv[i][2] ? -1 : argv[i][1]) {
		case 'c':
			if(++i < argc)
				opt_class = argv[i];
			break;
		case 'e':
			/* eat all remaining arguments */
			if(++i < argc)
				opt_cmd = &argv[i];
			goto run;
		case 'f':
			if(++i < argc)
				opt_font = argv[i];
			break;
		case 'g':
			if(++i >= argc)
				break;

			bitm = XParseGeometry(argv[i], &xr, &yr, &wr, &hr);
			if(bitm & XValue)
				xw.fx = xr;
			if(bitm & YValue)
				xw.fy = yr;
			if(bitm & WidthValue)
				xw.fw = (int)wr;
			if(bitm & HeightValue)
				xw.fh = (int)hr;
			if(bitm & XNegative && xw.fx == 0)
				xw.fx = -1;
			if(bitm & XNegative && xw.fy == 0)
				xw.fy = -1;

			if(xw.fh != 0 && xw.fw != 0)
				xw.isfixed = True;
			break;
		case 'o':
			if(++i < argc)
				opt_io = argv[i];
			break;
		case 't':
			if(++i < argc)
				opt_title = argv[i];
			break;
		case 'v':
		default:
			die(USAGE);
		case 'w':
			if(++i < argc)
				opt_embed = argv[i];
			break;
		}
	}

run:
	setlocale(LC_CTYPE, "");
	XSetLocaleModifiers("");
	tnew(80, 24);
	xinit();
	ttynew();
	selinit();
	run();

	return 0;
}

