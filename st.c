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

#define TNAME "xterm"

/* Arbitrary sizes */
#define TITLESIZ 256
#define ESCSIZ 256
#define ESCARGSIZ 16
#define MAXDRAWBUF 1024

#define SERRNO strerror(errno)
#define MIN(a, b)  ((a) < (b) ? (a) : (b))
#define MAX(a, b)  ((a) < (b) ? (b) : (a))
#define LEN(a)     (sizeof(a) / sizeof(a[0]))
#define DEFAULT(a, b)     (a) = (a) ? (a) : (b)    
#define BETWEEN(x, a, b)  ((a) <= (x) && (x) <= (b))
#define LIMIT(x, a, b)    (x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x)
#define ATTRCMP(a, b) ((a).mode != (b).mode || (a).fg != (b).fg || (a).bg != (b).bg)

/* Attribute, Cursor, Character state, Terminal mode, Screen draw mode */
enum { ATnone=0 , ATreverse=1 , ATunderline=2, ATbold=4, ATgfx=8 };
enum { CSup, CSdown, CSright, CSleft, CShide, CSdraw, CSsave, CSload };
enum { CRset=1, CRupdate=2 };
enum { TMwrap=1, TMinsert=2 };
enum { ESCin=1, ESCcsi=2, ESCosc=4, ESCtitle=8, ESCcharset=16 };
enum { SCupdate, SCredraw };

typedef int Color;

typedef struct {
	char c;     /* character code  */
	char mode;  /* attribute flags */
	Color fg;   /* foreground      */
	Color bg;   /* background      */
	char state; /* state flag      */
} Glyph;

typedef Glyph* Line;

typedef struct {
	Glyph attr;  /* current char attributes */
	char hidden;
	int x;
	int y;
} TCursor;

/* CSI Escape sequence structs */
/* ESC '[' [[ [<priv>] <arg> [;]] <mode>] */
typedef struct {
	char buf[ESCSIZ]; /* raw string */
	int len;          /* raw string length */
	char priv;
	int arg[ESCARGSIZ];
	int narg;           /* nb of args */
	char mode;
} CSIEscape;

/* Internal representation of the screen */
typedef struct {
	int row;    /* nb row */  
	int col;    /* nb col */
	Line* line; /* screen */
	TCursor c;  /* cursor */
	int top;    /* top    scroll limit */
	int bot;    /* bottom scroll limit */
	int mode;   /* terminal mode */
	int esc;
	char title[TITLESIZ];
	int titlelen;
} Term;

/* Purely graphic info */
typedef struct {
	Display* dis;
	Window win;
	int scr;
	int w;  /* window width  */
	int h;  /* window height */
	int ch; /* char height */
	int cw; /* char width  */
} XWindow; 

typedef struct {
	KeySym k;
	char s[ESCSIZ];
} Key;

#include "config.h"

/* Drawing Context */
typedef struct {
	unsigned long col[LEN(colorname)];
	XFontStruct* font;
	GC gc;
} DC;

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
static void tcpos(int);
static void tcursor(int);
static void tdeletechar(int);
static void tdeleteline(int);
static void tinsertblank(int);
static void tinsertblankline(int);
static void tmoveto(int, int);
static void tnew(int, int);
static void tnewline(void);
static void tputc(char);
static void tputs(char*, int);
static void tresize(int, int);
static void tscroll(void);
static void tsetattr(int*, int);
static void tsetchar(char);
static void tsetscroll(int, int);

static void ttynew(void);
static void ttyread(void);
static void ttyresize(int, int);
static void ttywrite(const char *, size_t);

static unsigned long xgetcol(const char *);
static void xclear(int, int, int, int);
static void xcursor(int);
static void xdrawc(int, int, Glyph);
static void xinit(void);
static void xscroll(void);

static void expose(XEvent *);
static char * kmap(KeySym);
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
				putchar(c.state & CRset ? c.c : '.');
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
	char *args[3] = {SHELL, "-i", NULL};
	putenv("TERM=" TNAME);
	execvp(SHELL, args);
}

void
xbell(void) { /* visual bell */
	XRectangle r = { 0, 0, xw.w, xw.h };
	XSetForeground(xw.dis, dc.gc, dc.col[BellCol]);
	XFillRectangles(xw.dis, xw.win, dc.gc, &r, 1);
	/* usleep(30000); */
	draw(SCredraw);
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

	switch(ret = read(cmdfd, buf, BUFSIZ)) {
	case -1: 
		die("Couldn't read from shell: %s\n", SERRNO);
		break;
	default:
		tputs(buf, ret);
	}
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
tcpos(int mode) {
	static int x = 0;
	static int y = 0;

	if(mode == CSsave)
		x = term.c.x, y = term.c.y;
	else if(mode == CSload)
		tmoveto(x, y);
}

void
tnew(int col, int row) {   /* screen size */
	term.row = row, term.col = col;
	term.top = 0, term.bot = term.row - 1;
	/* mode */
	term.mode = TMwrap;
	/* cursor */
	term.c.attr.mode = ATnone;
	term.c.attr.fg = DefaultFG;
	term.c.attr.bg = DefaultBG;
	term.c.x = term.c.y = 0;
	term.c.hidden = 0;
	/* allocate screen */
	term.line = calloc(term.row, sizeof(Line));
	for(row = 0 ; row < term.row; row++)
		term.line[row] = calloc(term.col, sizeof(Glyph));
}

void
tscroll(void) {
	Line temp = term.line[term.top];
	int i;
	/* X stuff _before_ the line swapping (results in wrong line index) */
	xscroll();
	for(i = term.top; i < term.bot; i++)
		term.line[i] = term.line[i+1];
	memset(temp, 0, sizeof(Glyph) * term.col);
	term.line[term.bot] = temp;
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
		if(*p == ';' && escseq.narg+1 < ESCARGSIZ)
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
tcursor(int dir) {
	int xf = term.c.x, yf = term.c.y;
	
	switch(dir) {
	case CSup:
		yf--;
		break;
	case CSdown:
		yf++;
		break;
	case CSleft:
		xf--;
		if(term.mode & TMwrap && xf < 0) {
			xf = term.col-1, yf--;
			if(yf < term.top)
				yf = term.top, xf = 0;
		}
		break;
	case CSright:
		xf++;
		if(term.mode & TMwrap && xf >= term.col) {
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
	term.line[term.c.y][term.c.x].state |= CRset | CRupdate;
}

void
tclearregion(int x1, int y1, int x2, int y2) {
	int x, y;

	LIMIT(x1, 0, term.col-1);
	LIMIT(x2, 0, term.col-1);
	LIMIT(y1, 0, term.row-1);
	LIMIT(y2, 0, term.row-1);

	/* XXX: could be optimized */
	for(x = x1; x <= x2; x++)
		for(y = y1; y <= y2; y++)
			memset(&term.line[y][x], 0, sizeof(Glyph));

	xclear(x1, y1, x2, y2);
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
tsetlinestate(int n, int state) {
	int i;
	for(i = 0; i < term.col; i++)
		term.line[n][i].state |= state;
}

void
tinsertblankline (int n) {
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
		tsetlinestate(i, CRupdate);
		tsetlinestate(i-n, CRupdate);
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
		tsetlinestate(i, CRupdate);
		tsetlinestate(i-n, CRupdate);
	}
}

void
tsetattr(int *attr, int l) {
	int i;

	for(i = 0; i < l; i++) {
		switch(attr[i]) {
		case 0:
			term.c.attr.mode &= ~(ATreverse | ATunderline | ATbold);
			term.c.attr.fg = DefaultFG;
			term.c.attr.bg = DefaultBG;
			break;
		case 1:
			term.c.attr.mode |= ATbold;	 
			break;
		case 4: 
			term.c.attr.mode |= ATunderline;
			break;
		case 7: 
			term.c.attr.mode |= ATreverse;	
			break;
		case 8:
			term.c.hidden = CShide;
			break;
		case 22: 
			term.c.attr.mode &= ~ATbold;  
			break;
		case 24: 
			term.c.attr.mode &= ~ATunderline;
			break;
		case 27: 
			term.c.attr.mode &= ~ATreverse;	 
			break;
		case 39:
			term.c.attr.fg = DefaultFG;
			break;
		case 49:
			term.c.attr.fg = DefaultBG;
			break;
		default:
			if(BETWEEN(attr[i], 30, 37))
				term.c.attr.fg = attr[i] - 30;
			else if(BETWEEN(attr[i], 40, 47))
				term.c.attr.bg = attr[i] - 40;
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
	if(escseq.priv)
		csidump();
	switch(escseq.mode) {
	unknown:
	default:
		fprintf(stderr, "erresc: unknown sequence\n");
		csidump();
		/* die(""); */
		break;
	case '@': /* Insert <n> blank char */
		DEFAULT(escseq.arg[0], 1);
		tinsertblank(escseq.arg[0]);
		break;
	case 'A': /* Cursor <n> Up */
	case 'e':
		DEFAULT(escseq.arg[0], 1);
		tmoveto(term.c.x, term.c.y-escseq.arg[0]);
		break;
	case 'B': /* Cursor <n> Down */
		DEFAULT(escseq.arg[0], 1);
		tmoveto(term.c.x, term.c.y+escseq.arg[0]);
		break;
	case 'C': /* Cursor <n> Forward */
	case 'a':
		DEFAULT(escseq.arg[0], 1);
		tmoveto(term.c.x+escseq.arg[0], term.c.y);
		break;
	case 'D': /* Cursor <n> Backward */
		DEFAULT(escseq.arg[0], 1);
		tmoveto(term.c.x-escseq.arg[0], term.c.y);
		break;
	case 'E': /* Cursor <n> Down and first col */
		DEFAULT(escseq.arg[0], 1);
		tmoveto(0, term.c.y+escseq.arg[0]);
		break;
	case 'F': /* Cursor <n> Up and first col */
		DEFAULT(escseq.arg[0], 1);
		tmoveto(0, term.c.y-escseq.arg[0]);
		break;
	case 'G': /* Move to <col> */
	case '`':
		DEFAULT(escseq.arg[0], 1);
     	tmoveto(escseq.arg[0]-1, term.c.y);
		break;
	case 'H': /* Move to <row> <col> */
	case 'f':
		DEFAULT(escseq.arg[0], 1);
		DEFAULT(escseq.arg[1], 1);
		tmoveto(escseq.arg[1]-1, escseq.arg[0]-1);
		break;
	case 'J': /* Clear screen */
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
		}
		break;
	case 'K': /* Clear line */
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
	case 'S':
	case 'L': /* Insert <n> blank lines */
		DEFAULT(escseq.arg[0], 1);
		tinsertblankline(escseq.arg[0]);
		break;
	case 'l':
		if(escseq.priv) {
			switch(escseq.arg[0]) {
			case 7:
				term.mode &= ~TMwrap;
				break;
			case 25:
				term.c.hidden = 1;
				break;
			}
		}
		break;
	case 'M': /* Delete <n> lines */
		DEFAULT(escseq.arg[0], 1);
		tdeleteline(escseq.arg[0]);
		break;
	case 'X':
	case 'P': /* Delete <n> char */
		DEFAULT(escseq.arg[0], 1);
		tdeletechar(escseq.arg[0]);
		break;
	case 'd': /* Move to <row> */
		DEFAULT(escseq.arg[0], 1);
		tmoveto(term.c.x, escseq.arg[0]-1);
		break;
	case 'h': /* Set terminal mode */
		if(escseq.priv)
			switch(escseq.arg[0]) {
			case 7:
				term.mode |= TMwrap;
				break;
			case 25:
				term.c.hidden = 0;
				break;
			case 1034:
				/* XXX: Interpret "meta" key, sets eighth bit. */
				break;
			}
		break;
	case 'm': /* Terminal attribute (color) */
		tsetattr(escseq.arg, escseq.narg);
		break;
	case 'r':
		if(escseq.priv)
			goto unknown;
		else {
			DEFAULT(escseq.arg[0], 1);
			DEFAULT(escseq.arg[1], term.row);
			tsetscroll(escseq.arg[0]-1, escseq.arg[1]-1);
		}
		break;
	case 's': /* Save cursor position */
		tcpos(CSsave);
		break;
	case 'u': /* Load cursor position */
		tcpos(CSload);
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
		tcursor(CSright);
}

void
tputc(char c) {
#if 0
	dump(c);
#endif	
	if(term.esc & ESCin) {
		if(term.esc & ESCcsi) {
			escseq.buf[escseq.len++] = c;
			if(BETWEEN(c, 0x40, 0x7E) || escseq.len >= ESCSIZ) {
				term.esc = 0;
				csiparse(), csihandle();
			}
		} else if (term.esc & ESCosc) {
			if(c == ';') {
				term.titlelen = 0;
				term.esc = ESCin | ESCtitle;
			}
		} else if(term.esc & ESCtitle) {
			if(c == '\a' || term.titlelen+1 >= TITLESIZ) {
				term.esc = 0;
				term.title[term.titlelen] = '\0';
				XStoreName(xw.dis, xw.win, term.title);
			} else {
				term.title[term.titlelen++] = c;
			}
		} else if(term.esc & ESCcharset) {
			printf("ESC ( %c\n", c);
			switch(c) {
			case '0': /* Line drawing crap */
				term.c.attr.mode |= ATgfx;
				break;
			case 'B': /* Back to regular text */
				term.c.attr.mode &= ~ATgfx;
				break;
			}
			term.esc = 0;
		} else {		
			switch(c) {
			case '[':
				term.esc |= ESCcsi;
				break;
			case ']':
				term.esc |= ESCosc;
				break;
			case '(':
				term.esc |= ESCcharset;
				break;
			case 'A':
				tmoveto(term.c.x, term.c.y-1);
				break;
			case 'B':
				tmoveto(term.c.x, term.c.y+1);
				break;
			case 'C':
				tmoveto(term.c.x+1, term.c.y);
				break;
			case 'D':
				tmoveto(term.c.x-1, term.c.y);
				break;
			default:
				fprintf(stderr, "erresc: unknown sequence ESC %02X '%c'\n", c, isprint(c)?c:'.');
			}
		}
	} else {
		switch(c) {
		case '\t':
			tputtab();
			break;
		case '\b':
			tcursor(CSleft);
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
			term.esc = ESCin;
			break;
		default:
			tsetchar(c);
			tcursor(CSright);
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

unsigned long
xgetcol(const char *s) {
	XColor color;
	Colormap cmap = DefaultColormap(xw.dis, xw.scr);

	if(!XAllocNamedColor(xw.dis, cmap, s, &color, &color)) {
		color.pixel = WhitePixel(xw.dis, xw.scr);
		fprintf(stderr, "Could not allocate color '%s'\n", s);
	}
	return color.pixel;
}

void
xclear(int x1, int y1, int x2, int y2) {
	XClearArea(xw.dis, xw.win, 
			x1 * xw.cw, y1 * xw.ch, 
			(x2-x1+1) * xw.cw, (y2-y1+1) * xw.ch, 
			False);
}

void
xscroll(void) {
	int srcy = (term.top+1) * xw.ch;
	int dsty = term.top * xw.ch;
	int height = (term.bot-term.top) * xw.ch;

	xcursor(CShide);
	XCopyArea(xw.dis, xw.win, xw.win, dc.gc, 0, srcy, xw.w, height, 0, dsty);
	xclear(0, term.bot, term.col-1, term.bot);
}

void
xinit(void) {
	XGCValues values;
	unsigned long valuemask;
	XClassHint chint;
	XWMHints wmhint;
	XSizeHints shint;
	char *args[] = {NULL};
	int i;

	xw.dis = XOpenDisplay(NULL);
	xw.scr = XDefaultScreen(xw.dis);
	if(!xw.dis)
		die("Can't open display\n");
	
	/* font */
	if(!(dc.font = XLoadQueryFont(xw.dis, FONT)))
		die("Can't load font %s\n", FONT);

	xw.cw = dc.font->max_bounds.rbearing - dc.font->min_bounds.lbearing;
	xw.ch = dc.font->ascent + dc.font->descent + LINESPACE;

	/* colors */
	for(i = 0; i < LEN(colorname); i++)
		dc.col[i] = xgetcol(colorname[i]);

	term.c.attr.fg = DefaultFG;
	term.c.attr.bg = DefaultBG;
	term.c.attr.mode = ATnone;
	/* windows */
	xw.h = term.row * xw.ch;
	xw.w = term.col * xw.cw;
	/* XXX: this BORDER is useless after the first resize, handle it in xdraws() */
	xw.win = XCreateSimpleWindow(xw.dis, XRootWindow(xw.dis, xw.scr), 0, 0,
			xw.w, xw.h, BORDER, 
			dc.col[DefaultBG],
			dc.col[DefaultBG]);
	/* gc */
	values.foreground = XWhitePixel(xw.dis, xw.scr);
	values.font = dc.font->fid;
	valuemask = GCForeground | GCFont;
	dc.gc = XCreateGC(xw.dis, xw.win, valuemask, &values);
	XMapWindow(xw.dis, xw.win);
	/* wm stuff */
	chint.res_name = TNAME, chint.res_class = TNAME;
	wmhint.input = 1, wmhint.flags = InputHint;
	shint.height_inc = xw.ch, shint.width_inc = xw.cw;
	shint.height = xw.h, shint.width = xw.w;
	shint.flags = PSize | PResizeInc;
	XSetWMProperties(xw.dis, xw.win, NULL, NULL, &args[0], 0, &shint, &wmhint, &chint);
	XStoreName(xw.dis, xw.win, TNAME);
	XSync(xw.dis, 0);
}

void
xdraws (char *s, Glyph base, int x, int y, int len) {
	unsigned long xfg, xbg;
	int winx = x*xw.cw, winy = y*xw.ch + dc.font->ascent, width = len*xw.cw;
	int i;

	if(base.mode & ATreverse)
		xfg = dc.col[base.bg], xbg = dc.col[base.fg];
	else
		xfg = dc.col[base.fg], xbg = dc.col[base.bg];

	XSetBackground(xw.dis, dc.gc, xbg);
	XSetForeground(xw.dis, dc.gc, xfg);
	
	if(base.mode & ATgfx) {
	   
		for(i = 0; i < len; i++)
			s[i] = gfx[s[i]];
	}
	
	XDrawImageString(xw.dis, xw.win, dc.gc, winx, winy, s, len);
	
	if(base.mode & ATunderline)
		XDrawLine(xw.dis, xw.win, dc.gc, winx, winy+1, winx+width-1, winy+1);
}

void
xdrawc(int x, int y, Glyph g) {
	XRectangle r = { x * xw.cw, y * xw.ch, xw.cw, xw.ch };
	unsigned long xfg, xbg;

	/* reverse video */
	if(g.mode & ATreverse)
		xfg = dc.col[g.bg], xbg = dc.col[g.fg];
	else
		xfg = dc.col[g.fg], xbg = dc.col[g.bg];
	/* background */
	XSetBackground(xw.dis, dc.gc, xbg);
	XSetForeground(xw.dis, dc.gc, xfg);
	XDrawImageString(xw.dis, xw.win, dc.gc, r.x, r.y+dc.font->ascent, &g.c, 1);
}

void
xcursor(int mode) {
	static int oldx = 0;
	static int oldy = 0;
	Glyph g = {' ', ATnone, DefaultBG, DefaultCS, 0};
	
	LIMIT(oldx, 0, term.col-1);
	LIMIT(oldy, 0, term.row-1);
	
	if(term.line[term.c.y][term.c.x].state & CRset)
		g.c = term.line[term.c.y][term.c.x].c;
	/* remove the old cursor */
	if(term.line[oldy][oldx].state & CRset)
		xdrawc(oldx, oldy, term.line[oldy][oldx]);
	else 
		xclear(oldx, oldy, oldx, oldy);
	/* draw the new one */
	if(mode == CSdraw) {
		xdrawc(term.c.x, term.c.y, g);
		oldx = term.c.x, oldy = term.c.y;
	}
}

void
draw(int redraw_all) {
	int i, x, y, ox;
	Glyph base, new;
	char buf[MAXDRAWBUF];
	
	for(y = 0; y < term.row; y++) {
		base = term.line[y][0];
		i = ox = 0;
		for(x = 0; x < term.col; x++) {
			new = term.line[y][x];
			if(!ATTRCMP(base, new) && i < MAXDRAWBUF)
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
	if(!term.c.hidden)
		xcursor(CSdraw);
}

void
expose(XEvent *ev) {
	draw(SCredraw);
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

	meta  = e->state & Mod1Mask;
	shift = e->state & ShiftMask;
	len = XLookupString(e, buf, sizeof(buf), &ksym, NULL);

	if(customkey = kmap(ksym))
		ttywrite(customkey, strlen(customkey));
	else if(len > 0) {
		buf[sizeof(buf)-1] = '\0';
		if(meta && len == 1)
			ttywrite("\033", 1);
		ttywrite(buf, len);
	} else
		switch(ksym) {
		case XK_Insert:
			if(shift)
				/* XXX: paste X clipboard */;
			break;
		default:
			fprintf(stderr, "errkey: %d\n", (int)ksym);
			break;
		}
}

void
resize(XEvent *e) {
	int col, row;
	col = e->xconfigure.width / xw.cw;
	row = e->xconfigure.height / xw.ch;
	
	if(term.col != col || term.row != row) {
		tresize(col, row);
		ttyresize(col, row);
		xw.w = e->xconfigure.width;
		xw.h = e->xconfigure.height;
		draw(SCredraw);
	}
}

void
run(void) {
	XEvent ev;
	fd_set rfd;
	int xfd = XConnectionNumber(xw.dis);

	running = 1;
	XSelectInput(xw.dis, xw.win, ExposureMask | KeyPressMask | StructureNotifyMask);
	XResizeWindow(xw.dis, xw.win, xw.w , xw.h); /* fix resize bug in wmii (?) */

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
			draw(SCupdate);
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
