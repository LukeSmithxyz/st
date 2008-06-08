#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LENGTH(x)	(sizeof(x) / sizeof((x)[0]))
#define MAX(a,b)	(((a) > (b)) ? (a) : (b))
#define MIN(a,b)	(((a) < (b)) ? (a) : (b))

void buffer(char c);
void cmd(const char *cmdstr, ...);
void *emallocz(unsigned int size);
void eprint(const char *errstr, ...);
void eprintn(const char *errstr, ...);
void getpty(void);
void movea(int x, int y);
void mover(int x, int y);
void parse(void);
void scroll(int l);
void shell(void);
void sigchld(int n);
char unbuffer(void);

typedef struct {
	unsigned char data[BUFSIZ];
	int s, e;
	int n;
} RingBuffer;

int cols = 80, lines = 25;
int cx = 0, cy = 0;
int c;
FILE *fptm = NULL;
int ptm, pts;
_Bool bold, digit, qmark;
pid_t pid;
RingBuffer buf;

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

void *
emallocz(unsigned int size) {
	void *res = calloc(1, size);

	if(!res)
		eprint("fatal: could not malloc() %u bytes\n", size);
	return res;
}

void
eprint(const char *errstr, ...) {
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(EXIT_FAILURE);
}

void
eprintn(const char *errstr, ...) {
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	fprintf(stderr, ": %s\n", strerror(errno));
	exit(EXIT_FAILURE);
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
			eprintn("error, cannot open pty");
#endif
#if defined(_XOPEN_SOURCE)
	if(ptm != -1) {
		if(grantpt(ptm) == -1)
			eprintn("error, cannot grant access to pty");
		if(unlockpt(ptm) == -1)
			eprintn("error, cannot unlock pty");
		ptsdev = ptsname(ptm);
		if(!ptsdev)
			eprintn("error, slave pty name undefined");
		pts = open(ptsdev, O_RDWR);
		if(pts == -1)
			eprintn("error, cannot open slave pty");
	}
	else
		eprintn("error, cannot open pty");
#endif
}

void
movea(int x, int y) {
	x = MAX(x, cols);
	y = MAX(y, lines);
	cx = x;
	cy = y;
	cmd("s %d,%d", x, y);
}

void
mover(int x, int y) {
	movea(cx + x, cy + y);
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
					eprint("syntax error\n");
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
						cmd("b");
				case 1:
					if(!bold)
						cmd("b");
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
	cmd("s %d, %d", cx, cy + l);
}

void
shell(void) {
	static char *shell = NULL;

	if(!shell && !(shell = getenv("SHELL")))
		shell = "/bin/sh";
	pid = fork();
	switch(pid) {
	case -1:
		eprint("error, cannot fork\n");
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
		eprintn("error, waiting for child failed");
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
	fd_set rd;
	if(argc == 2 && !strcmp("-v", argv[1]))
		eprint("std-"VERSION", © 2008 Matthias-Christian Ott\n");
	else if(argc == 1)
		eprint("usage: st [-v]\n");
	getpty();
	shell();
	return 0;
}
