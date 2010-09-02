#define TAB 8
#define TNAME "st-256color"
#define FONT "-misc-*-medium-r-semicondensed-*-13-*-*-*-*-*-iso8859-*"
#define BOLDFONT "-misc-*-bold-r-semicondensed-*-13-*-*-*-*-*-iso8859-*"
#define BORDER 2
#define SHELL "/bin/sh"

/* Terminal colors */
static const char *colorname[] = {
	"black",
	"red3",
	"green3",
	"yellow3",
	"blue2",
	"magenta3",
	"cyan3",
	"gray90",
	"gray50",
	"red",
	"green",
	"yellow",
	"#5c5cff",
	"magenta",
	"cyan",
	"white"
};

/* Default colors (colorname index) */
/* foreground, background, cursor */
#define DefaultFG 7
#define DefaultBG 0
#define DefaultCS 1

/* Special keys */
static Key key[] = {
	{ XK_BackSpace, "\177" },
	{ XK_Delete,    "\033[3~" },
	{ XK_Home,      "\033[1~" },
	{ XK_End,       "\033[4~" },
	{ XK_Prior,     "\033[5~" },
	{ XK_Next,      "\033[6~" },
	{ XK_F1,        "\033OP"   },
	{ XK_F2,        "\033OQ"   },
	{ XK_F3,        "\033OR"   },
	{ XK_F4,        "\033OS"   },
	{ XK_F5,        "\033[15~" },
	{ XK_F6,        "\033[17~" },
	{ XK_F7,        "\033[18~" },
	{ XK_F8,        "\033[19~" },
	{ XK_F9,        "\033[20~" },
	{ XK_F10,       "\033[21~" },
	{ XK_F11,       "\033[23~" },
	{ XK_F12,       "\033[24~" },
};

/* Line drawing characters (sometime specific to each font...) */
static char gfx[] = {
	['`'] = 0x01,
	['a'] = 0x02,
	['f'] = 'o',
	['g'] = '+',
	['i'] = '#',
	['j'] = 0x0B,
	['k'] = 0x0C,
	['l'] = 0x0D,
	['m'] = 0x0E,
	['n'] = 0x0F,
	['o'] = 0x10,
	['p'] = 0x11,
	['q'] = 0x12,
	['r'] = 0x13,
	['s'] = 0x14,
	['t'] = 0x15,
	['u'] = 0x16,
	['v'] = 0x17,
	['w'] = 0x18,
	['x'] = 0x19,
	['y'] = 0x1A,
	['z'] = 0x1B,
	['{'] = 0x1C,
	['|'] = 0x1D,
	['}'] = 0x1E,
	['~'] = 0x1F,
	[255] = 0,
};
