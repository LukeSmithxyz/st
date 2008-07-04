/* See LICENSE file for copyright and license details. */
#include "util.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *
emallocz(unsigned int size) {
	void *res = calloc(1, size);

	if(!res)
		err(EXIT_FAILURE, "could not malloc() %u bytes\n", size);
	return res;
}
