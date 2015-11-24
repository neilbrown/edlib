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

struct map *ed_map;

DEF_LOOKUP_CMD(ed_handle, ed_map);

struct pane *editor_new(void)
{
	struct editor *ed = calloc(sizeof(*ed), 1);

	if (!ed_map)
		ed_map = key_alloc();

	pane_init(&ed->root, NULL, NULL);
	ed->root.handle = &ed_handle.c;
	ed->root.data = NULL;

	INIT_LIST_HEAD(&ed->documents);

	doc_make_docs(ed);
	ed->commands = key_alloc();
	point_new(ed->docs, &ed->docs_point);
	return &ed->root;
}

struct point *editor_choose_doc(struct editor *ed)
{
	/* Choose the first document with no watchers.
	 * If there isn't any, choose the last document
	 */
	struct doc *d, *choice = NULL, *last = NULL, *docs = NULL;

	list_for_each_entry(d, &ed->documents, list) {
		int i;
		if (d->deleting == 2)
			docs = d;
		if (d->deleting)
			continue;
		last = d;
		for (i = 0; i < d->nviews; i++)
			if (d->views[i].notify == NULL)
				break;
		if (i == d->nviews) {
			choice = d;
			break;
		}
	}
	if (!choice)
		choice = last;
	if (!choice)
		choice = docs;
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
