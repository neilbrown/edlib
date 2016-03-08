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
	struct map *map = ci->home->data;
	int ret;

	ret = key_lookup(ed_map, ci);
	if (ret)
		return ret;
	return key_lookup(map, ci);
}

DEF_CMD(global_set_attr)
{
	attr_set_str(&ci->home->attrs, ci->str, ci->str2, -1);
	return 1;
}

DEF_CMD(global_set_command)
{
	struct map *map = ci->home->data;

	if (ci->str2)
		key_add_range(map, ci->str, ci->str2, ci->comm2);
	else
		key_add(map, ci->str, ci->comm2);
	return 1;
}

DEF_CMD(global_get_command)
{
	struct map *map = ci->home->data;
	struct command *cm = key_lookup_cmd(map, ci->str);
	struct cmd_info ci2 = {0};

	if (!cm)
		return -1;
	ci2.key = "callback:comm";
	ci2.focus = ci->focus;
	ci2.str = ci->str;
	ci2.comm2 = cm;
	ci2.comm = ci->comm2;
	if (ci2.comm)
		return ci2.comm->func(&ci2);
	return -1;
}

DEF_CMD(editor_load_module)
{
	char *name = ci->str;
	char buf[PATH_MAX];
	void *h;
	void (*s)(struct pane *p);

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
	s(ci->home);
	return 1;
}

struct pane *editor_new(void)
{
	struct pane *ed;

	if (!ed_map) {
		ed_map = key_alloc();
		key_add(ed_map, "global-set-attr", &global_set_attr);
		key_add(ed_map, "global-set-command", &global_set_command);
		key_add(ed_map, "global-get-command", &global_get_command);
		key_add(ed_map, "global-load-module", &editor_load_module);
	}

	ed = pane_register(NULL, 0, &ed_handle, key_alloc(), NULL);

	return ed;
}

