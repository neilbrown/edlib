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

	ret = key_lookup(map, ci);
	if (!ret)
		ret = key_lookup(ed_map, ci);
	return ret;
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
	struct map *map = ci->home->data;
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
	if (h) {
		s = dlsym(h, "edlib_init");
		if (s) {
			s(ci->home);
			return 1;
		}
	}
	return key_lookup_prefix(map, ci);
}

DEF_CMD(editor_auto_load)
{
	int ret;
	struct map *map = ci->home->data;
	char *mod = ci->key + 7;

	if (strncmp(mod, "doc-", 4) == 0 ||
	    strncmp(mod, "render-", 7) == 0 ||
	    strncmp(mod, "mode-", 5) == 0 ||
	    strncmp(mod, "display-", 8) == 0)
		;
	else if (strcmp(mod, "global-keymap") == 0) {
		mod = strdup("lib-keymap");
	} else {
		mod = malloc(4+strlen(mod)+1);
		strcpy(mod, "lib-");
		strcpy(mod+4, ci->key + 7);
	}

	ret = call5("global-load-module", ci->home, 0, NULL,
		    mod, 0);
	if (mod != ci->key + 7)
		free(mod);

	if (ret > 0)
		/* auto-load succeeded */
		return key_lookup(map, ci);
	return 0;
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
		key_add_range(ed_map, "attach-", "attach.", &editor_auto_load);
	}

	ed = pane_register(NULL, 0, &ed_handle, key_alloc(), NULL);

	return ed;
}
