/* See LICENSE for licence details. */
#define _XOPEN_SOURCE 600
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include <sys/time.h>
#include <time.h>

#if   defined(__linux)
 #include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
 #include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
 #include <libutil.h>
#endif

#define USAGE \
	"st-" VERSION ", (c) 2010-2011 st engineers\n" \
	"usage: st [-t title] [-c class] [-v] [-e command...]\n"

/* Arbitrary sizes */
#define ESC_TITLE_SIZ 256
#define ESC_BUF_SIZ   256
#define ESC_ARG_SIZ   16
#define DRAW_BUF_SIZ  1024
#define UTF_SIZ       4

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

/* Attribute, Cursor, Character state, Terminal mode, Screen draw mode */
enum { ATTR_NULL=0 , ATTR_REVERSE=1 , ATTR_UNDERLINE=2, ATTR_BOLD=4, ATTR_GFX=8 };
enum { CURSOR_UP, CURSOR_DOWN, CURSOR_LEFT, CURSOR_RIGHT,
       CURSOR_SAVE, CURSOR_LOAD };
enum { CURSOR_DEFAULT = 0, CURSOR_HIDE = 1, CURSOR_WRAPNEXT = 2 };
enum { GLYPH_SET=1, GLYPH_DIRTY=2 };
enum { MODE_WRAP=1, MODE_INSERT=2, MODE_APPKEYPAD=4, MODE_ALTSCREEN=8,
       MODE_CRLF=16, MODE_MOUSEBTN=32, MODE_MOUSEMOTION=64, MODE_MOUSE=32|64, MODE_REVERSE=128 };
enum { ESC_START=1, ESC_CSI=2, ESC_OSC=4, ESC_TITLE=8, ESC_ALTCHARSET=16 };
enum { WIN_VISIBLE=1, WIN_REDRAW=2, WIN_FOCUSED=4 };

#undef B0
enum { B0=1, B1=2, B2=4, B3=8, B4=16, B5=32, B6=64, B7=128 };

typedef struct {
	char c[UTF_SIZ];     /* character code */
	char mode;  /* attribute flags */
	int fg;     /* foreground      */
	int bg;     /* background      */
	char state; /* state flags     */
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
	int len;               /* raw string length */
	char priv;
	int arg[ESC_ARG_SIZ];
	int narg;              /* nb of args */
	char mode;
} CSIEscape;

/* Internal representation of the screen */
typedef struct {
	int row;	/* nb row */
	int col;	/* nb col */
	Line* line;	/* screen */
	Line* alt;	/* alternate screen */
	TCursor c;	/* cursor */
	int top;	/* top    scroll limit */
	int bot;	/* bottom scroll limit */
	int mode;	/* terminal mode flags */
	int esc;	/* escape state flags */
	char title[ESC_TITLE_SIZ];
	int titlelen;
} Term;

/* Purely graphic info */
typedef struct {
	Display* dpy;
	Colormap cmap;
	Window win;
	Pixmap buf;
	XIM xim;
	XIC xic;
	int scr;
	int w;	/* window width */
	int h;	/* window height */
	int bufw; /* pixmap width  */
	int bufh; /* pixmap height */
	int ch; /* char height */
	int cw; /* char width  */
	char state; /* focus, redraw, visible */
} XWindow;

typedef struct {
	KeySym k;
	unsigned int mask;
	char s[ESC_BUF_SIZ];
} Key;

/* Drawing Context */
typedef struct {
	unsigned long col[256];
	GC gc;
	struct {
		int ascent;
		int descent;
		short lbearing;
		short rbearing;
		XFontSet set;
	} font, bfont;
} DC;

/* TODO: use better name for vars... */
typedef struct {
	int mode;
	int bx, by;
	int ex, ey;
	struct {int x, y;} b, e;
	char *clip;
	Atom xtarget;
	struct timeval tclick1;
	struct timeval tclick2;
} Selection;

#include "config.h"

static void die(const char*, ...);
static void draw(void);
static void drawregion(int, int, int, int);
static void execsh(void);
static void sigchld(int);
static void run(void);

static void csidump(void);
static void csihandle(void);
static void csiparse(void);
static void csireset(void);

static void tclearregion(int, int, int, int);
static void tcursor(int);
static void tdeletechar(int);
static void tdeleteline(int);
static void tinsertblank(int);
static void tinsertblankline(int);
static void tmoveto(int, int);
static void tnew(int, int);
static void tnewline(int);
static void tputtab(void);
static void tputc(char*);
static void treset(void);
static int tresize(int, int);
static void tscrollup(int, int);
static void tscrolldown(int, int);
static void tsetattr(int*, int);
static void tsetchar(char*);
static void tsetscroll(int, int);
static void tswapscreen(void);

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
static void xseturgency(int);
static void xsetsel(char*);
static void xresize(int, int);

static void expose(XEvent *);
static void visibility(XEvent *);
static void unmap(XEvent *);
static char* kmap(KeySym, unsigned int state);
static void kpress(XEvent *);
static void resize(XEvent *);
static void focus(XEvent *);
static void brelease(XEvent *);
static void bpress(XEvent *);
static void bmotion(XEvent *);
static void selnotify(XEvent *);
static void selrequest(XEvent *);

static void selinit(void);
static inline int selected(int, int);
static void selcopy(void);
static void selpaste();

static int utf8decode(char *, long *);
static int utf8encode(long *, char *);
static int utf8size(char *);
static int isfullutf8(char *, int);

static void (*handler[LASTEvent])(XEvent *) = {
	[KeyPress] = kpress,
	[ConfigureNotify] = resize,
	[VisibilityNotify] = visibility,
	[UnmapNotify] = unmap,
	[Expose] = expose,
	[FocusIn] = focus,
	[FocusOut] = focus,
	[MotionNotify] = bmotion,
	[ButtonPress] = bpress,
	[ButtonRelease] = brelease,
	[SelectionNotify] = selnotify,
	[SelectionRequest] = selrequest,
};

/* Globals */
static DC dc;
static XWindow xw;
static Term term;
static CSIEscape escseq;
static int cmdfd;
static pid_t pid;
static Selection sel;
static char **opt_cmd  = NULL;
static char *opt_title = NULL;
static char *opt_class = NULL;

int
utf8decode(char *s, long *u) {
	unsigned char c;
	int i, n, rtn;

	rtn = 1;
	c = *s;
	if(~c&B7) { /* 0xxxxxxx */
		*u = c;
		return rtn;
	} else if((c&(B7|B6|B5)) == (B7|B6)) { /* 110xxxxx */
		*u = c&(B4|B3|B2|B1|B0);
		n = 1;
	} else if((c&(B7|B6|B5|B4)) == (B7|B6|B5)) { /* 1110xxxx */
		*u = c&(B3|B2|B1|B0);
		n = 2;
	} else if((c&(B7|B6|B5|B4|B3)) == (B7|B6|B5|B4)) { /* 11110xxx */
		*u = c&(B2|B1|B0);
		n = 3;
	} else
		goto invalid;
	for(i=n,++s; i>0; --i,++rtn,++s) {
		c = *s;
		if((c&(B7|B6)) != B7) /* 10xxxxxx */
			goto invalid;
		*u <<= 6;
		*u |= c&(B5|B4|B3|B2|B1|B0);
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
	unsigned char *sp;
	unsigned long uc;
	int i, n;

	sp = (unsigned char*) s;
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
	unsigned char *c1, *c2, *c3;

	c1 = (unsigned char *) s;
	c2 = (unsigned char *) ++s;
	c3 = (unsigned char *) ++s;
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
	unsigned char c = *s;

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
	sel.tclick1.tv_sec = 0;
	sel.tclick1.tv_usec = 0;
	sel.mode = 0;
	sel.bx = -1;
	sel.clip = NULL;
	sel.xtarget = XInternAtom(xw.dpy, "UTF8_STRING", 0);
	if(sel.xtarget == None)
		sel.xtarget = XA_STRING;
}

static inline int
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
		sel.mode = 1;
		sel.ex = sel.bx = X2COL(e->xbutton.x);
		sel.ey = sel.by = Y2ROW(e->xbutton.y);
	}
}

void
selcopy(void) {
	char *str, *ptr;
	int x, y, sz, sl, ls = 0;

	if(sel.bx == -1)
		str = NULL;
	else {
		sz = (term.col+1) * (sel.e.y-sel.b.y+1) * UTF_SIZ;
		ptr = str = malloc(sz);
		for(y = 0; y < term.row; y++) {
			for(x = 0; x < term.col; x++)
				if(term.line[y][x].state & GLYPH_SET && (ls = selected(x, y))) {
					sl = utf8size(term.line[y][x].c);
					memcpy(ptr, term.line[y][x].c, sl);
					ptr += sl;
				}
			if(ls && y < sel.e.y)
				*ptr++ = '\n';
		}
		*ptr = 0;
	}
	xsetsel(str);
}

void
selnotify(XEvent *e) {
	unsigned long nitems;
	unsigned long ofs, rem;
	int format;
	unsigned char *data;
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
				(unsigned char *) &string, 1);
		xev.property = xsre->property;
	} else if(xsre->target == sel.xtarget) {
		XChangeProperty(xsre->display, xsre->requestor, xsre->property,
				xsre->target, 8, PropModeReplace,
				(unsigned char *) sel.clip, strlen(sel.clip));
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

	XFlush(xw.dpy);
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
	draw();
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
			drawregion(0, (starty > 0 ? starty : 0), term.col, (sel.ey < term.row ? endy+1 : term.row));
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

	DEFAULT(envshell, "sh");
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
	w.ws_xpixel = w.ws_ypixel = 0;
	if(ioctl(cmdfd, TIOCSWINSZ, &w) < 0)
		fprintf(stderr, "Couldn't set window size: %s\n", SERRNO);
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
	term.c = (TCursor){{
		.mode = ATTR_NULL,
		.fg = DefaultFG,
		.bg = DefaultBG
	}, .x = 0, .y = 0, .state = CURSOR_DEFAULT};
	
	term.top = 0, term.bot = term.row - 1;
	term.mode = MODE_WRAP;
	tclearregion(0, 0, term.col-1, term.row-1);
}

void
tnew(int col, int row) {
	/* set screen size */
	term.row = row, term.col = col;
	term.line = malloc(term.row * sizeof(Line));
	term.alt  = malloc(term.row * sizeof(Line));
	for(row = 0 ; row < term.row; row++) {
		term.line[row] = malloc(term.col * sizeof(Glyph));
		term.alt [row] = malloc(term.col * sizeof(Glyph));
	}
	/* setup screen */
	treset();
}

void
tswapscreen(void) {
	Line* tmp = term.line;
	term.line = term.alt;
	term.alt = tmp;
	term.mode ^= MODE_ALTSCREEN;
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
	}
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
	char *p = escseq.buf;

	escseq.narg = 0;
	if(*p == '?')
		escseq.priv = 1, p++;
	
	while(p < escseq.buf+escseq.len) {
		while(isdigit(*p)) {
			escseq.arg[escseq.narg] *= 10;
			escseq.arg[escseq.narg] += *p++ - '0'/*, noarg = 0 */;
		}
		if(*p == ';' && escseq.narg+1 < ESC_ARG_SIZ)
			escseq.narg++, p++;
		else {
			escseq.mode = *p;
			escseq.narg++;
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

	for(y = y1; y <= y2; y++)
		for(x = x1; x <= x2; x++)
			term.line[y][x].state = 0;
}

void
tdeletechar(int n) {
	int src = term.c.x + n;
	int dst = term.c.x;
	int size = term.col - src;

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
			term.c.attr.mode &= ~(ATTR_REVERSE | ATTR_UNDERLINE | ATTR_BOLD);
			term.c.attr.fg = DefaultFG;
			term.c.attr.bg = DefaultBG;
			break;
		case 1:
			term.c.attr.mode |= ATTR_BOLD;
			break;
		case 4:
			term.c.attr.mode |= ATTR_UNDERLINE;
			break;
		case 7:
			term.c.attr.mode |= ATTR_REVERSE;
			break;
		case 22:
			term.c.attr.mode &= ~ATTR_BOLD;
			break;
		case 24:
			term.c.attr.mode &= ~ATTR_UNDERLINE;
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
				fprintf(stderr, "erresc: gfx attr %d unknown\n", attr[i]);
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
				fprintf(stderr, "erresc: gfx attr %d unknown\n", attr[i]);
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
				term.c.attr.fg = attr[i] - 100 + 8;
			else
				fprintf(stderr, "erresc: gfx attr %d unknown\n", attr[i]), csidump();
			
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
csihandle(void) {
	switch(escseq.mode) {
	default:
	unknown:
		fprintf(stderr, "erresc: unknown csi ");
		csidump();
		/* die(""); */
		break;
	case '@': /* ICH -- Insert <n> blank char */
		DEFAULT(escseq.arg[0], 1);
		tinsertblank(escseq.arg[0]);
		break;
	case 'A': /* CUU -- Cursor <n> Up */
	case 'e':
		DEFAULT(escseq.arg[0], 1);
		tmoveto(term.c.x, term.c.y-escseq.arg[0]);
		break;
	case 'B': /* CUD -- Cursor <n> Down */
		DEFAULT(escseq.arg[0], 1);
		tmoveto(term.c.x, term.c.y+escseq.arg[0]);
		break;
	case 'C': /* CUF -- Cursor <n> Forward */
	case 'a':
		DEFAULT(escseq.arg[0], 1);
		tmoveto(term.c.x+escseq.arg[0], term.c.y);
		break;
	case 'D': /* CUB -- Cursor <n> Backward */
		DEFAULT(escseq.arg[0], 1);
		tmoveto(term.c.x-escseq.arg[0], term.c.y);
		break;
	case 'E': /* CNL -- Cursor <n> Down and first col */
		DEFAULT(escseq.arg[0], 1);
		tmoveto(0, term.c.y+escseq.arg[0]);
		break;
	case 'F': /* CPL -- Cursor <n> Up and first col */
		DEFAULT(escseq.arg[0], 1);
		tmoveto(0, term.c.y-escseq.arg[0]);
		break;
	case 'G': /* CHA -- Move to <col> */
	case '`': /* XXX: HPA -- same? */
		DEFAULT(escseq.arg[0], 1);
		tmoveto(escseq.arg[0]-1, term.c.y);
		break;
	case 'H': /* CUP -- Move to <row> <col> */
	case 'f': /* XXX: HVP -- same? */
		DEFAULT(escseq.arg[0], 1);
		DEFAULT(escseq.arg[1], 1);
		tmoveto(escseq.arg[1]-1, escseq.arg[0]-1);
		break;
	/* XXX: (CSI n I) CHT -- Cursor Forward Tabulation <n> tab stops */
	case 'J': /* ED -- Clear screen */
		switch(escseq.arg[0]) {
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
		switch(escseq.arg[0]) {
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
		DEFAULT(escseq.arg[0], 1);
		tscrollup(term.top, escseq.arg[0]);
		break;
	case 'T': /* SD -- Scroll <n> line down */
		DEFAULT(escseq.arg[0], 1);
		tscrolldown(term.top, escseq.arg[0]);
		break;
	case 'L': /* IL -- Insert <n> blank lines */
		DEFAULT(escseq.arg[0], 1);
		tinsertblankline(escseq.arg[0]);
		break;
	case 'l': /* RM -- Reset Mode */
		if(escseq.priv) {
			switch(escseq.arg[0]) {
			case 1:
				term.mode &= ~MODE_APPKEYPAD;
				break;
			case 5: /* DECSCNM -- Remove reverse video */
				if(IS_SET(MODE_REVERSE)) {
					term.mode &= ~MODE_REVERSE;
					draw();
				}
				break;
			case 7:
				term.mode &= ~MODE_WRAP;
				break;
			case 12: /* att610 -- Stop blinking cursor (IGNORED) */
				break;
			case 20:
				term.mode &= ~MODE_CRLF;
				break;
			case 25:
				term.c.state |= CURSOR_HIDE;
				break;
			case 1000: /* disable X11 xterm mouse reporting */
				term.mode &= ~MODE_MOUSEBTN;
				break;
			case 1002:
				term.mode &= ~MODE_MOUSEMOTION;
				break;
			case 1049: /* = 1047 and 1048 */
			case 47:
			case 1047:
				if(IS_SET(MODE_ALTSCREEN)) {
					tclearregion(0, 0, term.col-1, term.row-1);
					tswapscreen();
				}
				if(escseq.arg[0] != 1049)
					break;
			case 1048:
				tcursor(CURSOR_LOAD);
				break;
			default:
				goto unknown;
			}
		} else {
			switch(escseq.arg[0]) {
			case 4:
				term.mode &= ~MODE_INSERT;
				break;
			default:
				goto unknown;
			}
		}
		break;
	case 'M': /* DL -- Delete <n> lines */
		DEFAULT(escseq.arg[0], 1);
		tdeleteline(escseq.arg[0]);
		break;
	case 'X': /* ECH -- Erase <n> char */
		DEFAULT(escseq.arg[0], 1);
		tclearregion(term.c.x, term.c.y, term.c.x + escseq.arg[0], term.c.y);
		break;
	case 'P': /* DCH -- Delete <n> char */
		DEFAULT(escseq.arg[0], 1);
		tdeletechar(escseq.arg[0]);
		break;
	/* XXX: (CSI n Z) CBT -- Cursor Backward Tabulation <n> tab stops */
	case 'd': /* VPA -- Move to <row> */
		DEFAULT(escseq.arg[0], 1);
		tmoveto(term.c.x, escseq.arg[0]-1);
		break;
	case 'h': /* SM -- Set terminal mode */
		if(escseq.priv) {
			switch(escseq.arg[0]) {
			case 1:
				term.mode |= MODE_APPKEYPAD;
				break;
			case 5: /* DECSCNM -- Reverve video */
				if(!IS_SET(MODE_REVERSE)) {
					term.mode |= MODE_REVERSE;
					draw();
				}
				break;
			case 7:
				term.mode |= MODE_WRAP;
				break;
			case 20:
				term.mode |= MODE_CRLF;
				break;
			case 12: /* att610 -- Start blinking cursor (IGNORED) */
				 /* fallthrough for xterm cvvis = CSI [ ? 12 ; 25 h */
				if(escseq.narg > 1 && escseq.arg[1] != 25)
					break;
			case 25:
				term.c.state &= ~CURSOR_HIDE;
				break;
			case 1000: /* 1000,1002: enable xterm mouse report */
				term.mode |= MODE_MOUSEBTN;
				break;
			case 1002:
				term.mode |= MODE_MOUSEMOTION;
				break;
			case 1049: /* = 1047 and 1048 */
			case 47:
			case 1047:
				if(IS_SET(MODE_ALTSCREEN))
					tclearregion(0, 0, term.col-1, term.row-1);
				else
					tswapscreen();
				if(escseq.arg[0] != 1049)
					break;
			case 1048:
				tcursor(CURSOR_SAVE);
				break;
			default: goto unknown;
			}
		} else {
			switch(escseq.arg[0]) {
			case 4:
				term.mode |= MODE_INSERT;
				break;
			default: goto unknown;
			}
		};
		break;
	case 'm': /* SGR -- Terminal attribute (color) */
		tsetattr(escseq.arg, escseq.narg);
		break;
	case 'r': /* DECSTBM -- Set Scrolling Region */
		if(escseq.priv)
			goto unknown;
		else {
			DEFAULT(escseq.arg[0], 1);
			DEFAULT(escseq.arg[1], term.row);
			tsetscroll(escseq.arg[0]-1, escseq.arg[1]-1);
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
	printf("ESC [ %s", escseq.priv ? "? " : "");
	if(escseq.narg)
		for(i = 0; i < escseq.narg; i++)
			printf("%d ", escseq.arg[i]);
	if(escseq.mode)
		putchar(escseq.mode);
	putchar('\n');
}

void
csireset(void) {
	memset(&escseq, 0, sizeof(escseq));
}

void
tputtab(void) {
	int space = TAB - term.c.x % TAB;
	tmoveto(term.c.x + space, term.c.y);
}

void
tputc(char *c) {
	char ascii = *c;
	if(term.esc & ESC_START) {
		if(term.esc & ESC_CSI) {
			escseq.buf[escseq.len++] = ascii;
			if(BETWEEN(ascii, 0x40, 0x7E) || escseq.len >= ESC_BUF_SIZ) {
				term.esc = 0;
				csiparse(), csihandle();
			}
			/* TODO: handle other OSC */
		} else if(term.esc & ESC_OSC) {
			if(ascii == ';') {
				term.titlelen = 0;
				term.esc = ESC_START | ESC_TITLE;
			}
		} else if(term.esc & ESC_TITLE) {
			if(ascii == '\a' || term.titlelen+1 >= ESC_TITLE_SIZ) {
				term.esc = 0;
				term.title[term.titlelen] = '\0';
				XStoreName(xw.dpy, xw.win, term.title);
			} else {
				term.title[term.titlelen++] = ascii;
			}
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
			case ']':
				term.esc |= ESC_OSC;
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
			default:
				fprintf(stderr, "erresc: unknown sequence ESC 0x%02X '%c'\n",
				    (unsigned char) ascii, isprint(ascii)?ascii:'.');
				term.esc = 0;
			}
		}
	} else {
		switch(ascii) {
		case '\t':
			tputtab();
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
	term.line = realloc(term.line, row * sizeof(Line));
	term.alt  = realloc(term.alt,  row * sizeof(Line));

	/* resize each row to new width, zero-pad if needed */
	for(i = 0; i < minrow; i++) {
		term.line[i] = realloc(term.line[i], col * sizeof(Glyph));
		term.alt[i]  = realloc(term.alt[i],  col * sizeof(Glyph));
		for(x = mincol; x < col; x++) {
			term.line[i][x].state = 0;
			term.alt[i][x].state = 0;
		}
	}

	/* allocate any new rows */
	for(/* i == minrow */; i < row; i++) {
		term.line[i] = calloc(col, sizeof(Glyph));
		term.alt [i] = calloc(col, sizeof(Glyph));
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
	Pixmap newbuf;
	int oldw, oldh;

	oldw = xw.bufw;
	oldh = xw.bufh;
	xw.bufw = MAX(1, col * xw.cw);
	xw.bufh = MAX(1, row * xw.ch);
	newbuf = XCreatePixmap(xw.dpy, xw.win, xw.bufw, xw.bufh, XDefaultDepth(xw.dpy, xw.scr));
	XCopyArea(xw.dpy, xw.buf, newbuf, dc.gc, 0, 0, xw.bufw, xw.bufh, 0, 0);
	XFreePixmap(xw.dpy, xw.buf);
	XSetForeground(xw.dpy, dc.gc, dc.col[DefaultBG]);
	if(xw.bufw > oldw)
		XFillRectangle(xw.dpy, newbuf, dc.gc, oldw, 0,
				xw.bufw-oldw, MIN(xw.bufh, oldh));
	else if(xw.bufw < oldw && (BORDER > 0 || xw.w > xw.bufw))
		XClearArea(xw.dpy, xw.win, BORDER+xw.bufw, BORDER,
				xw.w-xw.bufh-BORDER, BORDER+MIN(xw.bufh, oldh),
				False);
	if(xw.bufh > oldh)
		XFillRectangle(xw.dpy, newbuf, dc.gc, 0, oldh,
				xw.bufw, xw.bufh-oldh);
	else if(xw.bufh < oldh && (BORDER > 0 || xw.h > xw.bufh))
		XClearArea(xw.dpy, xw.win, BORDER, BORDER+xw.bufh,
				xw.w-2*BORDER, xw.h-xw.bufh-BORDER,
				False);
	xw.buf = newbuf;
}

void
xloadcols(void) {
	int i, r, g, b;
	XColor color;
	unsigned long white = WhitePixel(xw.dpy, xw.scr);

	for(i = 0; i < 16; i++) {
		if(!XAllocNamedColor(xw.dpy, xw.cmap, colorname[i], &color, &color)) {
			dc.col[i] = white;
			fprintf(stderr, "Could not allocate color '%s'\n", colorname[i]);
		} else
			dc.col[i] = color.pixel;
	}

	/* same colors as xterm */
	for(r = 0; r < 6; r++)
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
		if (!XAllocColor(xw.dpy, xw.cmap, &color)) {
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
	               x1 * xw.cw, y1 * xw.ch,
	               (x2-x1+1) * xw.cw, (y2-y1+1) * xw.ch);
}

void
xhints(void) {
	XClassHint class = {opt_class ? opt_class : TNAME, TNAME};
	XWMHints wm = {.flags = InputHint, .input = 1};
	XSizeHints size = {
		.flags = PSize | PResizeInc | PBaseSize,
		.height = xw.h,
		.width = xw.w,
		.height_inc = xw.ch,
		.width_inc = xw.cw,
		.base_height = 2*BORDER,
		.base_width = 2*BORDER,
	};
	XSetWMProperties(xw.dpy, xw.win, NULL, NULL, NULL, 0, &size, &wm, &class);
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
initfonts(char *fontstr, char *bfontstr) {
	if((dc.font.set = xinitfont(fontstr)) == NULL ||
	   (dc.bfont.set = xinitfont(bfontstr)) == NULL)
		die("Can't load font %s\n", dc.font.set ? BOLDFONT : FONT);
	xgetfontinfo(dc.font.set, &dc.font.ascent, &dc.font.descent,
	    &dc.font.lbearing, &dc.font.rbearing);
	xgetfontinfo(dc.bfont.set, &dc.bfont.ascent, &dc.bfont.descent,
	    &dc.bfont.lbearing, &dc.bfont.rbearing);
}

void
xinit(void) {
	XSetWindowAttributes attrs;
	Cursor cursor;

	if(!(xw.dpy = XOpenDisplay(NULL)))
		die("Can't open display\n");
	xw.scr = XDefaultScreen(xw.dpy);
	
	/* font */
	initfonts(FONT, BOLDFONT);

	/* XXX: Assuming same size for bold font */
	xw.cw = dc.font.rbearing - dc.font.lbearing;
	xw.ch = dc.font.ascent + dc.font.descent;

	/* colors */
	xw.cmap = XDefaultColormap(xw.dpy, xw.scr);
	xloadcols();

	/* window - default size */
	xw.bufh = 24 * xw.ch;
	xw.bufw = 80 * xw.cw;
	xw.h = xw.bufh + 2*BORDER;
	xw.w = xw.bufw + 2*BORDER;

	attrs.background_pixel = dc.col[DefaultBG];
	attrs.border_pixel = dc.col[DefaultBG];
	attrs.bit_gravity = NorthWestGravity;
	attrs.event_mask = FocusChangeMask | KeyPressMask
		| ExposureMask | VisibilityChangeMask | StructureNotifyMask
		| ButtonMotionMask | ButtonPressMask | ButtonReleaseMask;
	attrs.colormap = xw.cmap;

	xw.win = XCreateWindow(xw.dpy, XRootWindow(xw.dpy, xw.scr), 0, 0,
			xw.w, xw.h, 0, XDefaultDepth(xw.dpy, xw.scr), InputOutput,
			XDefaultVisual(xw.dpy, xw.scr),
			CWBackPixel | CWBorderPixel | CWBitGravity | CWEventMask
			| CWColormap,
			&attrs);
	xw.buf = XCreatePixmap(xw.dpy, xw.win, xw.bufw, xw.bufh, XDefaultDepth(xw.dpy, xw.scr));


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

	XStoreName(xw.dpy, xw.win, opt_title ? opt_title : "st");
	XMapWindow(xw.dpy, xw.win);
	xhints();
	XSync(xw.dpy, 0);
}

void
xdraws(char *s, Glyph base, int x, int y, int charlen, int bytelen) {
	unsigned long xfg = dc.col[base.fg], xbg = dc.col[base.bg], temp;
	int winx = x*xw.cw, winy = y*xw.ch + dc.font.ascent, width = charlen*xw.cw;
	int i;
	
	/* only switch default fg/bg if term is in RV mode */
	if(IS_SET(MODE_REVERSE)) {
		if(base.fg == DefaultFG)
			xfg = dc.col[DefaultBG];
		if(base.bg == DefaultBG)
			xbg = dc.col[DefaultFG];
	}

	if(base.mode & ATTR_REVERSE)
		temp = xfg, xfg = xbg, xbg = temp;

	XSetBackground(xw.dpy, dc.gc, xbg);
	XSetForeground(xw.dpy, dc.gc, xfg);

	if(base.mode & ATTR_GFX) {
		for(i = 0; i < bytelen; i++) {
			char c = gfx[(unsigned int)s[i] % 256];
			if(c)
				s[i] = c;
			else if(s[i] > 0x5f)
				s[i] -= 0x5f;
		}
	}

	XmbDrawImageString(xw.dpy, xw.buf, base.mode & ATTR_BOLD ? dc.bfont.set : dc.font.set,
		dc.gc, winx, winy, s, bytelen);
	
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
	if(!(term.c.state & CURSOR_HIDE) && (xw.state & WIN_FOCUSED)) {
		sl = utf8size(g.c);
		if(IS_SET(MODE_REVERSE))
			g.mode |= ATTR_REVERSE, g.fg = DefaultCS, g.bg = DefaultFG;
		xdraws(g.c, g, term.c.x, term.c.y, 1, sl);
		oldx = term.c.x, oldy = term.c.y;
	}
}

void
draw() {
	drawregion(0, 0, term.col, term.row);
}

void
drawregion(int x1, int y1, int x2, int y2) {
	int ic, ib, x, y, ox, sl;
	Glyph base, new;
	char buf[DRAW_BUF_SIZ];

	if(!(xw.state & WIN_VISIBLE))
		return;

	xclear(x1, y1, x2-1, y2-1);
	for(y = y1; y < y2; y++) {
		base = term.line[y][0];
		ic = ib = ox = 0;
		for(x = x1; x < x2; x++) {
			new = term.line[y][x];
			if(sel.bx != -1 && *(new.c) && selected(x, y))
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
	XCopyArea(xw.dpy, xw.buf, xw.win, dc.gc, 0, 0, xw.bufw, xw.bufh, BORDER, BORDER);
}

void
expose(XEvent *ev) {
	XExposeEvent *e = &ev->xexpose;
	if(xw.state & WIN_REDRAW) {
		if(!e->count) {
			xw.state &= ~WIN_REDRAW;
			draw();
		}
	} else
		XCopyArea(xw.dpy, xw.buf, xw.win, dc.gc, e->x-BORDER, e->y-BORDER,
				e->width, e->height, e->x, e->y);
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
	draw();
}

char*
kmap(KeySym k, unsigned int state) {
	int i;
	for(i = 0; i < LEN(key); i++)
		if(key[i].k == k && (key[i].mask == 0 || key[i].mask & state))
			return (char*)key[i].s;
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
	ttyresize(col, row);
	xresize(col, row);
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
		if(FD_ISSET(cmdfd, &rfd)) {
			ttyread();
			draw();
		}
		while(XPending(xw.dpy)) {
			XNextEvent(xw.dpy, &ev);
			if(XFilterEvent(&ev, xw.win))
				continue;
			if(handler[ev.type])
				(handler[ev.type])(&ev);
		}
	}
}

int
main(int argc, char *argv[]) {
	int i;
	
	for(i = 1; i < argc; i++) {
		switch(argv[i][0] != '-' || argv[i][2] ? -1 : argv[i][1]) {
		case 't':
			if(++i < argc) opt_title = argv[i];
			break;
		case 'c':
			if(++i < argc) opt_class = argv[i];
			break;
		case 'e': 
			/* eat every remaining arguments */
			if(++i < argc) opt_cmd = &argv[i];
			goto run;
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
