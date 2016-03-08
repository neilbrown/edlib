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

DEF_CMD(ed_handle)
{
	struct editor *ed = container_of(ci->home, struct editor, root);
	int ret;

	ret = key_lookup(ed_map, ci);
	if (ret)
		return ret;
	return key_lookup(ed->commands, ci);
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
	ed->root.handle = &ed_handle;
	ed->root.data = NULL;

	ed->commands = key_alloc();
	return &ed->root;
}

int editor_load_module(struct editor *ed, char *name)
{
	char buf[PATH_MAX];
	void *h;
	void (*s)(struct editor *e);

	sprintf(buf, "edlib-%s.so", name);
	/* RTLD_GLOBAL is needed for python, else we get
	 * errors about _Py_ZeroStruct which a python script
	 * tries "import gtk"
	 *
	 */
	h = dlopen(buf, RTLD_NOW | RTLD_GLOBAL);
	if (!h)
		return 0;
	s = dlsym(h, "edlib_init");
	if (!s)
		return 0;
	s(ed);
	return 1;
}
