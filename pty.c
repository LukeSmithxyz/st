/* See LICENSE file for copyright and license details. */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#if !(_POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600)
#include <pty.h>
#endif

extern int ptm, pts;

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
