#define SHELL "/bin/bash"
#define TAB    8

#define FONT "fixed"
#define BORDER 3
#define LINESPACE 1 /* additional pixel between each line */

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
static const char *key[] = {
	[XK_Delete] = "\033[3~", 
	[XK_Home]   = "\033[1~",
	[XK_End]    = "\033[4~",
	[XK_Prior]  = "\033[5~",
	[XK_Next]   = "\033[6~",
	[XK_Left]   = "\033[D",
	[XK_Right]  = "\033[C",
	[XK_Up]     = "\033[A",
	[XK_Down]   = "\033[B",
};
