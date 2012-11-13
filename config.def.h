/* See LICENSE file for copyright and license details. */

/* appearance */
static char font[] = "Liberation Mono:pixelsize=12:antialias=false:autohint=false";
static int borderpx = 2;
static char shell[] = "/bin/sh";

/* double-click timeout (in milliseconds) between clicks for selection */
static unsigned int doubleclicktimeout = 300;
static unsigned int tripleclicktimeout = 600;

/* TERM value */
static char termname[] = "st-256color";

static unsigned int tabspaces = 8;


/* Terminal colors (16 first used in escape sequence) */
static const char *colorname[] = {
	/* 8 normal colors */
	"black",
	"red3",
	"green3",
	"yellow3",
	"blue2",
	"magenta3",
	"cyan3",
	"gray90",

	/* 8 bright colors */
	"gray50",
	"red",
	"green",
	"yellow",
	"#5c5cff",
	"magenta",
	"cyan",
	"white",

	[255] = 0,

	/* more colors can be added after 255 to use with DefaultXX */
	"#cccccc",
	"#333333",
};


/*
 * Default colors (colorname index)
 * foreground, background, cursor, unfocused cursor
 */
static unsigned int defaultfg = 7;
static unsigned int defaultbg = 0;
static unsigned int defaultcs = 256;
static unsigned int defaultucs = 257;

/*
 * Special keys (change & recompile st.info accordingly)
 *
 * Mask value:
 * * Use XK_ANY_MOD to match the key no matter modifiers state
 * * Use XK_NO_MOD to match the key alone (no modifiers)
 * keypad value:
 * * 0: no value
 * * > 0: keypad application mode enabled
 * * < 0: keypad application mode disabled
 * cursor value:
 * * 0: no value
 * * > 0: cursor application mode enabled
 * * < 0: cursor application mode disabled
 * crlf value
 * * 0: no value
 * * > 0: crlf mode is enabled
 * * < 0: crlf mode is disabled
 */

/* key, mask, output, keypad, cursor, crlf */
static Key key[] = {
	/* keysym             mask         string         keypad cursor crlf */
	{ XK_BackSpace,     XK_NO_MOD,      "\177",          0,    0,    0},
	{ XK_Up,            XK_NO_MOD,      "\033[A",        0,   -1,    0},
	{ XK_Up,            XK_NO_MOD,      "\033OA",        0,   +1,    0},
	{ XK_Up,            ShiftMask,      "\033[a",        0,    0,    0},
	{ XK_Down,          XK_NO_MOD,      "\033[B",        0,   -1,    0},
	{ XK_Down,          XK_NO_MOD,      "\033OB",        0,   +1,    0},
	{ XK_Down,          ShiftMask,      "\033[b",        0,    0,    0},
	{ XK_Left,     	    XK_NO_MOD,      "\033[D",        0,   -1,    0},
	{ XK_Left,          XK_NO_MOD,      "\033OD",        0,   +1,    0},
	{ XK_Left,          ShiftMask,      "\033[d",        0,    0,    0},
	{ XK_Right,         XK_NO_MOD,      "\033[C",        0,   -1,    0},
	{ XK_Right,         XK_NO_MOD,      "\033OC",        0,   +1,    0},
	{ XK_Right,         ShiftMask,      "\033[c",        0,    0,    0},
	{ XK_Return,        XK_NO_MOD,      "\n",            0,    0,   -1},
	{ XK_Return,        XK_NO_MOD,      "\r\n",          0,    0,   +1},
	{ XK_Return,        Mod1Mask,       "\033\n",        0,    0,   -1},
	{ XK_Return,        Mod1Mask,       "\033\r\n",      0,    0,   +1},
	{ XK_Insert,        XK_NO_MOD,      "\033[2~",       0,    0,    0},
	{ XK_Delete,        XK_NO_MOD,      "\033[3~",       0,    0,    0},
	{ XK_Home,          XK_NO_MOD,      "\033[1~",       0,    0,    0},
	{ XK_End,           XK_NO_MOD,      "\033[4~",       0,    0,    0},
	{ XK_Prior,         XK_NO_MOD,      "\033[5~",       0,    0,    0},
	{ XK_Next,          XK_NO_MOD,      "\033[6~",       0,    0,    0},
	{ XK_F1,            XK_NO_MOD,      "\033OP" ,       0,    0,    0},
	{ XK_F2,            XK_NO_MOD,      "\033OQ" ,       0,    0,    0},
	{ XK_F3,            XK_NO_MOD,      "\033OR" ,       0,    0,    0},
	{ XK_F4,            XK_NO_MOD,      "\033OS" ,       0,    0,    0},
	{ XK_F5,            XK_NO_MOD,      "\033[15~",      0,    0,    0},
	{ XK_F6,            XK_NO_MOD,      "\033[17~",      0,    0,    0},
	{ XK_F7,            XK_NO_MOD,      "\033[18~",      0,    0,    0},
	{ XK_F8,            XK_NO_MOD,      "\033[19~",      0,    0,    0},
	{ XK_F9,            XK_NO_MOD,      "\033[20~",      0,    0,    0},
	{ XK_F10,           XK_NO_MOD,      "\033[21~",      0,    0,    0},
	{ XK_F11,           XK_NO_MOD,      "\033[23~",      0,    0,    0},
	{ XK_F12,           XK_NO_MOD,      "\033[24~",      0,    0,    0},
};

/* Internal shortcuts. */
#define MODKEY Mod1Mask

static Shortcut shortcuts[] = {
	/* modifier		key		function	argument */
	{ MODKEY|ShiftMask,	XK_Prior,	xzoom,		{.i = +1} },
	{ MODKEY|ShiftMask,	XK_Next,	xzoom,		{.i = -1} },
	{ ShiftMask,		XK_Insert,	selpaste,	{.i =  0} },
};

