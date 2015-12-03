/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
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

DEF_CMD(null_display_handle)
{
	return 0;
}

DEF_CMD(global_set_attr)
{
	attr_set_str(&ci->home->attrs, ci->str, ci->str2, -1);
	return 1;
}

DEF_CMD(global_set_command)
{
	struct editor *ed = container_of(ci->home, struct editor, root);

	key_add(ed->commands, ci->str, ci->comm2);
	return 1;
}

struct pane *editor_new(void)
{
	struct editor *ed = calloc(sizeof(*ed), 1);

	if (!ed_map) {
		ed_map = key_alloc();
		key_add(ed_map, "global-set-attr", &global_set_attr);
		key_add(ed_map, "global-set-command", &global_set_command);
	}

	pane_init(&ed->root, NULL, NULL);
	ed->root.handle = &ed_handle.c;
	ed->root.data = NULL;

	/* The first child of the root is the 'null_display'
	 * which holds one pane for every document.
	 */
	pane_register(&ed->root, 0,
		      &null_display_handle, NULL, NULL);

	doc_make_docs(ed);

	ed->commands = key_alloc();
	return &ed->root;
}

struct pane *editor_choose_doc(struct editor *ed)
{
	/* Choose the first document with no watchers.
	 * If there isn't any, choose the last document
	 */
	struct doc *d, *choice = NULL, *last = NULL, *docs = NULL;
	struct pane *p;

	list_for_each_entry(p, &ed->root.focus->children, siblings) {
		int i;
		struct doc_data *dd = p->data;
		d = dd->doc;
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
	return choice->home;
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
