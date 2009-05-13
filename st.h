/* See LICENSE for licence details. */
#define _XOPEN_SOURCE
#include <ctype.h>
#include <fcntl.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>

/* special keys */
#define KEYDELETE "\033[3~"
#define KEYHOME   "\033[1~"
#define KEYEND    "\033[4~"
#define KEYPREV   "\033[5~"
#define KEYNEXT   "\033[6~"

#define TNAME "st"
#define SHELL "/bin/bash"
#define TAB    8

#define FONT "fixed"
#define BORDER 3
#define LINESPACE 1 /* additional pixel between each line */

/* Default colors */
#define DefaultFG 7
#define DefaultBG 0
#define DefaultCS 1
#define BellCol   DefaultFG /* visual bell color */

static char* colorname[] = {
	"black",
	"red",
	"green",
	"yellow",
	"blue",
	"magenta",
	"cyan",
	"white",
};

/* Arbitrary sizes */
#define ESCSIZ 256
#define ESCARG 16

#define MIN(a, b)  ((a) < (b) ? (a) : (b))
#define MAX(a, b)  ((a) < (b) ? (b) : (a))
#define LEN(a)     (sizeof(a) / sizeof(a[0]))
#define DEFAULT(a, b)     (a) = (a) ? (a) : (b)    
#define BETWEEN(x, a, b)  ((a) <= (x) && (x) <= (b))
#define LIMIT(x, a, b)    (x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x)


enum { ATnone=0 , ATreverse=1 , ATunderline=2, ATbold=4 }; /* Attribute */
enum { CSup, CSdown, CSright, CSleft, CShide, CSdraw, CSwrap, CSsave, CSload }; /* Cursor */
enum { CRset=1 , CRupdate=2 }; /* Character state */
enum { TMwrap=1 , TMinsert=2 }; /* Terminal mode */
enum { SCupdate, SCredraw }; /* screen draw mode */

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

/* Escape sequence structs */
typedef struct {
	char buf[ESCSIZ+1]; /* raw string */
	int len;            /* raw string length */
	/* ESC <pre> [[ [<priv>] <arg> [;]] <mode>] */
	char pre;           
	char priv;
	int arg[ESCARG+1];
	int narg;           /* nb of args */
	char mode;
} Escseq;

/* Internal representation of the screen */
typedef struct {
	int row;    /* nb row */  
	int col;    /* nb col */
	Line* line; /* screen */
	TCursor c;  /* cursor */
	int top;    /* top    scroll limit */
	int bot;    /* bottom scroll limit */
	int mode;   /* terminal mode */
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

/* Drawing Context */
typedef struct {
	unsigned long col[LEN(colorname)];
	XFontStruct* font;
	GC gc;
} DC;


void die(const char *errstr, ...);
void draw(int);
void execsh(void);
void kpress(XKeyEvent *);
void resize(XEvent *);
void run(void);

int escaddc(char);
int escfinal(char);
void escdump(void);
void eschandle(void);
void escparse(void);
void escreset(void);

void tclearregion(int, int, int, int);
void tcpos(int);
void tcursor(int);
void tdeletechar(int);
void tdeleteline(int);
void tdump(void);
void tinsertblank(int);
void tinsertblankline(int);
void tmoveto(int, int);
void tnew(int, int);
void tnewline(void);
void tputc(char);
void tputs(char*, int);
void tresize(int, int);
void tscroll(void);
void tsetattr(int*, int);
void tsetchar(char);
void tsetscroll(int, int);

void ttynew(void);
void ttyread(void);
void ttyresize(int, int);
void ttywrite(char *, size_t);

unsigned long xgetcol(const char *);
void xclear(int, int, int, int);
void xcursor(int);
void xdrawc(int, int, Glyph);
void xinit(void);
void xscroll(void);
