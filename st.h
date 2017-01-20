/* See LICENSE for license details. */

/* Arbitrary sizes */
#define UTF_SIZ       4

/* macros */
#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define MAX(a, b)		((a) < (b) ? (b) : (a))
#define LEN(a)			(sizeof(a) / sizeof(a)[0])
#define BETWEEN(x, a, b)	((a) <= (x) && (x) <= (b))
#define DIVCEIL(n, d)		(((n) + ((d) - 1)) / (d))
#define LIMIT(x, a, b)		(x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x)
#define ATTRCMP(a, b)		((a).mode != (b).mode || (a).fg != (b).fg || \
				(a).bg != (b).bg)
#define IS_SET(flag)		((term.mode & (flag)) != 0)
#define TIMEDIFF(t1, t2)	((t1.tv_sec-t2.tv_sec)*1000 + \
				(t1.tv_nsec-t2.tv_nsec)/1E6)
#define MODBIT(x, set, bit)	((set) ? ((x) |= (bit)) : ((x) &= ~(bit)))

#define TRUECOLOR(r,g,b)	(1 << 24 | (r) << 16 | (g) << 8 | (b))
#define IS_TRUECOL(x)		(1 << 24 & (x))

enum glyph_attribute {
	ATTR_NULL       = 0,
	ATTR_BOLD       = 1 << 0,
	ATTR_FAINT      = 1 << 1,
	ATTR_ITALIC     = 1 << 2,
	ATTR_UNDERLINE  = 1 << 3,
	ATTR_BLINK      = 1 << 4,
	ATTR_REVERSE    = 1 << 5,
	ATTR_INVISIBLE  = 1 << 6,
	ATTR_STRUCK     = 1 << 7,
	ATTR_WRAP       = 1 << 8,
	ATTR_WIDE       = 1 << 9,
	ATTR_WDUMMY     = 1 << 10,
	ATTR_BOLD_FAINT = ATTR_BOLD | ATTR_FAINT,
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
	MODE_UTF8        = 1 << 21,
	MODE_SIXEL       = 1 << 22,
	MODE_MOUSE       = MODE_MOUSEBTN|MODE_MOUSEMOTION|MODE_MOUSEX10\
	                  |MODE_MOUSEMANY,
};

enum selection_mode {
	SEL_IDLE = 0,
	SEL_EMPTY = 1,
	SEL_READY = 2
};

enum selection_type {
	SEL_REGULAR = 1,
	SEL_RECTANGULAR = 2
};

enum selection_snap {
	SNAP_WORD = 1,
	SNAP_LINE = 2
};

enum window_state {
	WIN_VISIBLE = 1,
	WIN_FOCUSED = 2
};

typedef unsigned char uchar;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef unsigned short ushort;

typedef uint_least32_t Rune;

typedef struct {
	Rune u;           /* character code */
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

/* Internal representation of the screen */
typedef struct {
	int row;      /* nb row */
	int col;      /* nb col */
	Line *line;   /* screen */
	Line *alt;    /* alternate screen */
	int *dirty;  /* dirtyness of lines */
	GlyphFontSpec *specbuf; /* font spec buffer used for rendering */
	TCursor c;    /* cursor */
	int top;      /* top    scroll limit */
	int bot;      /* bottom scroll limit */
	int mode;     /* terminal mode flags */
	int esc;      /* escape state flags */
	char trantbl[4]; /* charset table translation */
	int charset;  /* current charset */
	int icharset; /* selected charset for sequence */
	int numlock; /* lock numbers in keyboard */
	int *tabs;
} Term;

/* Purely graphic info */
typedef struct {
	int tw, th; /* tty width and height */
	int w, h; /* window width and height */
	int ch; /* char height */
	int cw; /* char width  */
	char state; /* focus, redraw, visible */
	int cursor; /* cursor style */
} TermWindow;

typedef struct {
	uint b;
	uint mask;
	char *s;
} MouseShortcut;

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

	char *primary, *clipboard;
	int alt;
	struct timespec tclick1;
	struct timespec tclick2;

	//Atom xtarget;
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

void die(const char *, ...);
void redraw(void);

int tattrset(int);
void tnew(int, int);
void tsetdirt(int, int);
void tsetdirtattr(int);
int match(uint, uint);
void ttynew(void);
size_t ttyread(void);
void ttyresize(void);
void ttysend(char *, size_t);
void ttywrite(const char *, size_t);

void resettitle(void);

char *kmap(KeySym, uint);
void cresize(int, int);
void selclear(void);

void selinit(void);
void selnormalize(void);
int selected(int, int);
char *getsel(void);
int x2col(int);
int y2row(int);

size_t utf8decode(char *, Rune *, size_t);
size_t utf8encode(Rune, char *);

void *xmalloc(size_t);
char *xstrdup(char *);

void usage(void);

/* Globals */
extern TermWindow win;
extern Term term;
extern Selection sel;
extern int cmdfd;
extern pid_t pid;
extern char **opt_cmd;
extern char *opt_class;
extern char *opt_embed;
extern char *opt_font;
extern char *opt_io;
extern char *opt_line;
extern char *opt_name;
extern char *opt_title;
extern int oldbutton;

extern char *usedfont;
extern double usedfontsize;
extern double defaultfontsize;

/* config.h globals */
extern char font[];
extern int borderpx;
extern float cwscale;
extern float chscale;
extern unsigned int doubleclicktimeout;
extern unsigned int tripleclicktimeout;
extern int allowaltscreen;
extern unsigned int xfps;
extern unsigned int actionfps;
extern unsigned int cursorthickness;
extern unsigned int blinktimeout;
extern char termname[];
extern const char *colorname[];
extern size_t colornamelen;
extern unsigned int defaultfg;
extern unsigned int defaultbg;
extern unsigned int defaultcs;
extern unsigned int defaultrcs;
extern unsigned int cursorshape;
extern unsigned int cols;
extern unsigned int rows;
extern unsigned int mouseshape;
extern unsigned int mousefg;
extern unsigned int mousebg;
extern unsigned int defaultattr;
extern MouseShortcut mshortcuts[];
extern size_t mshortcutslen;
extern Shortcut shortcuts[];
extern size_t shortcutslen;
extern uint forceselmod;
extern uint selmasks[];
extern size_t selmaskslen;
extern char ascii_printable[];
