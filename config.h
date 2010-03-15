#define SHELL "/bin/bash"
#define TAB    8

#define FONT "6x13"
#define BOLDFONT FONT"bold"
#define BORDER 2

/* Terminal colors */
static const char *colorname[] = {
	"black",
	"red",
	"green",
	"yellow",
	"blue",
	"magenta",
	"cyan",
	"white",
};

/* Default colors (colorname index) */
/* foreground, background, cursor, visual bell */
#define DefaultFG 7
#define DefaultBG 0
#define DefaultCS 1
#define BellCol   DefaultFG

/* special keys */
static Key key[] = {
	{ XK_Delete, "\033[3~" },
	{ XK_Home,   "\033[1~" },
	{ XK_End,    "\033[4~" },
	{ XK_Prior,  "\033[5~" },
	{ XK_Next,   "\033[6~" },
};

static char gfx[] = {
	['}'] = 'f',
	['.'] = 'v',
	[','] = '<',
	['+'] = '>',
	['-'] = '^',
	['h'] = '#',
	['~'] = 'o',
	['a'] = ':',
	['f'] = '\\',
	['`'] = '+',
	['z'] = '>',
	['{'] = '*',
	['q'] = '-',
	['i'] = '#',
	['n'] = '+',
	['y'] = '<',
	['m'] = '+',
	['j'] = '+',
	['|'] = '!',
	['g'] = '#',
	['o'] = '~',
	['p'] = '-',
	['r'] = '-',
	['s'] = '_',
	['0'] = '#',
	['w'] = '+',
	['u'] = '+',
	['t'] = '+',
	['v'] = '+',
	['l'] = '+',
	['k'] = '+',
	['x'] = '|',
	[255] = 0,
};
