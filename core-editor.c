/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distributed under terms of GPLv2 - see file:COPYING
 */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>

#include "core.h"

struct pane *editor_new(void)
{
	struct editor *ed = calloc(sizeof(*ed), 1);
	struct pane *p;

	INIT_LIST_HEAD(&ed->documents);

	doc_make_docs(ed);
	ed->commands = key_alloc();
	ed->null_display.ed = ed;
	p = pane_register(NULL, 0, NULL, &ed->null_display, NULL);
	point_new(ed->docs, &p->point);
	return p;
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

int editor_load_module(struct editor *ed, char *name)
{
	char buf[PATH_MAX];
	void *h;
	void (*s)(struct editor *e);

	sprintf(buf, "edlib-%s.so", name);
	h = dlopen(buf, RTLD_NOW);
	if (!h)
		return 0;
	s = dlsym(h, "edlib_init");
	if (!s)
		return 0;
	s(ed);
	return 1;
}
