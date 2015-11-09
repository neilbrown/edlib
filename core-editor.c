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

struct point *editor_choose_doc(struct editor *ed)
{
	/* Choose the first document with no watchers.
	 * If there isn't any, choose the last document
	 */
	struct doc *d, *choice;
	choice = list_last_entry(&ed->documents, struct doc, list);
	list_for_each_entry(d, &ed->documents, list) {
		int i;
		for (i = 0; i < d->nviews; i++)
			if (d->views[i].notify == NULL)
				break;
		if (i == d->nviews) {
			choice = d;
			break;
		}
	}
	return point_new(choice, NULL);
}
