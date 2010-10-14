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
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>

#if   defined(__linux)
 #include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__)
 #include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
 #include <libutil.h>
#endif

#define USAGE \
	"st-" VERSION ", (c) 2010 st engineers\n" \
	"usage: st [-t title] [-e cmd] [-v]\n"

/* Arbitrary sizes */
#define ESC_TITLE_SIZ 256
#define ESC_BUF_SIZ   256
#define ESC_ARG_SIZ   16
#define DRAW_BUF_SIZ  1024

#define SERRNO strerror(errno)
#define MIN(a, b)  ((a) < (b) ? (a) : (b))
#define MAX(a, b)  ((a) < (b) ? (b) : (a))
#define LEN(a)     (sizeof(a) / sizeof(a[0]))
#define DEFAULT(a, b)     (a) = (a) ? (a) : (b)    
#define BETWEEN(x, a, b)  ((a) <= (x) && (x) <= (b))
#define LIMIT(x, a, b)    (x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x)
#define ATTRCMP(a, b) ((a).mode != (b).mode || (a).fg != (b).fg || (a).bg != (b).bg)
#define IS_SET(flag) (term.mode & (flag))

/* Attribute, Cursor, Character state, Terminal mode, Screen draw mode */
enum { ATTR_NULL=0 , ATTR_REVERSE=1 , ATTR_UNDERLINE=2, ATTR_BOLD=4, ATTR_GFX=8 };
enum { CURSOR_UP, CURSOR_DOWN, CURSOR_LEFT, CURSOR_RIGHT,
       CURSOR_SAVE, CURSOR_LOAD };
enum { CURSOR_DEFAULT = 0, CURSOR_HIDE = 1, CURSOR_WRAPNEXT = 2 };
enum { GLYPH_SET=1, GLYPH_DIRTY=2 };
enum { MODE_WRAP=1, MODE_INSERT=2, MODE_APPKEYPAD=4, MODE_ALTSCREEN=8 };
enum { ESC_START=1, ESC_CSI=2, ESC_OSC=4, ESC_TITLE=8, ESC_ALTCHARSET=16 };
enum { SCREEN_UPDATE, SCREEN_REDRAW };

typedef struct {
	char c;     /* character code  */
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
	int len;			   /* raw string length */
	char priv;
	int arg[ESC_ARG_SIZ];
	int narg;			   /* nb of args */
	char mode;
} CSIEscape;

/* Internal representation of the screen */
typedef struct {
	int row;	/* nb row */  
	int col;	/* nb col */
	Line* line;	/* screen */
	Line* alt;	/* alternate screen */
	TCursor c;	/* cursor */
	int top;	/* top	  scroll limit */
	int bot;	/* bottom scroll limit */
	int mode;	/* terminal mode flags */
	int esc;	/* escape state flags */
	char title[ESC_TITLE_SIZ];
	int titlelen;
} Term;

/* Purely graphic info */
typedef struct {
	Display* dis;
	Colormap cmap;
	Window win;
	Pixmap buf;
	XIM xim;
	XIC xic;
	int scr;
	int w;	/* window width	 */
	int h;	/* window height */
	int bufw; /* pixmap width  */
	int bufh; /* pixmap height */
	int ch; /* char height */
	int cw; /* char width  */
	int focus;
	int vis; /* is visible */
} XWindow; 

typedef struct {
	KeySym k;
	char s[ESC_BUF_SIZ];
} Key;

/* Drawing Context */
typedef struct {
	unsigned long col[256];
	XFontStruct* font;
	XFontStruct* bfont;
	GC gc;
} DC;

/* TODO: use better name for vars... */
typedef struct {
	int mode;
	int bx, by;
	int ex, ey;
	struct {int x, y;}  b, e;
	char *clip;
} Selection;

#include "config.h"

static void die(const char *errstr, ...);
static void draw(int);
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
static void tnewline(void);
static void tputtab(void);
static void tputc(char);
static void tputs(char*, int);
static void treset(void);
static void tresize(int, int);
static void tscrollup(int, int);
static void tscrolldown(int, int);
static void tsetattr(int*, int);
static void tsetchar(char);
static void tsetscroll(int, int);
static void tswapscreen(void);

static void ttynew(void);
static void ttyread(void);
static void ttyresize(int, int);
static void ttywrite(const char *, size_t);

static void xdraws(char *, Glyph, int, int, int);
static void xhints(void);
static void xclear(int, int, int, int);
static void xdrawcursor(void);
static void xinit(void);
static void xloadcols(void);
static void xseturgency(int);

static void expose(XEvent *);
static void visibility(XEvent *);
static void unmap(XEvent *);
static char* kmap(KeySym);
static void kpress(XEvent *);
static void resize(XEvent *);
static void focus(XEvent *);
static void brelease(XEvent *);
static void bpress(XEvent *);
static void bmotion(XEvent *);
static void selection_notify(XEvent *);
static void selection_request(XEvent *);

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
	[SelectionNotify] = selection_notify,
	[SelectionRequest] = selection_request,
};

/* Globals */
static DC dc;
static XWindow xw;
static Term term;
static CSIEscape escseq;
static int cmdfd;
static pid_t pid;
static Selection sel;
static char *opt_cmd   = NULL;
static char *opt_title = NULL;

void
selinit(void) {
	sel.mode = 0;
	sel.bx = -1;
	sel.clip = NULL;
}

static inline int selected(int x, int y) {
	if(sel.ey == y && sel.by == y) {
		int bx = MIN(sel.bx, sel.ex);
		int ex = MAX(sel.bx, sel.ex);
		return BETWEEN(x, bx, ex);
	}
	return ((sel.b.y < y&&y < sel.e.y) || (y==sel.e.y && x<=sel.e.x)) 
		|| (y==sel.b.y && x>=sel.b.x && (x<=sel.e.x || sel.b.y!=sel.e.y));
}

static void getbuttoninfo(XEvent *e, int *b, int *x, int *y) {
	if(b) 
		*b = e->xbutton.button;

	*x = e->xbutton.x/xw.cw;
	*y = e->xbutton.y/xw.ch;
	sel.b.x = sel.by < sel.ey ? sel.bx : sel.ex;
	sel.b.y = MIN(sel.by, sel.ey);
	sel.e.x = sel.by < sel.ey ? sel.ex : sel.bx;
	sel.e.y = MAX(sel.by, sel.ey);
}

static void bpress(XEvent *e) {
	sel.mode = 1;
	sel.ex = sel.bx = e->xbutton.x/xw.cw;
	sel.ey = sel.by = e->xbutton.y/xw.ch;
}

static char *getseltext() {
	char *str, *ptr;
	int ls, x, y, sz;
	if(sel.bx == -1)
		return NULL;
	sz = (term.col+1) * (sel.e.y-sel.b.y+1);
	ptr = str = malloc(sz);
	for(y = 0; y < term.row; y++) {
		for(x = 0; x < term.col; x++)
			if(term.line[y][x].state & GLYPH_SET && (ls = selected(x, y)))
				*ptr = term.line[y][x].c, ptr++;
		if(ls)
			*ptr = '\n', ptr++;
	}
	*ptr = 0;
	return str;
}

static void selection_notify(XEvent *e) {
	unsigned long nitems;
	unsigned long length;
	int format, res;
	unsigned char *data;
	Atom type;

	res = XGetWindowProperty(xw.dis, xw.win, XA_PRIMARY, 0, 0, False, 
				AnyPropertyType, &type, &format, &nitems, &length, &data);
	switch(res) {
		case BadAtom:
		case BadValue:
		case BadWindow:
			fprintf(stderr, "Invalid paste, XGetWindowProperty0");
			return;
	}

	res = XGetWindowProperty(xw.dis, xw.win, XA_PRIMARY, 0, length, False,
				AnyPropertyType, &type, &format, &nitems, &length, &data);
	switch(res) {
		case BadAtom:
		case BadValue:
		case BadWindow:
			fprintf(stderr, "Invalid paste, XGetWindowProperty0");
			return;
	}

	if(data) {
		ttywrite((const char *) data, nitems * format / 8);
		XFree(data);
	}
}

static void selpaste() {
	XConvertSelection(xw.dis, XA_PRIMARY, XA_STRING, XA_PRIMARY, xw.win, CurrentTime);
}

static void selection_request(XEvent *e)
{
	XSelectionRequestEvent *xsre;
	XSelectionEvent xev;
	int res;
	Atom xa_targets;

	xsre = (XSelectionRequestEvent *) e;
	xev.type = SelectionNotify;
	xev.requestor = xsre->requestor;
	xev.selection = xsre->selection;
	xev.target = xsre->target;
	xev.time = xsre->time;
	/* reject */
	xev.property = None;

	xa_targets = XInternAtom(xw.dis, "TARGETS", 0);
	if(xsre->target == xa_targets) {
		/* respond with the supported type */
		Atom string = XA_STRING;
		res = XChangeProperty(xsre->display, xsre->requestor, xsre->property, XA_ATOM, 32,
				PropModeReplace, (unsigned char *) &string, 1);
		switch(res) {
			case BadAlloc:
			case BadAtom:
			case BadMatch:
			case BadValue:
			case BadWindow:
				fprintf(stderr, "Error in selection_request, TARGETS");
				break;
			default:
				xev.property = xsre->property;
		}
	} else if(xsre->target == XA_STRING) {
		res = XChangeProperty(xsre->display, xsre->requestor, xsre->property,
				xsre->target, 8, PropModeReplace, (unsigned char *) sel.clip,
				strlen(sel.clip));
		switch(res) {
			case BadAlloc:
			case BadAtom:
			case BadMatch:
			case BadValue:
			case BadWindow:
				fprintf(stderr, "Error in selection_request, XA_STRING");
				break;
			default:
			 xev.property = xsre->property;
		}
	}

	/* all done, send a notification to the listener */
	res = XSendEvent(xsre->display, xsre->requestor, True, 0, (XEvent *) &xev);
	switch(res) {
		case 0:
		case BadValue:
		case BadWindow:
			fprintf(stderr, "Error in selection_requested, XSendEvent");
	}
}

static void selcopy(char *str) {
	/* register the selection for both the clipboard and the primary */
	Atom clipboard;
	int res;

	free(sel.clip);
	sel.clip = str;

	res = XSetSelectionOwner(xw.dis, XA_PRIMARY, xw.win, CurrentTime);
	switch(res) {
		case BadAtom:
		case BadWindow:
			fprintf(stderr, "Invalid copy, XSetSelectionOwner");
			return;
	}

	clipboard = XInternAtom(xw.dis, "CLIPBOARD", 0);
	res = XSetSelectionOwner(xw.dis, clipboard, xw.win, CurrentTime);
	switch(res) {
		case BadAtom:
		case BadWindow:
			fprintf(stderr, "Invalid copy, XSetSelectionOwner");
			return;
	}

	XFlush(xw.dis);
}

/* TODO: doubleclick to select word */
static void brelease(XEvent *e) {
	int b;
	sel.mode = 0;
	getbuttoninfo(e, &b, &sel.ex, &sel.ey);
	if(sel.bx==sel.ex && sel.by==sel.ey) {
		sel.bx = -1;
		if(b==2)
			selpaste();
	} else {
		if(b==1)
			selcopy(getseltext());
	}
	draw(1);
}

static void bmotion(XEvent *e) {
	if (sel.mode) {
		getbuttoninfo(e, NULL, &sel.ex, &sel.ey);
		draw(1);
	}
}

#ifdef DEBUG
void
tdump(void) {
	int row, col;
	Glyph c;

	for(row = 0; row < term.row; row++) {
		for(col = 0; col < term.col; col++) {
			if(col == term.c.x && row == term.c.y)
				putchar('#');
			else {
				c = term.line[row][col];
				putchar(c.state & GLYPH_SET ? c.c : '.');
			}
		}
		putchar('\n');
	}
}
#endif

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
	char *args[] = {getenv("SHELL"), "-i", NULL};
	if(opt_cmd)
		args[0] = opt_cmd, args[1] = NULL;
	else
		DEFAULT(args[0], SHELL);
	putenv("TERM="TNAME);
	execvp(args[0], args);
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
	char buf[BUFSIZ];
	int ret;

	if((ret = read(cmdfd, buf, LEN(buf))) < 0)
		die("Couldn't read from shell: %s\n", SERRNO);
	else
		tputs(buf, ret);
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
tnewline(void) {
	int x = term.c.x+1 < term.col ? term.c.x : 0;
	int y = term.c.y;
	if(term.c.y == term.bot)
		tscrollup(term.top, 1);
	else
		y++;
	tmoveto(x, y);
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
tsetchar(char c) {
	term.line[term.c.y][term.c.x] = term.c.attr;
	term.line[term.c.y][term.c.x].c = c;
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
			if (i + 2 < l && attr[i + 1] == 5) {
				i += 2;
				if (BETWEEN(attr[i], 0, 255))
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
			if (i + 2 < l && attr[i + 1] == 5) {
				i += 2;
				if (BETWEEN(attr[i], 0, 255))
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
		printf("erresc: unknown csi ");
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
			tclearregion(term.c.x, term.c.y, term.col-1, term.row-1);
			break;
		case 1: /* above */
			tclearregion(0, 0, term.c.x, term.c.y);
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
			case 5: /* TODO: DECSCNM -- Remove reverse video */
				break;
			case 7:
				term.mode &= ~MODE_WRAP;
				break;
			case 12: /* att610 -- Stop blinking cursor (IGNORED) */
				break;
			case 25:
				term.c.state |= CURSOR_HIDE;
				break;
			case 1049: /* = 1047 and 1048 */
			case 1047:
				if(IS_SET(MODE_ALTSCREEN)) {
					tclearregion(0, 0, term.col-1, term.row-1);
					tswapscreen();
				}
				if(escseq.arg[0] == 1047)
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
				/* TODO: set REVERSE on the whole screen (f) */
				break;
			case 7:
				term.mode |= MODE_WRAP;
				break;
			case 12: /* att610 -- Start blinking cursor (IGNORED) */
				 /* fallthrough for xterm cvvis = CSI [ ? 12 ; 25 h */
				if(escseq.narg > 1 && escseq.arg[1] != 25)
					break;
			case 25:
				term.c.state &= ~CURSOR_HIDE;
				break;
			case 1049: /* = 1047 and 1048 */
			case 1047:
				if(IS_SET(MODE_ALTSCREEN))
					tclearregion(0, 0, term.col-1, term.row-1);
				else
					tswapscreen();
				if(escseq.arg[0] == 1047)
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
tputc(char c) {
	if(term.esc & ESC_START) {
		if(term.esc & ESC_CSI) {
			escseq.buf[escseq.len++] = c;
			if(BETWEEN(c, 0x40, 0x7E) || escseq.len >= ESC_BUF_SIZ) {
				term.esc = 0;
				csiparse(), csihandle();
			}
			/* TODO: handle other OSC */
		} else if(term.esc & ESC_OSC) { 
			if(c == ';') {
				term.titlelen = 0;
				term.esc = ESC_START | ESC_TITLE;
			}
		} else if(term.esc & ESC_TITLE) {
			if(c == '\a' || term.titlelen+1 >= ESC_TITLE_SIZ) {
				term.esc = 0;
				term.title[term.titlelen] = '\0';
				XStoreName(xw.dis, xw.win, term.title);
			} else {
				term.title[term.titlelen++] = c;
			}
		} else if(term.esc & ESC_ALTCHARSET) {
			switch(c) {
			case '0': /* Line drawing crap */
				term.c.attr.mode |= ATTR_GFX;
				break;
			case 'B': /* Back to regular text */
				term.c.attr.mode &= ~ATTR_GFX;
				break;
			default:
				printf("esc unhandled charset: ESC ( %c\n", c);
			}
			term.esc = 0;
		} else {
			switch(c) {
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
				tnewline();
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
				fprintf(stderr, "erresc: unknown sequence ESC 0x%02X '%c'\n", c, isprint(c)?c:'.');
				term.esc = 0;
			}
		}
	} else {
		switch(c) {
		case '\t':
			tputtab();
			break;
		case '\b':
			tmoveto(term.c.x-1, term.c.y);
			break;
		case '\r':
			tmoveto(0, term.c.y);
			break;
		case '\n':
			tnewline();
			break;
		case '\a':
			if(!xw.focus)
				xseturgency(1);
			break;
		case '\033':
			csireset();
			term.esc = ESC_START;
			break;
		default:
			if(IS_SET(MODE_WRAP) && term.c.state & CURSOR_WRAPNEXT)
				tnewline();
			tsetchar(c);
			if(term.c.x+1 < term.col)
				tmoveto(term.c.x+1, term.c.y);
			else
				term.c.state |= CURSOR_WRAPNEXT;
			break;
		}
	}
}

void
tputs(char *s, int len) {
	for(; len > 0; len--)
		tputc(*s++);
}

void
tresize(int col, int row) {
	int i, x;
	int minrow = MIN(row, term.row);
	int mincol = MIN(col, term.col);
	int slide = term.c.y - row + 1;

	if(col < 1 || row < 1)
		return;

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
}

void
xloadcols(void) {
	int i, r, g, b;
	XColor color;
	unsigned long white = WhitePixel(xw.dis, xw.scr);

	for(i = 0; i < 16; i++) {
		if (!XAllocNamedColor(xw.dis, xw.cmap, colorname[i], &color, &color)) {
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
				if (!XAllocColor(xw.dis, xw.cmap, &color)) {
					dc.col[i] = white;
					fprintf(stderr, "Could not allocate color %d\n", i);
				} else
					dc.col[i] = color.pixel;
				i++;
			}

	for(r = 0; r < 24; r++, i++) {
		color.red = color.green = color.blue = 0x0808 + 0x0a0a * r;
		if (!XAllocColor(xw.dis, xw.cmap, &color)) {
			dc.col[i] = white;
			fprintf(stderr, "Could not allocate color %d\n", i);
		} else
			dc.col[i] = color.pixel;
	}
}

void
xclear(int x1, int y1, int x2, int y2) {
	XSetForeground(xw.dis, dc.gc, dc.col[DefaultBG]);
	XFillRectangle(xw.dis, xw.buf, dc.gc,
	               x1 * xw.cw, y1 * xw.ch,
	               (x2-x1+1) * xw.cw, (y2-y1+1) * xw.ch);
}

void
xhints(void)
{
	XClassHint class = {TNAME, TNAME};
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
	XSetWMProperties(xw.dis, xw.win, NULL, NULL, NULL, 0, &size, &wm, &class);
}

void
xinit(void) {
	XSetWindowAttributes attrs;

	if(!(xw.dis = XOpenDisplay(NULL)))
		die("Can't open display\n");
	xw.scr = XDefaultScreen(xw.dis);
	
	/* font */
	if(!(dc.font = XLoadQueryFont(xw.dis, FONT)) || !(dc.bfont = XLoadQueryFont(xw.dis, BOLDFONT)))
		die("Can't load font %s\n", dc.font ? BOLDFONT : FONT);

	/* XXX: Assuming same size for bold font */
	xw.cw = dc.font->max_bounds.rbearing - dc.font->min_bounds.lbearing;
	xw.ch = dc.font->ascent + dc.font->descent;

	/* colors */
	xw.cmap = XDefaultColormap(xw.dis, xw.scr);
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
		| PointerMotionMask | ButtonPressMask | ButtonReleaseMask;
	attrs.colormap = xw.cmap;

	xw.win = XCreateWindow(xw.dis, XRootWindow(xw.dis, xw.scr), 0, 0,
			xw.w, xw.h, 0, XDefaultDepth(xw.dis, xw.scr), InputOutput,
			XDefaultVisual(xw.dis, xw.scr),
			CWBackPixel | CWBorderPixel | CWBitGravity | CWEventMask
			| CWColormap,
			&attrs);
	xw.buf = XCreatePixmap(xw.dis, xw.win, xw.bufw, xw.bufh, XDefaultDepth(xw.dis, xw.scr));


	/* input methods */
	xw.xim = XOpenIM(xw.dis, NULL, NULL, NULL);
	xw.xic = XCreateIC(xw.xim, XNInputStyle, XIMPreeditNothing 
					   | XIMStatusNothing, XNClientWindow, xw.win, 
					   XNFocusWindow, xw.win, NULL);
	/* gc */
	dc.gc = XCreateGC(xw.dis, xw.win, 0, NULL);
	
	XMapWindow(xw.dis, xw.win);
	xhints();
	XStoreName(xw.dis, xw.win, opt_title ? opt_title : "st");
	XSync(xw.dis, 0);
}

void
xdraws(char *s, Glyph base, int x, int y, int len) {
	unsigned long xfg, xbg;
	int winx = x*xw.cw, winy = y*xw.ch + dc.font->ascent, width = len*xw.cw;
	int i;

	if(base.mode & ATTR_REVERSE)
		xfg = dc.col[base.bg], xbg = dc.col[base.fg];
	else
		xfg = dc.col[base.fg], xbg = dc.col[base.bg];

	XSetBackground(xw.dis, dc.gc, xbg);
	XSetForeground(xw.dis, dc.gc, xfg);
	
	if(base.mode & ATTR_GFX)
		for(i = 0; i < len; i++) {
			char c = gfx[(unsigned int)s[i] % 256];
			if(c)
				s[i] = c;
			else if(s[i] > 0x5f)
				s[i] -= 0x5f;
		}

	XSetFont(xw.dis, dc.gc, base.mode & ATTR_BOLD ? dc.bfont->fid : dc.font->fid);
	XDrawImageString(xw.dis, xw.buf, dc.gc, winx, winy, s, len);
	
	if(base.mode & ATTR_UNDERLINE)
		XDrawLine(xw.dis, xw.buf, dc.gc, winx, winy+1, winx+width-1, winy+1);
}

void
xdrawcursor(void) {
	static int oldx = 0;
	static int oldy = 0;
	Glyph g = {' ', ATTR_NULL, DefaultBG, DefaultCS, 0};
	
	LIMIT(oldx, 0, term.col-1);
	LIMIT(oldy, 0, term.row-1);
	
	if(term.line[term.c.y][term.c.x].state & GLYPH_SET)
		g.c = term.line[term.c.y][term.c.x].c;
	
	/* remove the old cursor */
	if(term.line[oldy][oldx].state & GLYPH_SET)
		xdraws(&term.line[oldy][oldx].c, term.line[oldy][oldx], oldx, oldy, 1);
	else
		xclear(oldx, oldy, oldx, oldy);
	
	/* draw the new one */
	if(!(term.c.state & CURSOR_HIDE) && xw.focus) {
		xdraws(&g.c, g, term.c.x, term.c.y, 1);
		oldx = term.c.x, oldy = term.c.y;
	}
}

#ifdef DEBUG
/* basic drawing routines */
void
xdrawc(int x, int y, Glyph g) {
	XRectangle r = { x * xw.cw, y * xw.ch, xw.cw, xw.ch };
	XSetBackground(xw.dis, dc.gc, dc.col[g.bg]);
	XSetForeground(xw.dis, dc.gc, dc.col[g.fg]);
	XSetFont(xw.dis, dc.gc, g.mode & ATTR_BOLD ? dc.bfont->fid : dc.font->fid);
	XDrawImageString(xw.dis, xw.buf, dc.gc, r.x, r.y+dc.font->ascent, &g.c, 1);
}

void
draw(int dummy) {
	int x, y;

	xclear(0, 0, term.col-1, term.row-1);
	for(y = 0; y < term.row; y++)
		for(x = 0; x < term.col; x++)
			if(term.line[y][x].state & GLYPH_SET)
				xdrawc(x, y, term.line[y][x]);

	xdrawcursor();
	XCopyArea(xw.dis, xw.buf, xw.win, dc.gc, 0, 0, xw.bufw, xw.bufh, BORDER, BORDER);
	XFlush(xw.dis);
}

#else
/* optimized drawing routine */
void
draw(int redraw_all) {
	int i, x, y, ox;
	Glyph base, new;
	char buf[DRAW_BUF_SIZ];

	if(!xw.vis)
		return;

	xclear(0, 0, term.col-1, term.row-1);
	for(y = 0; y < term.row; y++) {
		base = term.line[y][0];
		i = ox = 0;
		for(x = 0; x < term.col; x++) {
			new = term.line[y][x];
			if(sel.bx!=-1 && new.c && selected(x, y))
				new.mode ^= ATTR_REVERSE;
			if(i > 0 && (!(new.state & GLYPH_SET) || ATTRCMP(base, new) ||
					i >= DRAW_BUF_SIZ)) {
				xdraws(buf, base, ox, y, i);
				i = 0;
			}
			if(new.state & GLYPH_SET) {
				if(i == 0) {
					ox = x;
					base = new;
				}
				buf[i++] = new.c;
			}
		}
		if(i > 0)
			xdraws(buf, base, ox, y, i);
	}
	xdrawcursor();
	XCopyArea(xw.dis, xw.buf, xw.win, dc.gc, 0, 0, xw.bufw, xw.bufh, BORDER, BORDER);
	XFlush(xw.dis);
}

#endif

void
expose(XEvent *ev) {
	draw(SCREEN_REDRAW);
}

void
visibility(XEvent *ev) {
	XVisibilityEvent *e = &ev->xvisibility;
	/* XXX if this goes from 0 to 1, need a full redraw for next Expose,
	 * not just a buf copy */
	xw.vis = e->state != VisibilityFullyObscured;
}

void
unmap(XEvent *ev) {
	xw.vis = 0;
}

void
xseturgency(int add) {
	XWMHints *h = XGetWMHints(xw.dis, xw.win);
	h->flags = add ? (h->flags | XUrgencyHint) : (h->flags & ~XUrgencyHint);
	XSetWMHints(xw.dis, xw.win, h);
	XFree(h);
}

void
focus(XEvent *ev) {
	if((xw.focus = ev->type == FocusIn))
		xseturgency(0);
	draw(SCREEN_UPDATE);
}

char*
kmap(KeySym k) {
	int i;
	for(i = 0; i < LEN(key); i++)
		if(key[i].k == k)
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

	if((customkey = kmap(ksym)))
		ttywrite(customkey, strlen(customkey));
	else if(len > 0) {
		buf[sizeof(buf)-1] = '\0';
		if(meta && len == 1)
			ttywrite("\033", 1);
		ttywrite(buf, len);
	} else
		switch(ksym) {
		case XK_Up:
		case XK_Down:
		case XK_Left:
		case XK_Right:
			sprintf(buf, "\033%c%c", IS_SET(MODE_APPKEYPAD) ? 'O' : '[', "DACB"[ksym - XK_Left]);
			ttywrite(buf, 3);
			break;
		case XK_Insert:
			if(shift)
				selpaste();
			break;
		default:
			fprintf(stderr, "errkey: %d\n", (int)ksym);
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
	xw.bufw = xw.w - 2*BORDER;
	xw.bufh = xw.h - 2*BORDER;
	col = xw.bufw / xw.cw;
	row = xw.bufh / xw.ch;
	tresize(col, row);
	ttyresize(col, row);
	xw.bufh = MAX(1, xw.bufh);
	xw.bufw = MAX(1, xw.bufw);
	XFreePixmap(xw.dis, xw.buf);
	xw.buf = XCreatePixmap(xw.dis, xw.win, xw.bufw, xw.bufh, XDefaultDepth(xw.dis, xw.scr));
}

void
run(void) {
	XEvent ev;
	fd_set rfd;
	int xfd = XConnectionNumber(xw.dis);

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
			draw(SCREEN_UPDATE); 
		}
		while(XPending(xw.dis)) {
			XNextEvent(xw.dis, &ev);
			if (XFilterEvent(&ev, xw.win))
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
		case 'e':
			if(++i < argc) opt_cmd = argv[i];
			break;
		case 'v':
		default:
			die(USAGE);
		}
	}
	setlocale(LC_CTYPE, "");
	tnew(80, 24);
	ttynew();
	xinit();
	selinit();
	run();
	return 0;
}
