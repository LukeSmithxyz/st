/* See LICENSE file for copyright and license details. */
#include <stdio.h>

int
Xmain(int argc, char *argv[]) {
	if(argc == 2 && !strcmp("-v", argv[1]))
		eprint("st-"VERSION", © 2007-2008 st engineers, see LICENSE for details\n");
	else if(argc != 1)
		eprint("usage: st [-v]\n");
	return 0;
}
