#define TAB    8
#define TNAME "st-256color"
#define FONT "6x13"
#define BOLDFONT FONT"bold"
#define BORDER 2

/* Terminal colors */
static const char *colorname[] = {
	"black",
	"#CC0000",
	"#4E9A06",
	"#C4A000",
	"#3465A4",
	"#75507B",
	"#06989A",
	"#888a85",
	"#555753",
	"#EF2929",
	"#8AE234",
	"#FCE94F",
	"#729FCF",
	"#AD7FA8",
	"#34E2E2",
	"#EEEEEC"
};

/* Default colors (colorname index) */
/* foreground, background, cursor, visual bell */
#define DefaultFG 7
#define DefaultBG 0
#define DefaultCS 1
#define BellCol   DefaultFG

/* special keys */
static Key key[] = {
	{ XK_BackSpace, "\177" },
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
