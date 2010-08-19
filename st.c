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
#include <X11/keysym.h>
#include <X11/Xutil.h>

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

/* Attribute, Cursor, Character state, Terminal mode, Screen draw mode */
enum { ATTR_NULL=0 , ATTR_REVERSE=1 , ATTR_UNDERLINE=2, ATTR_BOLD=4, ATTR_GFX=8 };
enum { CURSOR_UP, CURSOR_DOWN, CURSOR_LEFT, CURSOR_RIGHT, CURSOR_HIDE, CURSOR_DRAW, 
       CURSOR_SAVE, CURSOR_LOAD };
enum { GLYPH_SET=1, GLYPH_DIRTY=2 };
enum { MODE_WRAP=1, MODE_INSERT=2, MODE_APPKEYPAD=4 };
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
	Line* line; /* screen */
	TCursor c;	/* cursor */
	char hidec;
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
	Window win;
	Pixmap buf;
	int scr;
	int w;	/* window width	 */
	int h;	/* window height */
	int bufw; /* pixmap width  */
	int bufh; /* pixmap height */
	int ch; /* char height */
	int cw; /* char width  */
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
static void tmovecursor(int);
static void tdeletechar(int);
static void tdeleteline(int);
static void tinsertblank(int);
static void tinsertblankline(int);
static void tmoveto(int, int);
static void tnew(int, int);
static void tnewline(void);
static void tputc(char);
static void tputs(char*, int);
static void treset(void);
static void tresize(int, int);
static void tscroll(void);
static void tscrollup(int);
static void tscrolldown(int);
static void tsetattr(int*, int);
static void tsetchar(char);
static void tsetscroll(int, int);

static void ttynew(void);
static void ttyread(void);
static void ttyresize(int, int);
static void ttywrite(const char *, size_t);

static void xclear(int, int, int, int);
static void xcursor(int);
static void xinit(void);
static void xloadcols(void);

static void expose(XEvent *);
static char* kmap(KeySym);
static void kpress(XEvent *);
static void resize(XEvent *);

static void (*handler[LASTEvent])(XEvent *) = {
	[KeyPress] = kpress,
	[Expose] = expose,
	[ConfigureNotify] = resize
};

/* Globals */
static DC dc;
static XWindow xw;
static Term term;
static CSIEscape escseq;
static int cmdfd;
static pid_t pid;
static int running;

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
	char *args[3] = {getenv("SHELL"), "-i", NULL};
	DEFAULT(args[0], "/bin/sh"); /* default shell if getenv() failed */
	putenv("TERM=" TNAME);
	execvp(args[0], args);
}

void
xbell(void) { /* visual bell */
	XRectangle r = { BORDER, BORDER, xw.bufw, xw.bufh };
	XSetForeground(xw.dis, dc.gc, dc.col[BellCol]);
	XFillRectangles(xw.dis, xw.win, dc.gc, &r, 1);
	/* usleep(30000); */
	draw(SCREEN_REDRAW);
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
	char *pts;

	if((m = posix_openpt(O_RDWR | O_NOCTTY)) < 0)
		die("openpt failed: %s\n", SERRNO);
	if(grantpt(m) < 0)
		die("grandpt failed: %s\n", SERRNO);
	if(unlockpt(m) < 0)
		die("unlockpt failed: %s\n", SERRNO);
	if(!(pts = ptsname(m)))
		die("ptsname failed: %s\n", SERRNO);
	if((s = open(pts, O_RDWR | O_NOCTTY)) < 0)
		die("Couldn't open slave: %s\n", SERRNO);
	fcntl(s, F_SETFL, O_NDELAY);
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
			die("ioctl TTIOCSTTY failed: %s\n", SERRNO);
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
	char buf[BUFSIZ] = {0};
	int ret;

	if((ret = read(cmdfd, buf, BUFSIZ)) < 0)
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
	term.c.attr.mode = ATTR_NULL;
	term.c.attr.fg = DefaultFG;
	term.c.attr.bg = DefaultBG;
	term.c.x = term.c.y = 0;
	term.hidec = 0;
	term.top = 0, term.bot = term.row - 1;
	term.mode = MODE_WRAP;
	tclearregion(0, 0, term.col-1, term.row-1);
}

void
tnew(int col, int row) {   /* screen size */
	term.row = row, term.col = col;
	term.top = 0, term.bot = term.row - 1;
	/* mode */
	term.mode = MODE_WRAP;
	/* cursor */
	term.c.attr.mode = ATTR_NULL;
	term.c.attr.fg = DefaultFG;
	term.c.attr.bg = DefaultBG;
	term.c.x = term.c.y = 0;
	term.hidec = 0;
	/* allocate screen */
	term.line = calloc(term.row, sizeof(Line));
	for(row = 0 ; row < term.row; row++)
		term.line[row] = calloc(term.col, sizeof(Glyph));
}

/* TODO: Replace with scrollup/scolldown */
void
tscroll(void) {
	Line temp = term.line[term.top];
	int i;

	for(i = term.top; i < term.bot; i++)
		term.line[i] = term.line[i+1];
	memset(temp, 0, sizeof(Glyph) * term.col);
	term.line[term.bot] = temp;
}

void
tscrolldown (int n) {
	int i;
	Line temp;
	
	LIMIT(n, 0, term.bot-term.top+1);

	for(i = 0; i < n; i++)
		memset(term.line[term.bot-i], 0, term.col*sizeof(Glyph));
	
	for(i = term.bot; i >= term.top+n; i--) {
		temp = term.line[i];
		term.line[i] = term.line[i-n];
		term.line[i-n] = temp;
	}
}

void
tscrollup (int n) {
	int i;
	Line temp;
	LIMIT(n, 0, term.bot-term.top+1);
	
	for(i = 0; i < n; i++)
		memset(term.line[term.top+i], 0, term.col*sizeof(Glyph));
	
	 for(i = term.top; i <= term.bot-n; i++) { 
		 temp = term.line[i];
		 term.line[i] = term.line[i+n]; 
		 term.line[i+n] = temp;
	 }
}

void
tnewline(void) {
	int y = term.c.y + 1;
	if(y > term.bot)
		tscroll(), y = term.bot;
	tmoveto(0, y);
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
	term.c.x = x < 0 ? 0 : x >= term.col ? term.col-1 : x;
	term.c.y = y < 0 ? 0 : y >= term.row ? term.row-1 : y;
}

void
tmovecursor(int dir) {
	int xf = term.c.x, yf = term.c.y;
	
	switch(dir) {
	case CURSOR_UP:
		yf--;
		break;
	case CURSOR_DOWN:
		yf++;
		break;
	case CURSOR_LEFT:
		xf--;
		if(term.mode & MODE_WRAP && xf < 0) {
			xf = term.col-1, yf--;
			if(yf < term.top)
				yf = term.top, xf = 0;
		}
		break;
	case CURSOR_RIGHT:
		xf++;
		if(term.mode & MODE_WRAP && xf >= term.col) {
			xf = 0, yf++;
			if(yf > term.bot)
				yf = term.bot, tscroll();
		}
		break;
	}
	tmoveto(xf, yf);
}
	
void
tsetchar(char c) {
	term.line[term.c.y][term.c.x] = term.c.attr;
	term.line[term.c.y][term.c.x].c = c;
	term.line[term.c.y][term.c.x].state |= GLYPH_SET;
}

void
tclearregion(int x1, int y1, int x2, int y2) {
	int y, temp;

	if(x1 > x2)
		temp = x1, x1 = x2, x2 = temp;
	if(y1 > y2)
		temp = y1, y1 = y2, y2 = temp;

	LIMIT(x1, 0, term.col-1);
	LIMIT(x2, 0, term.col-1);
	LIMIT(y1, 0, term.row-1);
	LIMIT(y2, 0, term.row-1);

	for(y = y1; y <= y2; y++)
		memset(&term.line[y][x1], 0, sizeof(Glyph)*(x2-x1+1));
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
	tclearregion(term.col-size, term.c.y, term.col-1, term.c.y);
}

void
tinsertblank(int n) {
	int src = term.c.x;
	int dst = src + n;
	int size = term.col - n - src;

	if(dst >= term.col) {
		tclearregion(term.c.x, term.c.y, term.col-1, term.c.y);
		return;
	}
	memmove(&term.line[term.c.y][dst], &term.line[term.c.y][src], size * sizeof(Glyph));
	tclearregion(src, term.c.y, dst, term.c.y);
}

void
tinsertblankline(int n) {
	int i;
	Line blank;
	int bot = term.bot;

	if(term.c.y > term.bot)
		bot = term.row - 1;
	else if(term.c.y < term.top)
		bot = term.top - 1;
	if(term.c.y + n >= bot) {
		tclearregion(0, term.c.y, term.col-1, bot);
		return;
	}
	for(i = bot; i >= term.c.y+n; i--) {
		/* swap deleted line <-> blanked line */
		blank = term.line[i];
		term.line[i] = term.line[i-n];
		term.line[i-n] = blank;
		/* blank it */
		memset(blank, 0, term.col * sizeof(Glyph));
	}
}

void
tdeleteline(int n) {
	int i;
	Line blank;
	int bot = term.bot;

	if(term.c.y > term.bot)
		bot = term.row - 1;
	else if(term.c.y < term.top)
		bot = term.top - 1;
	if(term.c.y + n >= bot) {
		tclearregion(0, term.c.y, term.col-1, bot);
		return;
	}
	for(i = term.c.y; i <= bot-n; i++) {
		/* swap deleted line <-> blanked line */
		blank = term.line[i];
		term.line[i] = term.line[i+n];
		term.line[i+n] = blank;
		/* blank it */
		memset(blank, 0, term.col * sizeof(Glyph));
	}
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
				fprintf(stderr, "erresc: gfx attr %d unknown\n", attr[i]); 
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
		case 3: /* XXX: erase saved lines (xterm) */
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
		tscrollup(escseq.arg[0]);
		break;
	case 'T': /* SD -- Scroll <n> line down */
		DEFAULT(escseq.arg[0], 1);
		tscrolldown(escseq.arg[0]);
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
			case 7:
				term.mode &= ~MODE_WRAP;
				break;
			case 12: /* att610 -- Stop blinking cursor (IGNORED) */
				break;
			case 25:
				term.hidec = 1;
				break;
			case 1048: /* XXX: no alt. screen to erase/save */
			case 1049:
				tcursor(CURSOR_LOAD);
				tclearregion(0, 0, term.col-1, term.row-1);
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
			case 7:
				term.mode |= MODE_WRAP;
				break;
			case 12: /* att610 -- Start blinking cursor (IGNORED) */
				break;
			case 25:
				term.hidec = 0;
				break;
			case 1048: 
			case 1049: /* XXX: no alt. screen to erase/save */
				tcursor(CURSOR_SAVE);
				tclearregion(0, 0, term.col-1, term.row-1);
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
	
	if(term.c.x + space >= term.col)
		space--;
	
	for(; space > 0; space--)
		tmovecursor(CURSOR_RIGHT);
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
			case 'A':
				tmoveto(term.c.x, term.c.y-1);
				term.esc = 0;
				break;
			case 'B':
				tmoveto(term.c.x, term.c.y+1);
				term.esc = 0;
				break;
			case 'C':
				tmoveto(term.c.x+1, term.c.y);
				term.esc = 0;
				break;
			case 'D': /* XXX: CUP (VT100) or IND (VT52) ... */
				tmoveto(term.c.x-1, term.c.y);
				term.esc = 0;
				break;
			case 'E': /* NEL -- Next line */
				tnewline();
				term.esc = 0;
				break;
			case 'M': /* RI -- Reverse index */
				if(term.c.y == term.top)
					tscrolldown(1);
				else
					tmoveto(term.c.x, term.c.y-1);
				term.esc = 0;
				break;
			case 'c': /* RIS -- Reset to inital state */
				treset();
				term.esc = 0;
				break;
			case '=': /* DECPAM */
				term.mode |= MODE_APPKEYPAD;
				term.esc = 0;
				break;
			case '>': /* DECPNM */
				term.mode &= ~MODE_APPKEYPAD;
				term.esc = 0;
				break;
			case '7':
				tcursor(CURSOR_SAVE);
				term.esc = 0;
				break;
			case '8':
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
			tmovecursor(CURSOR_LEFT);
			break;
		case '\r':
			tmoveto(0, term.c.y);
			break;
		case '\n':
			tnewline();
			break;
		case '\a':
			xbell();
			break;
		case '\033':
			csireset();
			term.esc = ESC_START;
			break;
		default:
			tsetchar(c);
			tmovecursor(CURSOR_RIGHT);
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
	int i;
	Line *line;
	int minrow = MIN(row, term.row);
	int mincol = MIN(col, term.col);

	if(col < 1 || row < 1)
		return;
	/* alloc */
	line = calloc(row, sizeof(Line));
	for(i = 0 ; i < row; i++)
		line[i] = calloc(col, sizeof(Glyph));
	/* copy */
	for(i = 0 ; i < minrow; i++)
		memcpy(line[i], term.line[i], mincol * sizeof(Glyph));
	/* free */
	for(i = 0; i < term.row; i++)
		free(term.line[i]);
	free(term.line);
	
	LIMIT(term.c.x, 0, col-1);
	LIMIT(term.c.y, 0, row-1);
	LIMIT(term.top, 0, row-1);
	LIMIT(term.bot, 0, row-1);
	
	term.bot = row-1;
	term.line = line;
	term.col = col, term.row = row;
}

void
xloadcols(void) {
	int i, r, g, b;
	XColor color;
	Colormap cmap = DefaultColormap(xw.dis, xw.scr);
	unsigned long white = WhitePixel(xw.dis, xw.scr);

	for(i = 0; i < 16; i++) {
		if (!XAllocNamedColor(xw.dis, cmap, colorname[i], &color, &color)) {
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
				if (!XAllocColor(xw.dis, cmap, &color)) {
					dc.col[i] = white;
					fprintf(stderr, "Could not allocate color %d\n", i);
				} else
					dc.col[i] = color.pixel;
				i++;
			}

	for(r = 0; r < 24; r++, i++) {
		color.red = color.green = color.blue = 0x0808 + 0x0a0a * r;
		if (!XAllocColor(xw.dis, cmap, &color)) {
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
	xw.dis = XOpenDisplay(NULL);
	xw.scr = XDefaultScreen(xw.dis);
	if(!xw.dis)
		die("Can't open display\n");
	
	/* font */
	if(!(dc.font = XLoadQueryFont(xw.dis, FONT)) || !(dc.bfont = XLoadQueryFont(xw.dis, BOLDFONT)))
		die("Can't load font %s\n", dc.font ? BOLDFONT : FONT);

	/* XXX: Assuming same size for bold font */
	xw.cw = dc.font->max_bounds.rbearing - dc.font->min_bounds.lbearing;
	xw.ch = dc.font->ascent + dc.font->descent;

	/* colors */
	xloadcols();

	term.c.attr.fg = DefaultFG;
	term.c.attr.bg = DefaultBG;
	term.c.attr.mode = ATTR_NULL;
	/* windows */
	xw.h = term.row * xw.ch + 2*BORDER;
	xw.w = term.col * xw.cw + 2*BORDER;
	xw.win = XCreateSimpleWindow(xw.dis, XRootWindow(xw.dis, xw.scr), 0, 0,
			xw.w, xw.h, 0,
			dc.col[DefaultBG],
			dc.col[DefaultBG]);
	xw.bufw = xw.w - 2*BORDER;
	xw.bufh = xw.h - 2*BORDER;
	xw.buf = XCreatePixmap(xw.dis, xw.win, xw.bufw, xw.bufh, XDefaultDepth(xw.dis, xw.scr));
	/* gc */
	dc.gc = XCreateGC(xw.dis, xw.win, 0, NULL);
	XMapWindow(xw.dis, xw.win);
	xhints();
	XStoreName(xw.dis, xw.win, "st");
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
		for(i = 0; i < len; i++)
			s[i] = gfx[(int)s[i]];

	XSetFont(xw.dis, dc.gc, base.mode & ATTR_BOLD ? dc.bfont->fid : dc.font->fid);
	XDrawImageString(xw.dis, xw.buf, dc.gc, winx, winy, s, len);
	
	if(base.mode & ATTR_UNDERLINE)
		XDrawLine(xw.dis, xw.buf, dc.gc, winx, winy+1, winx+width-1, winy+1);
}

void
xcursor(int mode) {
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
	if(mode == CURSOR_DRAW) {
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

	if(!term.hidec)
		xcursor(CURSOR_DRAW);
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
	
	XSetForeground(xw.dis, dc.gc, dc.col[DefaultBG]);
	XFillRectangle(xw.dis, xw.buf, dc.gc, 0, 0, xw.w, xw.h);
	for(y = 0; y < term.row; y++) {
		base = term.line[y][0];
		i = ox = 0;
		for(x = 0; x < term.col; x++) {
			new = term.line[y][x];
			if(!ATTRCMP(base, new) && i < DRAW_BUF_SIZ)
				buf[i++] = new.c;
			else {
				xdraws(buf, base, ox, y, i);
				buf[0] = new.c;
				i = 1;
				ox = x;
				base = new;
			}
		}
		xdraws(buf, base, ox, y, i);
	}
	xcursor(term.hidec ? CURSOR_HIDE : CURSOR_DRAW);
	XCopyArea(xw.dis, xw.buf, xw.win, dc.gc, 0, 0, xw.bufw, xw.bufh, BORDER, BORDER);
	XFlush(xw.dis);
}

#endif

void
expose(XEvent *ev) {
	draw(SCREEN_REDRAW);
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

	meta = e->state & Mod1Mask;
	shift = e->state & ShiftMask;
	len = XLookupString(e, buf, sizeof(buf), &ksym, NULL);

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
			sprintf(buf, "\033%c%c", term.mode & MODE_APPKEYPAD ? 'O' : '[', "DACB"[ksym - XK_Left]);
			ttywrite(buf, 3);
			break;
		case XK_Insert:
			if(shift)
				draw(1), puts("draw!")/* XXX: paste X clipboard */;
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
	XFreePixmap(xw.dis, xw.buf);
	xw.buf = XCreatePixmap(xw.dis, xw.win, xw.bufw, xw.bufh, XDefaultDepth(xw.dis, xw.scr));
	draw(SCREEN_REDRAW);
}

void
run(void) {
	XEvent ev;
	fd_set rfd;
	int xfd = XConnectionNumber(xw.dis);

	running = 1;
	XSelectInput(xw.dis, xw.win, ExposureMask | KeyPressMask | StructureNotifyMask);
	XResizeWindow(xw.dis, xw.win, xw.w, xw.h); /* XXX: fix resize bug in wmii (?) */

	while(running) {
		FD_ZERO(&rfd);
		FD_SET(cmdfd, &rfd);
		FD_SET(xfd, &rfd);
		if(select(MAX(xfd, cmdfd)+1, &rfd, NULL, NULL, NULL) == -1) {
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
			if(handler[ev.type])
				(handler[ev.type])(&ev);
		}
	}
}

int
main(int argc, char *argv[]) {
	if(argc == 2 && !strncmp("-v", argv[1], 3))
		die("st-" VERSION ", © 2009 st engineers\n");
	else if(argc != 1)
		die("usage: st [-v]\n");
	setlocale(LC_CTYPE, "");
	tnew(80, 24);
	ttynew();
	xinit();
	run();
	return 0;
}
