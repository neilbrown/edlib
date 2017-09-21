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

static struct map *ed_map safe;
struct ed_info {
	unsigned long magic;
	struct pane *freelist;
	struct idle_call {
		struct idle_call *next;
		struct pane *focus safe;
		struct command *callback safe;
	} *idle_calls;
	struct map *map safe;
	struct store {
		struct store *next;
		int size;
		char space[];
	} *store;
};

DEF_CMD(ed_handle)
{
	struct ed_info *ei = ci->home->data;
	struct map *map = ei->map;
	int ret;

	ret = key_lookup(map, ci);
	if (!ret)
		ret = key_lookup(ed_map, ci);
	return ret;
}

DEF_CMD(global_set_attr)
{
	if (!ci->str)
		return -1;
	attr_set_str(&ci->home->attrs, ci->str, ci->str2);
	return 1;
}

DEF_CMD(global_set_command)
{
	struct ed_info *ei = ci->home->data;
	struct map *map = ei->map;

	if (!ci->str)
		return -1;
	if (ci->str2)
		key_add_range(map, ci->str, ci->str2, ci->comm2);
	else
		key_add(map, ci->str, ci->comm2);
	return 1;
}

DEF_CMD(global_get_command)
{
	struct ed_info *ei = ci->home->data;
	struct map *map = ei->map;
	struct command *cm;

	if (!ci->str ||
	    !(cm = key_lookup_cmd(map, ci->str)))
		return -1;
	return comm_call(ci->comm2, "callback:comm", ci->focus, 0, NULL, ci->str,
			 0, NULL, NULL, cm);
}

DEF_CMD(editor_load_module)
{
	struct ed_info *ei = ci->home->data;
	struct map *map = ei->map;
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
	struct ed_info *ei = ci->home->data;
	struct map *map = ei->map;
	char *mod = ci->key + 7;

	if (strncmp(mod, "doc-", 4) == 0 ||
	    strncmp(mod, "render-", 7) == 0 ||
	    strncmp(mod, "mode-", 5) == 0 ||
	    strncmp(mod, "display-", 8) == 0)
		;
	else {
		char *m = strrchr(ci->key+6, '-');
		if (m) {
			m += 1;
			mod = malloc(4+strlen(m)+1);
			strcpy(mod, "lib-");
			strcpy(mod+4, m);
		}
	}

	ret = call("global-load-module", ci->home, 0, NULL,
		    mod, 0);
	if (mod != ci->key + 7)
		free(mod);

	if (ret > 0)
		/* auto-load succeeded */
		return key_lookup(map, ci);
	return 0;
}

DEF_CMD(editor_auto_event)
{
	/* Event handled register under a private name so we
	 * have to use key_lookup_prefix to find them.
	 * If nothing is found, autoload lib-libevent (hack?)
	 */
	struct ed_info *ei = ci->home->data;
	struct map *map = ei->map;
	int ret = key_lookup_prefix(map, ci);

	if (ret)
		return ret;
	call("attach-libevent", ci->home);
	return key_lookup_prefix(map, ci);
}

DEF_CMD(editor_multicall)
{
	struct ed_info *ei = ci->home->data;
	struct map *map = ei->map;

	((struct cmd_info*)ci)->key += strlen("global-multicall-");
	return key_lookup_prefix(map, ci);
}

DEF_CMD(editor_clean_up)
{
	struct ed_info *ei = ci->home->data;

	while (ei->idle_calls) {
		struct idle_call *i = ei->idle_calls;
		ei->idle_calls = i->next;
		comm_call(i->callback, "idle-callback", i->focus);
		command_put(i->callback);
		free(i);
	}
	while (ei->freelist) {
		struct pane *p = ei->freelist;
		ei->freelist = p->focus;
		attr_free(&p->attrs);
		free(p);
	}
	while (ei->store) {
		struct store *s = ei->store;
		ei->store = s->next;
		free(s);
	}
	return 0;
}

DEF_CMD(editor_on_idle)
{
	/* register comm2 to be called when next idle. */
	struct ed_info *ei = ci->home->data;
	struct idle_call *ic;

	if (!ci->comm2)
		return -1;

	ic = calloc(1, sizeof(*ic));
	ic->focus = ci->focus;
	ic->callback = command_get(ci->comm2);
	ic->next = ei->idle_calls;
	ei->idle_calls = ic;
	return 1;
}

void *memsave(struct pane *p safe, char *buf, int len)
{
	struct ed_info *ei;
	if (!buf || !len)
		return NULL;
	while (p->parent)
		p = p->parent;
	ei = p->data;
	ASSERT(ei->magic==0x4321765498765432UL);
	if (ei->store == NULL || ei->store->size < len) {
		struct store *s;
		int l = 4096 - sizeof(*s);
		while (l < len)
			l += 4096;
		s = malloc(l + sizeof(*s));
		s->next = ei->store;
		s->size = l;
		ei->store = s;
	}
	ei->store->size -= len;
	return memcpy(ei->store->space+ei->store->size, buf, len);
}

char *strsave(struct pane *p safe, char *buf)
{
	if (!buf)
		return NULL;
	return memsave(p, buf, strlen(buf)+1);
}

void editor_delayed_free(struct pane *ed safe, struct pane *p safe)
{
	struct ed_info *ei = ed->data;
	if (!ei) {
		attr_free(&p->attrs);
		free(p);
		return;
	}
	ASSERT(ei->magic==0x4321765498765432UL);
	p->focus = ei->freelist;
	ei->freelist = p;
}

struct pane *editor_new(void)
{
	struct pane *ed;
	struct ed_info *ei = calloc(1, sizeof(*ei));

	ei->magic = 0x4321765498765432UL;
	if (! (void*) ed_map) {
		ed_map = key_alloc();
		key_add(ed_map, "global-set-attr", &global_set_attr);
		key_add(ed_map, "global-set-command", &global_set_command);
		key_add(ed_map, "global-get-command", &global_get_command);
		key_add(ed_map, "global-load-module", &editor_load_module);
		key_add(ed_map, "editor-on-idle", &editor_on_idle);
		key_add_range(ed_map, "attach-", "attach.", &editor_auto_load);
		key_add_range(ed_map, "event:", "event;", &editor_auto_event);
		key_add_range(ed_map, "global-multicall-", "global-multicall.",
			      &editor_multicall);
	}
	ei->map = key_alloc();
	key_add(ei->map, "on_idle-clean_up", &editor_clean_up);
	ed = pane_register(NULL, 0, &ed_handle, ei, NULL);

	doc_setup(ed);
	return ed;
}
