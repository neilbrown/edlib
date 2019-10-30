/*
 * Copyright Neil Brown ©2015-2019 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 */
#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <dlfcn.h>

#include "core.h"
#include "misc.h"

static struct map *ed_map safe;
struct ed_info {
	unsigned long magic;
	struct pane *freelist;
	struct mark *mark_free_list;
	struct idle_call {
		struct idle_call *next;
		struct pane *focus safe;
		struct command *callback safe;
	} *idle_calls;
	struct map *map safe;
	struct lookup_cmd cmd;
	struct store {
		struct store *next;
		int size;
		char space[];
	} *store;
};

DEF_LOOKUP_CMD(ed_handle, ed_map);


DEF_CMD(global_set_attr)
{
	if (!ci->str)
		return Enoarg;
	attr_set_str(&ci->home->attrs, ci->str, ci->str2);
	return 1;
}

DEF_CMD(global_set_command)
{
	struct ed_info *ei = ci->home->data;
	struct map *map = ei->map;

	if (!ci->str)
		return Enoarg;
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
	    !(cm = key_lookup_cmd(map, ci->str, NULL, NULL)))
		return Efail;
	return comm_call(ci->comm2, "callback:comm", ci->focus, 0, NULL, ci->str,
			 0, NULL, NULL, 0,0, cm);
}

#ifdef edlib_init
#include "O/mod-list-decl.h"
typedef void init_func(struct pane *ed);
static struct builtin {
	char *name;
	init_func *func;
} builtins[]={
	#include "O/mod-list.h"
};
#endif
DEF_CMD(editor_load_module)
{
	struct ed_info *ei = ci->home->data;
	struct map *map = ei->map;
	char *name = ci->str;
	char buf[PATH_MAX];
#ifndef edlib_init
	void *h;
	void (*s)(struct pane *p);
	char **path;
	char pbuf[PATH_MAX];

	sprintf(buf, "edlib-%s.so", name);
	/* RTLD_GLOBAL is needed for python, else we get
	 * errors about _Py_ZeroStruct which a python script
	 * tries "import gtk"
	 *
	 */
	h = dlopen(buf, RTLD_NOW | RTLD_GLOBAL);
	if (h) {
		path = dlsym(h, "edlib_module_path");
		if (path) {
			if (dlinfo(h, RTLD_DI_ORIGIN, pbuf) == 0)
				*path = pbuf;
		}
		s = dlsym(h, "edlib_init");
		if (s) {
			s(ci->home);
			if (path)
				*path = NULL;
			return 1;
		}
	}
#else
	unsigned int i;

	strcpy(buf, name);
	for (i = 0; buf[i]; i++)
		if (buf[i] == '-')
			buf[i] = '_';
	for (i = 0; i < sizeof(builtins)/sizeof(builtins[0]); i++)
		if (strcmp(builtins[i].name, buf) == 0) {
			builtins[i].func(ci->home);
			return 1;
		}
#endif
	return key_lookup_prefix(map, ci);
}

DEF_CMD(editor_auto_load)
{
	int ret;
	struct ed_info *ei = ci->home->data;
	struct map *map = ei->map;
	char *mod = ci->key + 7;

	/* Check the key really doesn't exist, rather than
	 * it fails
	 */
	if (key_lookup_cmd(map, ci->key, NULL, NULL))
		return 0;

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
	/* Event handlers register under a private name so we
	 * have to use key_lookup_prefix to find them.
	 * If nothing is found, autoload lib-libevent (hack?)
	 */
	struct ed_info *ei = ci->home->data;
	struct map *map = ei->map;
	int ret = key_lookup_prefix(map, ci);

	if (ret)
		return ret;
	if (strcmp(ci->key, "event:refresh") == 0)
		/* pointless to autoload for refresh */
		return 0;
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

DEF_CMD(editor_global_request_notify)
{
	pane_add_notify(ci->focus, ci->home, ci->key + 8);
	return 1;
}

DEF_CMD(editor_global_notify)
{
	return pane_notify(ci->key + 5, ci->home, ci->num, ci->mark, ci->str,
	                   ci->num2, ci->mark2, ci->str2, ci->comm2);
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
	while (ei->mark_free_list) {
		struct mark *m = ei->mark_free_list;
		ei->mark_free_list = (struct mark*)m->all.next;
		free(m);
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
		return Enoarg;

	ic = calloc(1, sizeof(*ic));
	ic->focus = ci->focus;
	ic->callback = command_get(ci->comm2);
	ic->next = ei->idle_calls;
	ei->idle_calls = ic;
	return 1;
}

DEF_CMD(editor_close)
{

	stat_free();
	return 0;
}

void * safe memsave(struct pane *p safe, char *buf, int len)
{
	struct ed_info *ei;

	p = pane_root(p);
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
	if (buf)
		return memcpy(ei->store->space+ei->store->size, buf, len);
	else
		return ei->store->space+ei->store->size;
}

char *strsave(struct pane *p safe, char *buf)
{
	if (!buf)
		return NULL;
	return memsave(p, buf, strlen(buf)+1);
}

char *strnsave(struct pane *p safe, char *buf, int len)
{
	char *s;
	if (!buf)
		return NULL;
	s = memsave(p, buf, len+1);
	if (s)
		s[len] = 0;
	return s;
}

char * safe __strconcat(struct pane *p safe, char *s1 safe, ...)
{
	va_list ap;
	char *s;
	int len = 0;
	char *ret;

	len = strlen(s1);
	va_start(ap, s1);
	while ((s = va_arg(ap, char*)) != NULL)
		len += strlen(s);
	va_end(ap);

	ret = memsave(p, NULL, len+1);
	strcpy(ret, s1);
	va_start(ap, s1);
	while ((s = va_arg(ap, char*)) != NULL)
		strcat(ret, s);
	va_end(ap);
	return ret;
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

void editor_delayed_mark_free(struct mark *m safe)
{
	struct pane *p = pane_root(m->owner->home);
	struct ed_info *ei = p ? p->data : NULL;

	if (!ei) {
		free(m);
		return;
	}
	ASSERT(ei->magic==0x4321765498765432UL);
	m->all.next = (void*)ei->mark_free_list;
	ei->mark_free_list = m;
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
		key_add_prefix(ed_map, "attach-", &editor_auto_load);
		key_add_prefix(ed_map, "event:", &editor_auto_event);
		key_add_prefix(ed_map, "global-multicall-", &editor_multicall);
		key_add_prefix(ed_map, "Request:Notify:global-",
		               &editor_global_request_notify);
		key_add_prefix(ed_map, "Call:Notify:global-",
		               &editor_global_notify);
		key_add(ed_map, "Close", &editor_close);
	}
	ei->map = key_alloc();
	key_add(ei->map, "on_idle-clean_up", &editor_clean_up);
	key_add_chain(ei->map, ed_map);
	ei->cmd = ed_handle;
	ei->cmd.m = &ei->map;
	ed = pane_register(NULL, 0, &ei->cmd.c, ei);

	doc_setup(ed);
	return ed;
}
