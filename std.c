/* See LICENSE file for copyright and license details.
 *
 * Simple terminal daemon is a terminal emulator. It can be used in
 * combination with simple terminal to emulate a mostly VT100-compatible
 * terminal.
 * 
 * In this process std works like a filter. It reads data from a
 * pseudo-terminal and parses the escape sequences and transforms them
 * into an ed(1)-like language. The resulting data is buffered and
 * written to stdout.
 * Parallely it reads data from stdin and parses and executes the
 * commands. The resulting data is written to the pseudo-terminal.
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#if !(_POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600)
#include <pty.h>
#endif
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LENGTH(x)	(sizeof(x) / sizeof((x)[0]))
#define MAX(a,b)	(((a) > (b)) ? (a) : (b))
#define MIN(a,b)	(((a) < (b)) ? (a) : (b))

typedef struct {
	unsigned char data[BUFSIZ];
	int s, e;
	int n;
} RingBuffer;

static void buffer(char c);
static void cmd(const char *cmdstr, ...);
static void getpty(void);
static void movea(int x, int y);
static void mover(int x, int y);
static void parsecmd(void);
static void parseesc(void);
static void scroll(int l);
static void shell(void);
static void sigchld(int n);
static char unbuffer(void);

static int cols = 80, lines = 25;
static int cx = 0, cy = 0;
static int c;
static int ptm, pts;
static _Bool bold, digit, qmark;
static pid_t pid;
static RingBuffer buf;
static FILE *fptm;

void
buffer(char c) {
	if(buf.n < LENGTH(buf.data))
		buf.n++;
	else
		buf.s = (buf.s + 1) % LENGTH(buf.data);
	buf.data[buf.e++] = c;
	buf.e %= LENGTH(buf.data);
}

void
cmd(const char *cmdstr, ...) {
	va_list ap;

	putchar('\n');
	putchar(':');
	va_start(ap, cmdstr);
	vfprintf(stdout, cmdstr, ap);
	va_end(ap);
}

void
movea(int x, int y) {
	x = MAX(x, cols);
	y = MAX(y, lines);
	cx = x;
	cy = y;
	cmd("seek(%d,%d)", x, y);
}

void
mover(int x, int y) {
	movea(cx + x, cy + y);
}

void
parsecmd(void) {
}

void
parseesc(void) {
	int i, j;
	int arg[16];

	memset(arg, 0, LENGTH(arg));
	c = getc(fptm);
	switch(c) {
	case '[':
		c = getc(fptm);
		for(j = 0; j < LENGTH(arg);) {
			if(isdigit(c)) {
				digit = 1;
				arg[j] *= 10;
				arg[j] += c - '0';
			}
			else if(c == '?')
				qmark = 1;
			else if(c == ';') {
				if(!digit)
					errx(EXIT_FAILURE, "syntax error");
				digit = 0;
				j++;
			}
			else {
				if(digit) {
					digit = 0;
					j++;
				}
				break;
			}
			c = getc(fptm);
		}
		switch(c) {
		case '@':
			break;
		case 'A':
			mover(0, j ? arg[0] : 1);
			break;
		case 'B':
			mover(0, j ? -arg[0] : -1);
			break;
		case 'C':
			mover(j ? arg[0] : 1, 0);
			break;
		case 'D':
			mover(j ? -arg[0] : -1, 0);
			break;
		case 'E':
			/* movel(j ? arg[0] : 1); */
			break;
		case 'F':
			/* movel(j ? -arg[0] : -1); */
			break;
		case '`':
		case 'G':
			movea(j ? arg[0] : 1, cy);
			break;
		case 'f':
		case 'H':
			movea(arg[1] ? arg[1] : 1, arg[0] ? arg[0] : 1);
		case 'L':
			/* insline(j ? arg[0] : 1); */
			break;
		case 'M':
			/* delline(j ? arg[0] : 1); */
			break;
		case 'P':
			break;
		case 'S':
			scroll(j ? arg[0] : 1);
			break;
		case 'T':
			scroll(j ? -arg[0] : -1);
			break;
		case 'd':
			movea(cx, j ? arg[0] : 1);
			break;
		case 'm':
			for(i = 0; i < j; i++) {
				if(arg[i] >= 30 && arg[i] <= 37)
					cmd("#%d", arg[i] - 30);
				if(arg[i] >= 40 && arg[i] <= 47)
					cmd("|%d", arg[i] - 40);
				/* xterm bright colors */
				if(arg[i] >= 90 && arg[i] <= 97)
					cmd("#%d", arg[i] - 90);
				if(arg[i] >= 100 && arg[i] <= 107)
					cmd("|%d", arg[i] - 100);
				switch(arg[i]) {
				case 0:
				case 22:
					if(bold)
						cmd("bold");
				case 1:
					if(!bold)
						cmd("bold");
					break;
				}
			}
			break;
		}
		break;
	default:
		putchar('\033');
		ungetc(c, fptm);
	}
}

void
scroll(int l) {
	cmd("seek(%d,%d)", cx, cy + l);
}

void
getpty(void) {
	char *ptsdev;

#if defined(_GNU_SOURCE)
	ptm = getpt();
#elif _POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600
	ptm = posix_openpt(O_RDWR);
#else
	ptm = open("/dev/ptmx", O_RDWR);
	if(ptm == -1)
		if(openpty(&ptm, &pts, NULL, NULL, NULL) == -1)
			err(EXIT_FAILURE, "cannot open pty");
#endif
#if defined(_XOPEN_SOURCE)
	if(ptm != -1) {
		if(grantpt(ptm) == -1)
			err(EXIT_FAILURE, "cannot grant access to pty");
		if(unlockpt(ptm) == -1)
			err(EXIT_FAILURE, "cannot unlock pty");
		ptsdev = ptsname(ptm);
		if(!ptsdev)
			err(EXIT_FAILURE, "slave pty name undefined");
		pts = open(ptsdev, O_RDWR);
		if(pts == -1)
			err(EXIT_FAILURE, "cannot open slave pty");
	}
	else
		err(EXIT_FAILURE, "cannot open pty");
#endif
}

void
shell(void) {
	static char *shell = NULL;

	if(!shell && !(shell = getenv("SHELL")))
		shell = "/bin/sh";
	pid = fork();
	switch(pid) {
	case -1:
		err(EXIT_FAILURE, "cannot fork");
	case 0:
		setsid();
		dup2(pts, STDIN_FILENO);
		dup2(pts, STDOUT_FILENO);
		dup2(pts, STDERR_FILENO);
		close(ptm);
		putenv("TERM=vt102");
		execvp(shell, NULL);
		break;
	default:
		close(pts);
		signal(SIGCHLD, sigchld);
	}
}

void
sigchld(int n) {
	int ret;

	if(waitpid(pid, &ret, 0) == -1)
		err(EXIT_FAILURE, "waiting for child failed");
	if(WIFEXITED(ret))
		exit(WEXITSTATUS(ret));
	else
		exit(EXIT_SUCCESS);
}

char
unbuffer(void) {
	char c;

	c = buf.data[buf.s++];
	buf.s %= LENGTH(buf.data);
	buf.n--;
	return c;
}

int
main(int argc, char *argv[]) {
	fd_set rfds;

	if(argc == 2 && !strcmp("-v", argv[1]))
		errx(EXIT_SUCCESS, "std-"VERSION", © 2008 Matthias-Christian Ott");
	else if(argc == 1)
		errx(EXIT_FAILURE, "usage: std [-v]");
	getpty();
	shell();
	FD_ZERO(&rfds);
	FD_SET(STDIN_FILENO, &rfds);
	FD_SET(ptm, &rfds);
	if(!(fptm = fdopen(ptm, "r+")))
		err(EXIT_FAILURE, "cannot open pty");
	if(fcntl(ptm, F_SETFL, O_NONBLOCK) == -1)
		err(EXIT_FAILURE, "cannot set pty to non-blocking mode");
	if(fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) == -1)
		err(EXIT_FAILURE, "cannot set stdin to non-blocking mode");
	for(;;) {
		if(select(MAX(ptm, STDIN_FILENO) + 1, &rfds, NULL, NULL, NULL) == -1)
			err(EXIT_FAILURE, "cannot select");
		if(FD_ISSET(ptm, &rfds)) {
			for(;;) {
				if((c = getc(fptm)) == EOF) {
					if(feof(fptm)) {
						FD_CLR(ptm, &rfds);
						fflush(fptm);
						break;
					}
					if(errno != EAGAIN)
						err(EXIT_FAILURE, "cannot read from pty");
					fflush(stdout);
					break;
				}
				switch(c) {
				case '\033':
					parseesc();
					break;
				default:
					putchar(c);
				}
			}
			fflush(stdout);
		}
		if(FD_ISSET(STDIN_FILENO, &rfds)) {
			for(;;) {
				if((c = getchar()) == EOF) {
					if(feof(stdin)) {
						FD_CLR(STDIN_FILENO, &rfds);
						fflush(fptm);
						break;
					}
					if(errno != EAGAIN)
						err(EXIT_FAILURE, "cannot read from stdin");
					fflush(fptm);
					break;
				}
				switch(c) {
				case ':':
					parsecmd();
					break;
				default:
					putc(c, fptm);
				}
			}
			fflush(fptm);
		}
	}
	return 0;
}
