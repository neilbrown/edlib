/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distrubuted under terms of GPLv2 - see file:COPYING
 */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"

struct editor *editor_new(void)
{
	struct editor *ed = calloc(sizeof(*ed), 1);
	INIT_LIST_HEAD(&ed->documents);

	doc_make_docs(ed);
	ed->commands = key_alloc();
	return ed;
}
