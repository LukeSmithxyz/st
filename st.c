/* See LICENSE file for copyright and license details. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char *argv[]) {
	if(argc == 2 && !strcmp("-v", argv[1])) {
		fprintf(stderr, "st-"VERSION", © 2007-2008 st engineers, see LICENSE for details\n");
		exit(EXIT_SUCCESS);
	}
	else if(argc != 1) {
		fprintf(stderr, "usage: st [-v]\n");
		exit(EXIT_FAILURE);
	}
	return 0;
}
