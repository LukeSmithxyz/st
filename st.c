/* See LICENSE for licence details. */
#define _XOPEN_SOURCE 600
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
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
#include <X11/extensions/Xdbe.h>

#if   defined(__linux)
 #include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
 #include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
 #include <libutil.h>
#endif

#define USAGE \
	"st " VERSION " (c) 2010-2012 st engineers\n" \
	"usage: st [-t title] [-c class] [-g geometry]" \
	" [-w windowid] [-v] [-f file] [-e command...]\n"

/* XEMBED messages */
#define XEMBED_FOCUS_IN  4
#define XEMBED_FOCUS_OUT 5

/* Arbitrary sizes */
#define ESC_BUF_SIZ   256
#define ESC_ARG_SIZ   16
#define STR_BUF_SIZ   256
#define STR_ARG_SIZ   16
#define DRAW_BUF_SIZ  1024
#define UTF_SIZ       4
#define XK_NO_MOD     UINT_MAX
#define XK_ANY_MOD    0

#define REDRAW_TIMEOUT (80*1000) /* 80 ms */

#define SERRNO strerror(errno)
#define MIN(a, b)  ((a) < (b) ? (a) : (b))
#define MAX(a, b)  ((a) < (b) ? (b) : (a))
#define LEN(a)     (sizeof(a) / sizeof(a[0]))
#define DEFAULT(a, b)     (a) = (a) ? (a) : (b)
#define BETWEEN(x, a, b)  ((a) <= (x) && (x) <= (b))
#define LIMIT(x, a, b)    (x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x)
#define ATTRCMP(a, b) ((a).mode != (b).mode || (a).fg != (b).fg || (a).bg != (b).bg)
#define IS_SET(flag) (term.mode & (flag))
#define TIMEDIFF(t1, t2) ((t1.tv_sec-t2.tv_sec)*1000 + (t1.tv_usec-t2.tv_usec)/1000)
#define X2COL(x) (((x) - BORDER)/xw.cw)
#define Y2ROW(y) (((y) - BORDER)/xw.ch)

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
	CURSOR_UP,
	CURSOR_DOWN,
	CURSOR_LEFT,
	CURSOR_RIGHT,
	CURSOR_SAVE,
	CURSOR_LOAD
};

enum cursor_state {
	CURSOR_DEFAULT  = 0,
	CURSOR_HIDE     = 1,
	CURSOR_WRAPNEXT = 2
};

enum glyph_state {
	GLYPH_SET   = 1,
	GLYPH_DIRTY = 2
};

enum term_mode {
	MODE_WRAP	= 1,
	MODE_INSERT      = 2,
	MODE_APPKEYPAD   = 4,
	MODE_ALTSCREEN   = 8,
	MODE_CRLF	= 16,
	MODE_MOUSEBTN    = 32,
	MODE_MOUSEMOTION = 64,
	MODE_MOUSE       = 32|64,
	MODE_REVERSE     = 128
};

enum escape_state {
	ESC_START      = 1,
	ESC_CSI	= 2,
	ESC_STR	= 4, /* DSC, OSC, PM, APC */
	ESC_ALTCHARSET = 8,
	ESC_STR_END    = 16, /* a final string was encountered */
};

enum window_state {
	WIN_VISIBLE = 1,
	WIN_REDRAW  = 2,
	WIN_FOCUSED = 4
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

typedef Glyph* Line;

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
	int narg;	      /* nb of args */
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
	Line* line;	/* screen */
	Line* alt;	/* alternate screen */
	bool* dirty;	/* dirtyness of lines */
	TCursor c;	/* cursor */
	int top;	/* top    scroll limit */
	int bot;	/* bottom scroll limit */
	int mode;	/* terminal mode flags */
	int esc;	/* escape state flags */
	bool *tabs;
} Term;

/* Purely graphic info */
typedef struct {
	Display* dpy;
	Colormap cmap;
	Window win;
	XdbeBackBuffer buf;
	Atom xembed;
	XIM xim;
	XIC xic;
	int scr;
	Bool isfixed; /* is fixed geometry? */
	int fx, fy, fw, fh; /* fixed geometry */
	int w;	/* window width */
	int h;	/* window height */
	int ch; /* char height */
	int cw; /* char width  */
	char state; /* focus, redraw, visible */
} XWindow;

typedef struct {
	KeySym k;
	uint mask;
	char s[ESC_BUF_SIZ];
} Key;


/* TODO: use better name for vars... */
typedef struct {
	int mode;
	int bx, by;
	int ex, ey;
	struct {int x, y;} b, e;
	char *clip;
	Atom xtarget;
	bool alt;
	struct timeval tclick1;
	struct timeval tclick2;
} Selection;

#include "config.h"

/* Drawing Context */
typedef struct {
	ulong col[LEN(colorname) < 256 ? 256 : LEN(colorname)];
	GC gc;
	struct {
		int ascent;
		int descent;
		short lbearing;
		short rbearing;
		XFontSet set;
	} font, bfont, ifont, ibfont;
} DC;

static void die(const char*, ...);
static void draw(void);
static void redraw(void);
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

static void tclearregion(int, int, int, int);
static void tcursor(int);
static void tdeletechar(int);
static void tdeleteline(int);
static void tinsertblank(int);
static void tinsertblankline(int);
static void tmoveto(int, int);
static void tnew(int, int);
static void tnewline(int);
static void tputtab(bool);
static void tputc(char*);
static void treset(void);
static int tresize(int, int);
static void tscrollup(int, int);
static void tscrolldown(int, int);
static void tsetattr(int*, int);
static void tsetchar(char*);
static void tsetscroll(int, int);
static void tswapscreen(void);
static void tsetdirt(int, int);
static void tsetmode(bool, bool, int *, int);
static void tfulldirt(void);

static void ttynew(void);
static void ttyread(void);
static void ttyresize(int, int);
static void ttywrite(const char *, size_t);

static void xdraws(char *, Glyph, int, int, int, int);
static void xhints(void);
static void xclear(int, int, int, int);
static void xdrawcursor(void);
static void xinit(void);
static void xloadcols(void);
static void xresettitle(void);
static void xseturgency(int);
static void xsetsel(char*);
static void xresize(int, int);

static void expose(XEvent *);
static void visibility(XEvent *);
static void unmap(XEvent *);
static char* kmap(KeySym, uint);
static void kpress(XEvent *);
static void cmessage(XEvent *);
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
static void selpaste(void);
static void selscroll(int, int);

static int utf8decode(char *, long *);
static int utf8encode(long *, char *);
static int utf8size(char *);
static int isfullutf8(char *, int);

static void *xmalloc(size_t);
static void *xrealloc(void *, size_t);

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
static char **opt_cmd  = NULL;
static char *opt_io    = NULL;
static char *opt_title = NULL;
static char *opt_embed = NULL;
static char *opt_class = NULL;

void *
xmalloc(size_t len) {
	void *p = malloc(len);
	if(!p)
		die("Out of memory");
	return p;
}

void *
xrealloc(void *p, size_t len) {
	if((p = realloc(p, len)) == NULL)
		die("Out of memory");
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
	} else
		goto invalid;
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
	   (*u >= 0xD800 && *u <= 0xDFFF))
		goto invalid;
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

	sp = (uchar*) s;
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

	c1 = (uchar *) s;
	c2 = (uchar *) ++s;
	c3 = (uchar *) ++s;
	if(b < 1)
		return 0;
	else if((*c1&(B7|B6|B5)) == (B7|B6) && b == 1)
		return 0;
	else if((*c1&(B7|B6|B5|B4)) == (B7|B6|B5) &&
	    ((b == 1) ||
	    ((b == 2) && (*c2&(B7|B6)) == B7)))
		return 0;
	else if((*c1&(B7|B6|B5|B4|B3)) == (B7|B6|B5|B4) &&
	    ((b == 1) ||
	    ((b == 2) && (*c2&(B7|B6)) == B7) ||
	    ((b == 3) && (*c2&(B7|B6)) == B7 && (*c3&(B7|B6)) == B7)))
		return 0;
	else
		return 1;
}

int
utf8size(char *s) {
	uchar c = *s;

	if(~c&B7)
		return 1;
	else if((c&(B7|B6|B5)) == (B7|B6))
		return 2;
	else if((c&(B7|B6|B5|B4)) == (B7|B6|B5))
		return 3;
	else
		return 4;
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

static inline bool
selected(int x, int y) {
	if(sel.ey == y && sel.by == y) {
		int bx = MIN(sel.bx, sel.ex);
		int ex = MAX(sel.bx, sel.ex);
		return BETWEEN(x, bx, ex);
	}
	return ((sel.b.y < y&&y < sel.e.y) || (y==sel.e.y && x<=sel.e.x))
		|| (y==sel.b.y && x>=sel.b.x && (x<=sel.e.x || sel.b.y!=sel.e.y));
}

void
getbuttoninfo(XEvent *e, int *b, int *x, int *y) {
	if(b)
		*b = e->xbutton.button;

	*x = X2COL(e->xbutton.x);
	*y = Y2ROW(e->xbutton.y);
	sel.b.x = sel.by < sel.ey ? sel.bx : sel.ex;
	sel.b.y = MIN(sel.by, sel.ey);
	sel.e.x = sel.by < sel.ey ? sel.ex : sel.bx;
	sel.e.y = MAX(sel.by, sel.ey);
}

void
mousereport(XEvent *e) {
	int x = X2COL(e->xbutton.x);
	int y = Y2ROW(e->xbutton.y);
	int button = e->xbutton.button;
	int state = e->xbutton.state;
	char buf[] = { '\033', '[', 'M', 0, 32+x+1, 32+y+1 };
	static int ob, ox, oy;

	/* from urxvt */
	if(e->xbutton.type == MotionNotify) {
		if(!IS_SET(MODE_MOUSEMOTION) || (x == ox && y == oy))
			return;
		button = ob + 32;
		ox = x, oy = y;
	} else if(e->xbutton.type == ButtonRelease || button == AnyButton) {
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

	buf[3] = 32 + button + (state & ShiftMask ? 4 : 0)
		+ (state & Mod4Mask    ? 8  : 0)
		+ (state & ControlMask ? 16 : 0);

	ttywrite(buf, sizeof(buf));
}

void
bpress(XEvent *e) {
	if(IS_SET(MODE_MOUSE))
		mousereport(e);
	else if(e->xbutton.button == Button1) {
		if(sel.bx != -1) {
			sel.bx = -1;
			tsetdirt(sel.b.y, sel.e.y);
			draw();
		}
		sel.mode = 1;
		sel.ex = sel.bx = X2COL(e->xbutton.x);
		sel.ey = sel.by = Y2ROW(e->xbutton.y);
	}
}

void
selcopy(void) {
	char *str, *ptr;
	int x, y, bufsize, is_selected = 0;

	if(sel.bx == -1)
		str = NULL;

	else {
		bufsize = (term.col+1) * (sel.e.y-sel.b.y+1) * UTF_SIZ;
		ptr = str = xmalloc(bufsize);

		/* append every set & selected glyph to the selection */
		for(y = 0; y < term.row; y++) {
			for(x = 0; x < term.col; x++) {
				is_selected = selected(x, y);
				if((term.line[y][x].state & GLYPH_SET) && is_selected) {
					int size = utf8size(term.line[y][x].c);
					memcpy(ptr, term.line[y][x].c, size);
					ptr += size;
				}
			}

			/* \n at the end of every selected line except for the last one */
			if(is_selected && y < sel.e.y)
				*ptr++ = '\n';
		}
		*ptr = 0;
	}
	sel.alt = IS_SET(MODE_ALTSCREEN);
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
selpaste() {
	XConvertSelection(xw.dpy, XA_PRIMARY, sel.xtarget, XA_PRIMARY, xw.win, CurrentTime);
}

void selclear(XEvent *e) {
	if(sel.bx == -1)
		return;
	sel.bx = -1;
	tsetdirt(sel.b.y, sel.e.y);
}

void
selrequest(XEvent *e) {
	XSelectionRequestEvent *xsre;
	XSelectionEvent xev;
	Atom xa_targets;

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
		Atom string = sel.xtarget;
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
	if(IS_SET(MODE_MOUSE)) {
		mousereport(e);
		return;
	}
	if(e->xbutton.button == Button2)
		selpaste();
	else if(e->xbutton.button == Button1) {
		sel.mode = 0;
		getbuttoninfo(e, NULL, &sel.ex, &sel.ey);
		term.dirty[sel.ey] = 1;
		if(sel.bx == sel.ex && sel.by == sel.ey) {
			struct timeval now;
			sel.bx = -1;
			gettimeofday(&now, NULL);

			if(TIMEDIFF(now, sel.tclick2) <= TRIPLECLICK_TIMEOUT) {
				/* triple click on the line */
				sel.b.x = sel.bx = 0;
				sel.e.x = sel.ex = term.col;
				sel.b.y = sel.e.y = sel.ey;
				selcopy();
			} else if(TIMEDIFF(now, sel.tclick1) <= DOUBLECLICK_TIMEOUT) {
				/* double click to select word */
				sel.bx = sel.ex;
				while(sel.bx > 0 && term.line[sel.ey][sel.bx-1].state & GLYPH_SET &&
					  term.line[sel.ey][sel.bx-1].c[0] != ' ') sel.bx--;
				sel.b.x = sel.bx;
				while(sel.ex < term.col-1 && term.line[sel.ey][sel.ex+1].state & GLYPH_SET &&
					  term.line[sel.ey][sel.ex+1].c[0] != ' ') sel.ex++;
				sel.e.x = sel.ex;
				sel.b.y = sel.e.y = sel.ey;
				selcopy();
			}
		} else
			selcopy();
	}
	memcpy(&sel.tclick2, &sel.tclick1, sizeof(struct timeval));
	gettimeofday(&sel.tclick1, NULL);
}

void
bmotion(XEvent *e) {
	if(IS_SET(MODE_MOUSE)) {
		mousereport(e);
		return;
	}
	if(sel.mode) {
		int oldey = sel.ey, oldex = sel.ex;
		getbuttoninfo(e, NULL, &sel.ex, &sel.ey);

		if(oldey != sel.ey || oldex != sel.ex) {
			int starty = MIN(oldey, sel.ey);
			int endy = MAX(oldey, sel.ey);
			tsetdirt(starty, endy);
		}
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

	unsetenv("COLUMNS");
	unsetenv("LINES");
	unsetenv("TERMCAP");

	signal(SIGCHLD, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGALRM, SIG_DFL);

	DEFAULT(envshell, SHELL);
	putenv("TERM="TNAME);
	args = opt_cmd ? opt_cmd : (char*[]){envshell, "-i", NULL};
	execvp(args[0], args);
	exit(EXIT_FAILURE);
}

void
sigchld(int a) {
	int stat = 0;
	if(waitpid(pid, &stat, 0) < 0)
		die("Waiting for pid %hd failed: %s\n",	pid, SERRNO);
	if(WIFEXITED(stat))
		exit(WEXITSTATUS(stat));
	else
		exit(EXIT_FAILURE);
}

void
ttynew(void) {
	int m, s;

	/* seems to work fine on linux, openbsd and freebsd */
	struct winsize w = {term.row, term.col, 0, 0};
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
			if(!strcmp(opt_io, "-")) {
				iofd = STDOUT_FILENO;
			} else {
				if((iofd = open(opt_io, O_WRONLY | O_CREAT, 0666)) < 0) {
					fprintf(stderr, "Error opening %s:%s\n",
						opt_io, strerror(errno));
				}
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
		tputc(s);
		ptr    += charsize;
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
ttyresize(int x, int y) {
	struct winsize w;

	w.ws_row = term.row;
	w.ws_col = term.col;
	w.ws_xpixel = xw.w;
	w.ws_ypixel = xw.h;
	if(ioctl(cmdfd, TIOCSWINSZ, &w) < 0)
		fprintf(stderr, "Couldn't set window size: %s\n", SERRNO);
}

void
tsetdirt(int top, int bot)
{
	int i;

	LIMIT(top, 0, term.row-1);
	LIMIT(bot, 0, term.row-1);

	for(i = top; i <= bot; i++)
		term.dirty[i] = 1;
}

void
tfulldirt(void)
{
	tsetdirt(0, term.row-1);
}

void
tcursor(int mode) {
	static TCursor c;

	if(mode == CURSOR_SAVE)
		c = term.c;
	else if(mode == CURSOR_LOAD)
		term.c = c, tmoveto(c.x, c.y);
}

void
treset(void) {
	unsigned i;
	term.c = (TCursor){{
		.mode = ATTR_NULL,
		.fg = DefaultFG,
		.bg = DefaultBG
	}, .x = 0, .y = 0, .state = CURSOR_DEFAULT};

	memset(term.tabs, 0, term.col * sizeof(*term.tabs));
	for(i = TAB; i < term.col; i += TAB)
		term.tabs[i] = 1;
	term.top = 0, term.bot = term.row - 1;
	term.mode = MODE_WRAP;
	tclearregion(0, 0, term.col-1, term.row-1);
}

void
tnew(int col, int row) {
	/* set screen size */
	term.row = row, term.col = col;
	term.line = xmalloc(term.row * sizeof(Line));
	term.alt  = xmalloc(term.row * sizeof(Line));
	term.dirty = xmalloc(term.row * sizeof(*term.dirty));
	term.tabs = xmalloc(term.col * sizeof(*term.tabs));

	for(row = 0; row < term.row; row++) {
		term.line[row] = xmalloc(term.col * sizeof(Glyph));
		term.alt [row] = xmalloc(term.col * sizeof(Glyph));
		term.dirty[row] = 0;
	}
	memset(term.tabs, 0, term.col * sizeof(*term.tabs));
	/* setup screen */
	treset();
}

void
tswapscreen(void) {
	Line* tmp = term.line;
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

	tclearregion(0, term.bot-n+1, term.col-1, term.bot);

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

	tclearregion(0, orig, term.col-1, orig+n-1);

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
		if(sel.by < term.top) {
			sel.by = term.top;
			sel.bx = 0;
		}
		if(sel.ey > term.bot) {
			sel.ey = term.bot;
			sel.ex = term.col;
		}
		sel.b.y = sel.by, sel.b.x = sel.bx;
		sel.e.y = sel.ey, sel.e.x = sel.ex;
	}
}

void
tnewline(int first_col) {
	int y = term.c.y;
	if(y == term.bot)
		tscrollup(term.top, 1);
	else
		y++;
	tmoveto(first_col ? 0 : term.c.x, y);
}

void
csiparse(void) {
	/* int noarg = 1; */
	char *p = csiescseq.buf;

	csiescseq.narg = 0;
	if(*p == '?')
		csiescseq.priv = 1, p++;

	while(p < csiescseq.buf+csiescseq.len) {
		while(isdigit(*p)) {
			csiescseq.arg[csiescseq.narg] *= 10;
			csiescseq.arg[csiescseq.narg] += *p++ - '0'/*, noarg = 0 */;
		}
		if(*p == ';' && csiescseq.narg+1 < ESC_ARG_SIZ)
			csiescseq.narg++, p++;
		else {
			csiescseq.mode = *p;
			csiescseq.narg++;
			return;
		}
	}
}

void
tmoveto(int x, int y) {
	LIMIT(x, 0, term.col-1);
	LIMIT(y, 0, term.row-1);
	term.c.state &= ~CURSOR_WRAPNEXT;
	term.c.x = x;
	term.c.y = y;
}

void
tsetchar(char *c) {
	term.dirty[term.c.y] = 1;
	term.line[term.c.y][term.c.x] = term.c.attr;
	memcpy(term.line[term.c.y][term.c.x].c, c, UTF_SIZ);
	term.line[term.c.y][term.c.x].state |= GLYPH_SET;
}

void
tclearregion(int x1, int y1, int x2, int y2) {
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
		for(x = x1; x <= x2; x++)
			term.line[y][x].state = 0;
	}
}

void
tdeletechar(int n) {
	int src = term.c.x + n;
	int dst = term.c.x;
	int size = term.col - src;

	term.dirty[term.c.y] = 1;

	if(src >= term.col) {
		tclearregion(term.c.x, term.c.y, term.col-1, term.c.y);
		return;
	}
	memmove(&term.line[term.c.y][dst], &term.line[term.c.y][src], size * sizeof(Glyph));
	tclearregion(term.col-n, term.c.y, term.col-1, term.c.y);
}

void
tinsertblank(int n) {
	int src = term.c.x;
	int dst = src + n;
	int size = term.col - dst;

	term.dirty[term.c.y] = 1;

	if(dst >= term.col) {
		tclearregion(term.c.x, term.c.y, term.col-1, term.c.y);
		return;
	}
	memmove(&term.line[term.c.y][dst], &term.line[term.c.y][src], size * sizeof(Glyph));
	tclearregion(src, term.c.y, dst - 1, term.c.y);
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
			term.c.attr.fg = DefaultFG;
			term.c.attr.bg = DefaultBG;
			break;
		case 1:
			term.c.attr.mode |= ATTR_BOLD;
			break;
		case 3: /* enter standout (highlight) */
			term.c.attr.mode |= ATTR_ITALIC;
			break;
		case 4:
			term.c.attr.mode |= ATTR_UNDERLINE;
			break;
		case 5:
			term.c.attr.mode |= ATTR_BLINK;
			break;
		case 7:
			term.c.attr.mode |= ATTR_REVERSE;
			break;
		case 21:
		case 22:
			term.c.attr.mode &= ~ATTR_BOLD;
			break;
		case 23: /* leave standout (highlight) mode */
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
		case 38:
			if(i + 2 < l && attr[i + 1] == 5) {
				i += 2;
				if(BETWEEN(attr[i], 0, 255))
					term.c.attr.fg = attr[i];
				else
					fprintf(stderr, "erresc: bad fgcolor %d\n", attr[i]);
			}
			else
				fprintf(stderr, "erresc(38): gfx attr %d unknown\n", attr[i]);
			break;
		case 39:
			term.c.attr.fg = DefaultFG;
			break;
		case 48:
			if(i + 2 < l && attr[i + 1] == 5) {
				i += 2;
				if(BETWEEN(attr[i], 0, 255))
					term.c.attr.bg = attr[i];
				else
					fprintf(stderr, "erresc: bad bgcolor %d\n", attr[i]);
			}
			else
				fprintf(stderr, "erresc(48): gfx attr %d unknown\n", attr[i]);
			break;
		case 49:
			term.c.attr.bg = DefaultBG;
			break;
		default:
			if(BETWEEN(attr[i], 30, 37))
				term.c.attr.fg = attr[i] - 30;
			else if(BETWEEN(attr[i], 40, 47))
				term.c.attr.bg = attr[i] - 40;
			else if(BETWEEN(attr[i], 90, 97))
				term.c.attr.fg = attr[i] - 90 + 8;
			else if(BETWEEN(attr[i], 100, 107))
				term.c.attr.bg = attr[i] - 100 + 8;
			else
				fprintf(stderr, "erresc(default): gfx attr %d unknown\n", attr[i]), csidump();
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

	for(lim = args + narg; args < lim; ++args) {
		if(priv) {
			switch(*args) {
			case 1:
				MODBIT(term.mode, set, MODE_APPKEYPAD);
				break;
			case 5: /* DECSCNM -- Reverve video */
				mode = term.mode;
				MODBIT(term.mode,set, MODE_REVERSE);
				if(mode != term.mode)
					redraw();
				break;
			case 7:
				MODBIT(term.mode, set, MODE_WRAP);
				break;
			case 20:
				MODBIT(term.mode, set, MODE_CRLF);
				break;
			case 12: /* att610 -- Start blinking cursor (IGNORED) */
				break;
			case 25:
				MODBIT(term.c.state, !set, CURSOR_HIDE);
				break;
			case 1000: /* 1000,1002: enable xterm mouse report */
				MODBIT(term.mode, set, MODE_MOUSEBTN);
				break;
			case 1002:
				MODBIT(term.mode, set, MODE_MOUSEMOTION);
				break;
			case 1049: /* = 1047 and 1048 */
			case 47:
			case 1047:
				if(IS_SET(MODE_ALTSCREEN))
					tclearregion(0, 0, term.col-1, term.row-1);
				if((set && !IS_SET(MODE_ALTSCREEN)) ||
				    (!set && IS_SET(MODE_ALTSCREEN))) {
					    tswapscreen();
				}
				if(*args != 1049)
					break;
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
			case 4:
				MODBIT(term.mode, set, MODE_INSERT);
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
	case 'e':
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term.c.x, term.c.y-csiescseq.arg[0]);
		break;
	case 'B': /* CUD -- Cursor <n> Down */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term.c.x, term.c.y+csiescseq.arg[0]);
		break;
	case 'C': /* CUF -- Cursor <n> Forward */
	case 'a':
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
		switch (csiescseq.arg[0]) {
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
		tmoveto(csiescseq.arg[1]-1, csiescseq.arg[0]-1);
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
			tclearregion(term.c.x, term.c.y, term.col-1, term.c.y);
			if(term.c.y < term.row-1)
				tclearregion(0, term.c.y+1, term.col-1, term.row-1);
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
			tclearregion(term.c.x, term.c.y, term.col-1, term.c.y);
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
		tclearregion(term.c.x, term.c.y, term.c.x + csiescseq.arg[0], term.c.y);
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
		tmoveto(term.c.x, csiescseq.arg[0]-1);
		break;
	case 'h': /* SM -- Set terminal mode */
		tsetmode(csiescseq.priv, 1, csiescseq.arg, csiescseq.narg);
		break;
	case 'm': /* SGR -- Terminal attribute (color) */
		tsetattr(csiescseq.arg, csiescseq.narg);
		break;
	case 'r': /* DECSTBM -- Set Scrolling Region */
		if(csiescseq.priv)
			goto unknown;
		else {
			DEFAULT(csiescseq.arg[0], 1);
			DEFAULT(csiescseq.arg[1], term.row);
			tsetscroll(csiescseq.arg[0]-1, csiescseq.arg[1]-1);
			tmoveto(0, 0);
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
	printf("ESC[");
	for(i = 0; i < csiescseq.len; i++) {
		uint c = csiescseq.buf[i] & 0xff;
		if(isprint(c)) putchar(c);
		else if(c == '\n') printf("(\\n)");
		else if(c == '\r') printf("(\\r)");
		else if(c == 0x1b) printf("(\\e)");
		else printf("(%02x)", c);
	}
	putchar('\n');
}

void
csireset(void) {
	memset(&csiescseq, 0, sizeof(csiescseq));
}

void
strhandle(void) {
	char *p;

	/*
	 * TODO: make this being useful in case of color palette change.
	 */
	strparse();

	p = strescseq.buf;

	switch(strescseq.type) {
	case ']': /* OSC -- Operating System Command */
		switch(p[0]) {
		case '0':
		case '1':
		case '2':
			/*
			 * TODO: Handle special chars in string, like umlauts.
			 */
			if(p[1] == ';') {
				XStoreName(xw.dpy, xw.win, strescseq.buf+2);
			}
			break;
		case ';':
			XStoreName(xw.dpy, xw.win, strescseq.buf+1);
			break;
		case '4': /* TODO: Set color (arg0) to "rgb:%hexr/$hexg/$hexb" (arg1) */
			break;
		default:
			fprintf(stderr, "erresc: unknown str ");
			strdump();
			break;
		}
		break;
	case 'k': /* old title set compatibility */
		XStoreName(xw.dpy, xw.win, strescseq.buf);
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
	/*
	 * TODO: Implement parsing like for CSI when required.
	 * Format: ESC type cmd ';' arg0 [';' argn] ESC \
	 */
	return;
}

void
strdump(void) {
	int i;
	printf("ESC%c", strescseq.type);
	for(i = 0; i < strescseq.len; i++) {
		uint c = strescseq.buf[i] & 0xff;
		if(isprint(c)) putchar(c);
		else if(c == '\n') printf("(\\n)");
		else if(c == '\r') printf("(\\r)");
		else if(c == 0x1b) printf("(\\e)");
		else printf("(%02x)", c);
	}
	printf("ESC\\\n");
}

void
strreset(void) {
	memset(&strescseq, 0, sizeof(strescseq));
}

void
tputtab(bool forward) {
	unsigned x = term.c.x;

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
tputc(char *c) {
	char ascii = *c;

	if(iofd != -1)
		write(iofd, c, 1);

	if(term.esc & ESC_START) {
		if(term.esc & ESC_CSI) {
			csiescseq.buf[csiescseq.len++] = ascii;
			if(BETWEEN(ascii, 0x40, 0x7E) || csiescseq.len >= ESC_BUF_SIZ) {
				term.esc = 0;
				csiparse(), csihandle();
			}
		} else if(term.esc & ESC_STR) {
			switch(ascii) {
			case '\033':
				term.esc = ESC_START | ESC_STR_END;
				break;
			case '\a': /* backwards compatibility to xterm */
				term.esc = 0;
				strhandle();
				break;
			default:
				strescseq.buf[strescseq.len++] = ascii;
				if(strescseq.len+1 >= STR_BUF_SIZ) {
					term.esc = 0;
					strhandle();
				}
			}
		} else if(term.esc & ESC_STR_END) {
			term.esc = 0;
			if(ascii == '\\')
				strhandle();
		} else if(term.esc & ESC_ALTCHARSET) {
			switch(ascii) {
			case '0': /* Line drawing crap */
				term.c.attr.mode |= ATTR_GFX;
				break;
			case 'B': /* Back to regular text */
				term.c.attr.mode &= ~ATTR_GFX;
				break;
			default:
				fprintf(stderr, "esc unhandled charset: ESC ( %c\n", ascii);
			}
			term.esc = 0;
		} else {
			switch(ascii) {
			case '[':
				term.esc |= ESC_CSI;
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
			case '(':
				term.esc |= ESC_ALTCHARSET;
				break;
			case 'D': /* IND -- Linefeed */
				if(term.c.y == term.bot)
					tscrollup(term.top, 1);
				else
					tmoveto(term.c.x, term.c.y+1);
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
				if(term.c.y == term.top)
					tscrolldown(term.top, 1);
				else
					tmoveto(term.c.x, term.c.y-1);
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
				    (uchar) ascii, isprint(ascii)?ascii:'.');
				term.esc = 0;
			}
		}
	} else {
		if(sel.bx != -1 && BETWEEN(term.c.y, sel.by, sel.ey))
			sel.bx = -1;
		switch(ascii) {
		case '\0': /* padding character, do nothing */
			break;
		case '\t':
			tputtab(1);
			break;
		case '\b':
			tmoveto(term.c.x-1, term.c.y);
			break;
		case '\r':
			tmoveto(0, term.c.y);
			break;
		case '\f':
		case '\v':
		case '\n':
			/* go to first col if the mode is set */
			tnewline(IS_SET(MODE_CRLF));
			break;
		case '\a':
			if(!(xw.state & WIN_FOCUSED))
				xseturgency(1);
			break;
		case '\033':
			csireset();
			term.esc = ESC_START;
			break;
		default:
			if(IS_SET(MODE_WRAP) && term.c.state & CURSOR_WRAPNEXT)
				tnewline(1); /* always go to first col */
			tsetchar(c);
			if(term.c.x+1 < term.col)
				tmoveto(term.c.x+1, term.c.y);
			else
				term.c.state |= CURSOR_WRAPNEXT;
		}
	}
}

int
tresize(int col, int row) {
	int i, x;
	int minrow = MIN(row, term.row);
	int mincol = MIN(col, term.col);
	int slide = term.c.y - row + 1;

	if(col < 1 || row < 1)
		return 0;

	/* free unneeded rows */
	i = 0;
	if(slide > 0) {
		/* slide screen to keep cursor where we expect it -
		 * tscrollup would work here, but we can optimize to
		 * memmove because we're freeing the earlier lines */
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
		term.line[i] = calloc(col, sizeof(Glyph));
		term.alt [i] = calloc(col, sizeof(Glyph));
	}
	if(col > term.col) {
		bool *bp = term.tabs + term.col;

		memset(bp, 0, sizeof(*term.tabs) * (col - term.col));
		while(--bp > term.tabs && !*bp)
			/* nothing */ ;
		for(bp += TAB; bp < term.tabs + col; bp += TAB)
			*bp = 1;
	}
	/* update terminal size */
	term.col = col, term.row = row;
	/* make use of the LIMIT in tmoveto */
	tmoveto(term.c.x, term.c.y);
	/* reset scrolling region */
	tsetscroll(0, row-1);

	return (slide > 0);
}

void
xresize(int col, int row) {
	xw.w = MAX(1, 2*BORDER + col * xw.cw);
	xw.h = MAX(1, 2*BORDER + row * xw.ch);
}

void
xloadcols(void) {
	int i, r, g, b;
	XColor color;
	ulong white = WhitePixel(xw.dpy, xw.scr);

	/* load colors [0-15] colors and [256-LEN(colorname)[ (config.h) */
	for(i = 0; i < LEN(colorname); i++) {
		if(!colorname[i])
			continue;
		if(!XAllocNamedColor(xw.dpy, xw.cmap, colorname[i], &color, &color)) {
			dc.col[i] = white;
			fprintf(stderr, "Could not allocate color '%s'\n", colorname[i]);
		} else
			dc.col[i] = color.pixel;
	}

	/* load colors [16-255] ; same colors as xterm */
	for(i = 16, r = 0; r < 6; r++)
		for(g = 0; g < 6; g++)
			for(b = 0; b < 6; b++) {
				color.red = r == 0 ? 0 : 0x3737 + 0x2828 * r;
				color.green = g == 0 ? 0 : 0x3737 + 0x2828 * g;
				color.blue = b == 0 ? 0 : 0x3737 + 0x2828 * b;
				if(!XAllocColor(xw.dpy, xw.cmap, &color)) {
					dc.col[i] = white;
					fprintf(stderr, "Could not allocate color %d\n", i);
				} else
					dc.col[i] = color.pixel;
				i++;
			}

	for(r = 0; r < 24; r++, i++) {
		color.red = color.green = color.blue = 0x0808 + 0x0a0a * r;
		if(!XAllocColor(xw.dpy, xw.cmap, &color)) {
			dc.col[i] = white;
			fprintf(stderr, "Could not allocate color %d\n", i);
		} else
			dc.col[i] = color.pixel;
	}
}

void
xclear(int x1, int y1, int x2, int y2) {
	XSetForeground(xw.dpy, dc.gc, dc.col[IS_SET(MODE_REVERSE) ? DefaultFG : DefaultBG]);
	XFillRectangle(xw.dpy, xw.buf, dc.gc,
		       BORDER + x1 * xw.cw, BORDER + y1 * xw.ch,
		       (x2-x1+1) * xw.cw, (y2-y1+1) * xw.ch);
}

void
xhints(void) {
	XClassHint class = {opt_class ? opt_class : TNAME, TNAME};
	XWMHints wm = {.flags = InputHint, .input = 1};
	XSizeHints *sizeh = NULL;

	sizeh = XAllocSizeHints();
	if(xw.isfixed == False) {
		sizeh->flags = PSize | PResizeInc | PBaseSize;
		sizeh->height = xw.h;
		sizeh->width = xw.w;
		sizeh->height_inc = xw.ch;
		sizeh->width_inc = xw.cw;
		sizeh->base_height = 2*BORDER;
		sizeh->base_width = 2*BORDER;
	} else {
		sizeh->flags = PMaxSize | PMinSize;
		sizeh->min_width = sizeh->max_width = xw.fw;
		sizeh->min_height = sizeh->max_height = xw.fh;
	}

	XSetWMProperties(xw.dpy, xw.win, NULL, NULL, NULL, 0, sizeh, &wm, &class);
	XFree(sizeh);
}

XFontSet
xinitfont(char *fontstr) {
	XFontSet set;
	char *def, **missing;
	int n;

	missing = NULL;
	set = XCreateFontSet(xw.dpy, fontstr, &missing, &n, &def);
	if(missing) {
		while(n--)
			fprintf(stderr, "st: missing fontset: %s\n", missing[n]);
		XFreeStringList(missing);
	}
	return set;
}

void
xgetfontinfo(XFontSet set, int *ascent, int *descent, short *lbearing, short *rbearing) {
	XFontStruct **xfonts;
	char **font_names;
	int i, n;

	*ascent = *descent = *lbearing = *rbearing = 0;
	n = XFontsOfFontSet(set, &xfonts, &font_names);
	for(i = 0; i < n; i++) {
		*ascent = MAX(*ascent, (*xfonts)->ascent);
		*descent = MAX(*descent, (*xfonts)->descent);
		*lbearing = MAX(*lbearing, (*xfonts)->min_bounds.lbearing);
		*rbearing = MAX(*rbearing, (*xfonts)->max_bounds.rbearing);
		xfonts++;
	}
}

void
initfonts(char *fontstr, char *bfontstr, char *ifontstr, char *ibfontstr) {
	if((dc.font.set = xinitfont(fontstr)) == NULL)
		die("Can't load font %s\n", fontstr);
	if((dc.bfont.set = xinitfont(bfontstr)) == NULL)
		die("Can't load bfont %s\n", bfontstr);
	if((dc.ifont.set = xinitfont(ifontstr)) == NULL)
		die("Can't load ifont %s\n", ifontstr);
	if((dc.ibfont.set = xinitfont(ibfontstr)) == NULL)
		die("Can't load ibfont %s\n", ibfontstr);

	xgetfontinfo(dc.font.set, &dc.font.ascent, &dc.font.descent,
	    &dc.font.lbearing, &dc.font.rbearing);
	xgetfontinfo(dc.bfont.set, &dc.bfont.ascent, &dc.bfont.descent,
	    &dc.bfont.lbearing, &dc.bfont.rbearing);
	xgetfontinfo(dc.ifont.set, &dc.ifont.ascent, &dc.ifont.descent,
	    &dc.ifont.lbearing, &dc.ifont.rbearing);
	xgetfontinfo(dc.ibfont.set, &dc.ibfont.ascent, &dc.ibfont.descent,
	    &dc.ibfont.lbearing, &dc.ibfont.rbearing);
}

void
xinit(void) {
	XSetWindowAttributes attrs;
	Cursor cursor;
	Window parent;
	int sw, sh, major, minor;

	if(!(xw.dpy = XOpenDisplay(NULL)))
		die("Can't open display\n");
	xw.scr = XDefaultScreen(xw.dpy);

	/* font */
	initfonts(FONT, BOLDFONT, ITALICFONT, ITALICBOLDFONT);

	/* XXX: Assuming same size for bold font */
	xw.cw = dc.font.rbearing - dc.font.lbearing;
	xw.ch = dc.font.ascent + dc.font.descent;

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
		xw.h = 2*BORDER + term.row * xw.ch;
		xw.w = 2*BORDER + term.col * xw.cw;
		xw.fx = 0;
		xw.fy = 0;
	}

	attrs.background_pixel = dc.col[DefaultBG];
	attrs.border_pixel = dc.col[DefaultBG];
	attrs.bit_gravity = NorthWestGravity;
	attrs.event_mask = FocusChangeMask | KeyPressMask
		| ExposureMask | VisibilityChangeMask | StructureNotifyMask
		| ButtonMotionMask | ButtonPressMask | ButtonReleaseMask;
	attrs.colormap = xw.cmap;

	parent = opt_embed ? strtol(opt_embed, NULL, 0) : XRootWindow(xw.dpy, xw.scr);
	xw.win = XCreateWindow(xw.dpy, parent, xw.fx, xw.fy,
			xw.w, xw.h, 0, XDefaultDepth(xw.dpy, xw.scr), InputOutput,
			XDefaultVisual(xw.dpy, xw.scr),
			CWBackPixel | CWBorderPixel | CWBitGravity | CWEventMask
			| CWColormap,
			&attrs);
	if(!XdbeQueryExtension(xw.dpy, &major, &minor))
		die("Xdbe extension is not present\n");
	xw.buf = XdbeAllocateBackBufferName(xw.dpy, xw.win, XdbeCopied);

	/* input methods */
	xw.xim = XOpenIM(xw.dpy, NULL, NULL, NULL);
	xw.xic = XCreateIC(xw.xim, XNInputStyle, XIMPreeditNothing
					   | XIMStatusNothing, XNClientWindow, xw.win,
					   XNFocusWindow, xw.win, NULL);
	/* gc */
	dc.gc = XCreateGC(xw.dpy, xw.win, 0, NULL);

	/* white cursor, black outline */
	cursor = XCreateFontCursor(xw.dpy, XC_xterm);
	XDefineCursor(xw.dpy, xw.win, cursor);
	XRecolorCursor(xw.dpy, cursor,
		&(XColor){.red = 0xffff, .green = 0xffff, .blue = 0xffff},
		&(XColor){.red = 0x0000, .green = 0x0000, .blue = 0x0000});

	xw.xembed = XInternAtom(xw.dpy, "_XEMBED", False);

	xresettitle();
	XMapWindow(xw.dpy, xw.win);
	xhints();
	XSync(xw.dpy, 0);
}

void
xdraws(char *s, Glyph base, int x, int y, int charlen, int bytelen) {
	int fg = base.fg, bg = base.bg, temp;
	int winx = BORDER+x*xw.cw, winy = BORDER+y*xw.ch + dc.font.ascent, width = charlen*xw.cw;
	XFontSet fontset = dc.font.set;
	int i;

	/* only switch default fg/bg if term is in RV mode */
	if(IS_SET(MODE_REVERSE)) {
		if(fg == DefaultFG)
			fg = DefaultBG;
		if(bg == DefaultBG)
			bg = DefaultFG;
	}

	if(base.mode & ATTR_REVERSE)
		temp = fg, fg = bg, bg = temp;

	if(base.mode & ATTR_BOLD) {
		fg += 8;
		fontset = dc.bfont.set;
	}

	if(base.mode & ATTR_ITALIC)
		fontset = dc.ifont.set;
	if(base.mode & (ATTR_ITALIC|ATTR_ITALIC))
		fontset = dc.ibfont.set;

	XSetBackground(xw.dpy, dc.gc, dc.col[bg]);
	XSetForeground(xw.dpy, dc.gc, dc.col[fg]);

	if(base.mode & ATTR_GFX) {
		for(i = 0; i < bytelen; i++) {
			char c = gfx[(uint)s[i] % 256];
			if(c)
				s[i] = c;
			else if(s[i] > 0x5f)
				s[i] -= 0x5f;
		}
	}

	XmbDrawImageString(xw.dpy, xw.buf, fontset, dc.gc, winx, winy, s, bytelen);

	if(base.mode & ATTR_UNDERLINE)
		XDrawLine(xw.dpy, xw.buf, dc.gc, winx, winy+1, winx+width-1, winy+1);
}

void
xdrawcursor(void) {
	static int oldx = 0;
	static int oldy = 0;
	int sl;
	Glyph g = {{' '}, ATTR_NULL, DefaultBG, DefaultCS, 0};

	LIMIT(oldx, 0, term.col-1);
	LIMIT(oldy, 0, term.row-1);

	if(term.line[term.c.y][term.c.x].state & GLYPH_SET)
		memcpy(g.c, term.line[term.c.y][term.c.x].c, UTF_SIZ);

	/* remove the old cursor */
	if(term.line[oldy][oldx].state & GLYPH_SET) {
		sl = utf8size(term.line[oldy][oldx].c);
		xdraws(term.line[oldy][oldx].c, term.line[oldy][oldx], oldx, oldy, 1, sl);
	} else
		xclear(oldx, oldy, oldx, oldy);

	/* draw the new one */
	if(!(term.c.state & CURSOR_HIDE)) {
		if(!(xw.state & WIN_FOCUSED))
			g.bg = DefaultUCS;

		if(IS_SET(MODE_REVERSE))
			g.mode |= ATTR_REVERSE, g.fg = DefaultCS, g.bg = DefaultFG;

		sl = utf8size(g.c);
		xdraws(g.c, g, term.c.x, term.c.y, 1, sl);
		oldx = term.c.x, oldy = term.c.y;
	}
}

void
xresettitle(void) {
	XStoreName(xw.dpy, xw.win, opt_title ? opt_title : "st");
}

void
redraw(void) {
	struct timespec tv = {0, REDRAW_TIMEOUT * 1000};
	tfulldirt();
	draw();
	XSync(xw.dpy, False); /* necessary for a good tput flash */
	nanosleep(&tv, NULL);
}

void
draw() {
	XdbeSwapInfo swpinfo[1] = {{xw.win, XdbeCopied}};

	drawregion(0, 0, term.col, term.row);
	XdbeSwapBuffers(xw.dpy, swpinfo, 1);
}

void
drawregion(int x1, int y1, int x2, int y2) {
	int ic, ib, x, y, ox, sl;
	Glyph base, new;
	char buf[DRAW_BUF_SIZ];
	bool ena_sel = sel.bx != -1, alt = IS_SET(MODE_ALTSCREEN);

	if((sel.alt && !alt) || (!sel.alt && alt))
		ena_sel = 0;
	if(!(xw.state & WIN_VISIBLE))
		return;

	for(y = y1; y < y2; y++) {
		if(!term.dirty[y])
			continue;
		xclear(0, y, term.col, y);
		term.dirty[y] = 0;
		base = term.line[y][0];
		ic = ib = ox = 0;
		for(x = x1; x < x2; x++) {
			new = term.line[y][x];
			if(ena_sel && *(new.c) && selected(x, y))
				new.mode ^= ATTR_REVERSE;
			if(ib > 0 && (!(new.state & GLYPH_SET) || ATTRCMP(base, new) ||
						  ib >= DRAW_BUF_SIZ-UTF_SIZ)) {
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
}

void
visibility(XEvent *ev) {
	XVisibilityEvent *e = &ev->xvisibility;
	if(e->state == VisibilityFullyObscured)
		xw.state &= ~WIN_VISIBLE;
	else if(!(xw.state & WIN_VISIBLE))
		/* need a full redraw for next Expose, not just a buf copy */
		xw.state |= WIN_VISIBLE | WIN_REDRAW;
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
	if(ev->type == FocusIn) {
		xw.state |= WIN_FOCUSED;
		xseturgency(0);
	} else
		xw.state &= ~WIN_FOCUSED;
}

char*
kmap(KeySym k, uint state) {
	int i;
	state &= ~Mod2Mask;
	for(i = 0; i < LEN(key); i++) {
		uint mask = key[i].mask;
		if(key[i].k == k && ((state & mask) == mask || (mask == XK_NO_MOD && !state)))
			return (char*)key[i].s;
	}
	return NULL;
}

void
kpress(XEvent *ev) {
	XKeyEvent *e = &ev->xkey;
	KeySym ksym;
	char buf[32];
	char *customkey;
	int len;
	int meta;
	int shift;
	Status status;

	meta = e->state & Mod1Mask;
	shift = e->state & ShiftMask;
	len = XmbLookupString(xw.xic, e, buf, sizeof(buf), &ksym, &status);

	/* 1. custom keys from config.h */
	if((customkey = kmap(ksym, e->state)))
		ttywrite(customkey, strlen(customkey));
	/* 2. hardcoded (overrides X lookup) */
	else
		switch(ksym) {
		case XK_Up:
		case XK_Down:
		case XK_Left:
		case XK_Right:
			/* XXX: shift up/down doesn't work */
			sprintf(buf, "\033%c%c", IS_SET(MODE_APPKEYPAD) ? 'O' : '[', (shift ? "dacb":"DACB")[ksym - XK_Left]);
			ttywrite(buf, 3);
			break;
		case XK_Insert:
			if(shift)
				selpaste();
			break;
		case XK_Return:
			if(IS_SET(MODE_CRLF))
				ttywrite("\r\n", 2);
			else
				ttywrite("\r", 1);
			break;
			/* 3. X lookup  */
		default:
			if(len > 0) {
				if(meta && len == 1)
					ttywrite("\033", 1);
				ttywrite(buf, len);
			}
			break;
		}
}

void
cmessage(XEvent *e) {
	/* See xembed specs
	   http://standards.freedesktop.org/xembed-spec/xembed-spec-latest.html */
	if(e->xclient.message_type == xw.xembed && e->xclient.format == 32) {
		if(e->xclient.data.l[1] == XEMBED_FOCUS_IN) {
			xw.state |= WIN_FOCUSED;
			xseturgency(0);
		} else if(e->xclient.data.l[1] == XEMBED_FOCUS_OUT) {
			xw.state &= ~WIN_FOCUSED;
		}
	}
}

void
resize(XEvent *e) {
	int col, row;

	if(e->xconfigure.width == xw.w && e->xconfigure.height == xw.h)
		return;

	xw.w = e->xconfigure.width;
	xw.h = e->xconfigure.height;
	col = (xw.w - 2*BORDER) / xw.cw;
	row = (xw.h - 2*BORDER) / xw.ch;
	if(col == term.col && row == term.row)
		return;
	if(tresize(col, row))
		draw();
	xresize(col, row);
	ttyresize(col, row);
}

void
run(void) {
	XEvent ev;
	fd_set rfd;
	int xfd = XConnectionNumber(xw.dpy);

	for(;;) {
		FD_ZERO(&rfd);
		FD_SET(cmdfd, &rfd);
		FD_SET(xfd, &rfd);
		if(select(MAX(xfd, cmdfd)+1, &rfd, NULL, NULL, NULL) < 0) {
			if(errno == EINTR)
				continue;
			die("select failed: %s\n", SERRNO);
		}
		if(FD_ISSET(cmdfd, &rfd))
			ttyread();

		while(XPending(xw.dpy)) {
			XNextEvent(xw.dpy, &ev);
			if(XFilterEvent(&ev, xw.win))
				continue;
			if(handler[ev.type])
				(handler[ev.type])(&ev);
		}

		draw();
		XFlush(xw.dpy);
	}
}

int
main(int argc, char *argv[]) {
	int i, bitm, xr, yr;
	unsigned int wr, hr;

	xw.fw = xw.fh = xw.fx = xw.fy = 0;
	xw.isfixed = False;

	for(i = 1; i < argc; i++) {
		switch(argv[i][0] != '-' || argv[i][2] ? -1 : argv[i][1]) {
		case 't':
			if(++i < argc) opt_title = argv[i];
			break;
		case 'c':
			if(++i < argc) opt_class = argv[i];
			break;
		case 'w':
			if(++i < argc) opt_embed = argv[i];
			break;
		case 'f':
			if(++i < argc) opt_io = argv[i];
			break;
		case 'e':
			/* eat every remaining arguments */
			if(++i < argc) opt_cmd = &argv[i];
			goto run;
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
		case 'v':
		default:
			die(USAGE);
		}
	}

 run:
	setlocale(LC_CTYPE, "");
	tnew(80, 24);
	ttynew();
	xinit();
	selinit();
	run();
	return 0;
}

