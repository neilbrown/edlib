/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 */
#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdarg.h>
#include <dlfcn.h>

#define PANE_DATA_TYPE struct ed_info
#include "core.h"
#include "misc.h"
#include "internal.h"

#define ED_MAGIC 0x4321fedcUL

static struct map *ed_map safe;
struct ed_info {
	unsigned long magic;
	struct pane *freelist;
	struct mark *mark_free_list;
	struct map *map safe;
	struct lookup_cmd cmd;
	/* These two paths contain nul-terminated strings,
	 * with a double-nul at the end.
	 */
	char *data_path;
	char *config_path;
	char *bin_path;
	char *here;
	bool testing;
	struct store {
		struct store *next;
		int size;
		char space[];
	} *store;
};
#include "core-pane.h"

bool edlib_testing(struct pane *p safe)
{
	struct ed_info *ei = &pane_root(p)->data;
	return ei->testing;
}

DEF_LOOKUP_CMD(ed_handle, ed_map);

DEF_CMD(global_set_attr)
{
	char *v;
	if (!ci->str)
		return Enoarg;
	if (!ci->num) {
		attr_set_str(&ci->home->attrs, ci->str, ci->str2);
		return 1;
	}
	/* Append */
	if (!ci->str2)
		return 1;
	v = attr_find(ci->home->attrs, ci->str);
	if (!v) {
		attr_set_str(&ci->home->attrs, ci->str, ci->str2);
		return 1;
	}
	v = strconcat(ci->home, v, ci->str2);
	attr_set_str(&ci->home->attrs, ci->str, v);
	return 1;
}

DEF_CMD(global_set_command)
{
	struct ed_info *ei = &ci->home->data;
	struct map *map = ei->map;
	bool prefix = strcmp(ci->key, "global-set-command-prefix") == 0;

	if (!ci->str)
		return Enoarg;
	if (prefix) {
		char *e = strconcat(NULL, ci->str, "\xFF\xFF\xFF\xFF");
		key_add_range(map, ci->str, e, ci->comm2);
		free(e);
	} else if (ci->str2)
		key_add_range(map, ci->str, ci->str2, ci->comm2);
	else
		key_add(map, ci->str, ci->comm2);
	return 1;
}

DEF_CMD(global_get_command)
{
	struct ed_info *ei = &ci->home->data;
	struct map *map = ei->map;
	struct command *cm;

	if (!ci->str ||
	    !(cm = key_lookup_cmd(map, ci->str)))
		return Efail;
	return comm_call(ci->comm2, "callback:comm", ci->focus,
			 0, NULL, ci->str,
			 0, NULL, NULL, 0,0, cm);
}

DEF_CMD(global_config_dir)
{
	const char *var = ci->str;
	char *dir; // ci->str2;
	char *key, *val = NULL;
	struct pane *p = ci->home;
	char *end;

	/* var might be different in different directories.
	 * Config setting are attributes stored on root that
	 * look like "config:var:dir".
	 * We find the best and return that with the dir
	 */
	if (!var || !ci->str2 || !ci->comm2)
		return Enoarg;
	key = strconcat(p, "config:", var, ":", ci->str2);
	dir = key + 7 + strlen(var) + 1;
	end = dir + strlen(dir);
	while (!val && end > dir) {
		end[0] = 0;
		val = attr_find(p->attrs, key);
		if (end[-1] == '/') {
			while (end > dir && end[-1] == '/')
				end -= 1;
		} else {
			while (end > dir && end[-1] != '/')
				end -= 1;
		}
	}
	if (!val)
		return Efalse;
	comm_call(ci->comm2, "cb", ci->focus, 0, NULL, val,
			  0, NULL, dir);
	return 1;
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
	struct ed_info *ei = &ci->home->data;
	struct map *map = ei->map;
	const char *name = ci->str;
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
			char *v = dlsym(h, "edlib_version");
			LOG("Loading %s - version %s", name, v ?: "not provided");
			s(ci->home);
			if (path)
				*path = NULL;
			return 1;
		}
	} else {
		char *err = dlerror();
		if (strstr(err, "No such file or directory") == NULL)
			LOG("dlopen %s failed %s", buf, err);
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
	if (key_lookup_prefix(map, ci) > 0)
		return 1;
	LOG("Failed to load module: %s", name);
	return Efail;
}

DEF_CMD(editor_auto_event)
{
	/* Event handlers register under a private name so we
	 * have to use key_lookup_prefix to find them.
	 * If nothing is found, autoload lib-libevent (hack?)
	 */
	struct ed_info *ei = &ci->home->data;
	struct map *map = ei->map;
	int ret = key_lookup_prefix(map, ci);

	if (ret)
		return ret;
	if (strcmp(ci->key, "event:refresh") == 0)
		/* pointless to autoload for refresh */
		return Efallthrough;
	call("attach-libevent", ci->home);
	return key_lookup_prefix(map, ci);
}

DEF_CMD(editor_activate_display)
{
	/* Given a display attached to the root, integrate it
	 * into a full initial stack of panes.
	 */
	struct pane *disp = ci->focus;
	struct pane *p, *p2;
	char *ip = attr_find(ci->home->attrs, "editor-initial-panes");
	char *save, *t, *m;

	if (!ip)
		return Efail;
	ip = strsave(ci->home, ip);
	p = pane_root(ci->focus);

	p2 = call_ret(pane, "attach-window-core", p);
	if (!p2)
		return Efail;
	p = p2;

	for (t = strtok_r(ip, " \t\n", &save);
	     t;
	     t = strtok_r(NULL, " \t\n", &save)) {
		if (!*t)
			continue;
		if (strcmp(t, "DISPLAY") == 0) {
			if (disp) {
				pane_reparent(disp, p);
				p = disp;
				disp = NULL;
			}
			continue;
		}
		m = strconcat(NULL, "attach-", t);
		p2 = call_ret(pane, m, p);
		free(m);
		if (p2)
			p = p2;
	}
	comm_call(ci->comm2, "cb", p);
	return 1;
}

DEF_CMD(editor_multicall)
{
	struct ed_info *ei = &ci->home->data;
	struct map *map = ei->map;
	int ret;
	const char *key = ci->key;

	((struct cmd_info*)ci)->key = ksuffix(ci, "global-multicall-");
	ret = key_lookup_prefix(map, ci);
	((struct cmd_info*)ci)->key = key;
	return ret;
}

DEF_CMD(editor_request_notify)
{
	pane_add_notify(ci->focus, ci->home, ksuffix(ci, "editor:request:"));
	return 1;
}

DEF_CMD(editor_send_notify)
{
	/* editor:notify:... */
	return home_pane_notify(ci->home, ksuffix(ci, "editor:notify:"),
				ci->focus,
				ci->num, ci->mark, ci->str,
				ci->num2, ci->mark2, ci->str2, ci->comm2);
}

DEF_CMD(editor_free_panes)
{
	struct ed_info *ei = &ci->home->data;

	while (ei->freelist) {
		struct pane *p = ei->freelist;
		ei->freelist = p->focus;
		p->focus = NULL;

		p->damaged &= ~DAMAGED_DEAD;
		pane_call(p, "Free", p);
		command_put(p->handle);
		p->handle = NULL;
		attr_free(&p->attrs);
		pane_put(p);
	}
	return 1;
}

DEF_CMD(editor_free_marks)
{
	struct ed_info *ei = &ci->home->data;

	while (ei->mark_free_list) {
		struct mark *m = ei->mark_free_list;
		ei->mark_free_list = (struct mark*)m->all.next;
		do_mark_free(m);
	}

	return 1;
}

DEF_CMD(editor_free_store)
{
	struct ed_info *ei = &ci->home->data;

	while (ei->store) {
		struct store *s = ei->store;
		ei->store = s->next;
		free(s);
	}
	return 1;
}

DEF_EXTERN_CMD(edlib_do_free)
{
	if (ci->home->data_size)
		unalloc_buf_safe(ci->home->_data, ci->home->data_size, pane);
	return 1;
}

/* FIXME I should be able to remove things from a keymap, not
 * replace with this.
 */
DEF_EXTERN_CMD(edlib_noop)
{
	return Efallthrough;
}

DEF_CMD(editor_close)
{
	struct ed_info *ei = &ci->home->data;
	stat_free();
	free(ei->here); ei->here = NULL;
	free(ei->data_path); ei->data_path = NULL;
	free(ei->config_path); ei->config_path = NULL;
	return Efallthrough;
}

void * safe memsave(struct pane *p safe, const char *buf, int len)
{
	struct ed_info *ei;

	p = pane_root(p);
	ei = &p->data;
	ASSERT(ei->magic==ED_MAGIC);
	if (!ei->store)
		call_comm("event:on-idle", p, &editor_free_store, 2);
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

char *strsave(struct pane *p safe, const char *buf)
{
	if (!buf)
		return NULL;
	return memsave(p, buf, strlen(buf)+1);
}

char *strnsave(struct pane *p safe, const char *buf, int len)
{
	char *s;
	if (!buf)
		return NULL;
	s = memsave(p, buf, len+1);
	if (s)
		s[len] = 0;
	return s;
}

char * safe do_strconcat(struct pane *p, const char *s1 safe, ...)
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

	if (p)
		ret = memsave(p, NULL, len+1);
	else
		ret = malloc(len+1);
	strcpy(ret, s1);
	va_start(ap, s1);
	while ((s = va_arg(ap, char*)) != NULL)
		strcat(ret, s);
	va_end(ap);
	return ret;
}

void editor_delayed_free(struct pane *ed safe, struct pane *p safe)
{
	struct ed_info *ei = &ed->data;
	if (!ei) {
		p->damaged &= ~DAMAGED_DEAD;
		pane_call(p, "Free", p);
		command_put(p->handle);
		p->handle = NULL;
		attr_free(&p->attrs);
		pane_free(p);
		return;
	}
	ASSERT(ei->magic==ED_MAGIC);
	if (!ei->freelist)
		call_comm("event:on-idle", ed, &editor_free_panes, 2);
	p->focus = ei->freelist;
	ei->freelist = p;
}

void editor_delayed_mark_free(struct mark *m safe)
{
	struct pane *ed = pane_root(m->owner);
	struct ed_info *ei = &ed->data;

	ASSERT(ei->magic==ED_MAGIC);
	if (!ei->mark_free_list)
		call_comm("event:on-idle", ed, &editor_free_marks, 2);
	m->all.next = (void*)ei->mark_free_list;
	ei->mark_free_list = m;
}

static char *set_here(struct pane *p safe)
{
	struct ed_info *ei = &p->data;
	Dl_info info;

	if (ei->here)
		;
	else if (dladdr(&set_here, &info) == 0)
		ei->here = strdup("");
	else {
		char *sl;
		ei->here = strdup(info.dli_fname ?: "");
		sl = strrchr(ei->here, '/');
		if (sl)
			*sl = 0;
	}
	return ei->here;
}

static char *set_data_path(struct pane *p safe)
{
	struct ed_info *ei = &p->data;
	char *dh, *dd, *here;
	struct buf b;

	if (ei->data_path)
		return ei->data_path;

	buf_init(&b);
	dh = getenv("XDG_DATA_HOME");
	if (!dh) {
		char *h = getenv("HOME");
		if (h)
			dh = strconcat(p, h, "/.local/share");
	}
	if (dh && *dh == '/') {
		buf_concat(&b, dh);
		buf_concat(&b, "/edlib/");
		buf_append_byte(&b, 0);
	}

	here = set_here(p);
	if (here && *here == '/') {
		buf_concat(&b, here);
		buf_concat(&b, "/edlib/");
		buf_append_byte(&b, 0);
	}

	dd = getenv("XDG_DATA_DIRS");
	if (!dd)
		dd = "/usr/local/share:/usr/share";
	while (*dd) {
		char *c = strchrnul(dd, ':');
		if (*dd == '/') {
			buf_concat_len(&b, dd, c-dd);
			buf_concat(&b, "/edlib/");
			buf_append_byte(&b, 0);
		}
		if (*c)
			c++;
		dd = c;
	}
	if (b.len)
		ei->data_path = buf_final(&b);
	else
		free(buf_final(&b));
	return ei->data_path;
}

static char *set_config_path(struct pane *p safe)
{
	struct ed_info *ei = &p->data;
	char *ch, *cd, *here;
	struct buf b;

	if (ei->config_path)
		return ei->config_path;

	buf_init(&b);
	ch = getenv("XDG_CONFIG_HOME");
	if (!ch) {
		char *h = getenv("HOME");
		if (h)
			ch = strconcat(p, h, "/.config");
	}
	if (ch && *ch == '/') {
		buf_concat(&b, ch);
		buf_concat(&b, "/edlib/");
		buf_append_byte(&b, 0);
	}

	here = set_here(p);
	if (here && *here == '/') {
		buf_concat(&b, here);
		buf_concat(&b, "/edlib/");
		buf_append_byte(&b, 0);
	}

	cd = getenv("XDG_CONFIG_DIRS");
	if (!cd)
		cd = "/etc/xdg";
	while (*cd) {
		char *c = strchrnul(cd, ':');
		if (*cd == '/') {
			buf_concat_len(&b, cd, c-cd);
			buf_concat(&b, "/edlib/");
			buf_append_byte(&b, 0);
		}
		if (*c)
			c++;
		cd = c;
	}
	if (b.len)
		ei->config_path = buf_final(&b);
	else
		free(buf_final(&b));
	return ei->config_path;
}

static char *set_bin_path(struct pane *p safe)
{
	struct ed_info *ei = &p->data;
	char *bd, *here;
	struct buf b;

	if (ei->bin_path)
		return ei->bin_path;

	buf_init(&b);
	here = set_here(p);
	if (here && *here == '/') {
		buf_concat(&b, here);
		if (b.len > 4 &&
		    strncmp(b.b + b.len-4, "/lib", 4) == 0)
			b.len -= 3;
		else
			buf_concat(&b, "/../");
		buf_concat(&b, "bin/");
		buf_append_byte(&b, 0);
	}
	bd = getenv("PATH");
	if (!bd)
		bd = "/usr/bin:/usr/local/bin";
	while (*bd) {
		char *c = strchrnul(bd, ':');
		if (*bd == '/') {
			buf_concat_len(&b, bd, c-bd);
			buf_append_byte(&b, 0);
		}
		if (*c)
			c++;
		bd = c;
	}
	if (b.len)
		ei->bin_path = buf_final(&b);
	else
		free(buf_final(&b));
	return ei->bin_path;
}

DEF_CMD(global_find_file)
{
	/*
	 * ->str is a file basename.  If it contains {COMM}, that will
	 * be replaced with the "command-name" attr from root, or
	 * "edlib" if nothing can be found.
	 * ->str2 is one of "data", "config", "bin"
	 * We find a file with basename in a known location following
	 * the XDG Base Directory Specificaton.
	 * https://specifications.freedesktop.org/basedir-spec/basedir-spec-latest.html
	 * but also look in the directory containing this library $HERE
	 * For "data" we look in a directory "edlib" under:
	 * - $XDG_DATA_HOME, or $HOME/.local/share
	 * - $HERE
	 * - $XDG_DATA_DIRS, or /usr/local/share:/usr/share
	 *
	 * For config we look in a "edlib" under:
	 * - $XDG_CONFIG_HOME, or $HOME/.config
	 * - $HERE
	 * - $XDG_CONFIG_DIRS, or /etc/xdg
	 *
	 * For bin we look in $HERE/../bin and $PATH
	 */
	char *path = NULL;
	const char *base[2] = {ci->str, NULL};
	int i;
	char *cn;

	if (base[0] == NULL || ci->str2 == NULL)
		return -Enoarg;
	if (strcmp(ci->str2, "data") == 0)
		path = set_data_path(ci->home);
	else if (strcmp(ci->str2, "config") == 0)
		path = set_config_path(ci->home);
	else if (strcmp(ci->str2, "bin") == 0)
		path = set_bin_path(ci->home);

	if (!path)
		return Einval;
	cn = strstr(base[0], "{COMM}");
	if (cn) {
		char *p = strndup(base[0], cn - base[0]);
		char *comm = attr_find(ci->home->attrs, "command-name");
		if (!comm)
			comm = "edlib";
		base[0] = strconcat(ci->home, p, comm, cn+6);
		if (strcmp(comm, "edlib") != 0)
			base[1] = strconcat(ci->home, p, "edlib", cn+6);
	}
	for (i = 0; i < 2 && base[i] ; i++) {
		char *pth;
		for (pth = path; pth && *pth; pth += strlen(pth)+1) {
			char *p = strconcat(NULL, pth, base[i]);
			int fd;
			if (!p)
				continue;
			fd = open(p, O_RDONLY);
			if (fd < 0) {
				free(p);
				continue;
			}
			close(fd);
			comm_call(ci->comm2, "cb", ci->focus, 0, NULL, p);
			free(p);
			return 1;
		}
	}
	return Efalse;
}

struct pane *editor_new(const char *comm_name)
{
	struct pane *ed;
	struct ed_info *ei;

	if (! (void*) ed_map) {
		ed_map = key_alloc();
		key_add(ed_map, "global-set-attr", &global_set_attr);
		key_add(ed_map, "global-set-command", &global_set_command);
		key_add(ed_map, "global-set-command-prefix", &global_set_command);
		key_add(ed_map, "global-get-command", &global_get_command);
		key_add(ed_map, "global-load-module", &editor_load_module);
		key_add(ed_map, "global-config-dir", &global_config_dir);
		key_add(ed_map, "xdg-find-edlib-file", &global_find_file);
		key_add_prefix(ed_map, "event:", &editor_auto_event);
		key_add_prefix(ed_map, "global-multicall-", &editor_multicall);
		key_add_prefix(ed_map, "editor:request:",
			       &editor_request_notify);
		key_add_prefix(ed_map, "editor:notify:",
			       &editor_send_notify);
		key_add(ed_map, "editor:activate-display",
			&editor_activate_display);
		key_add(ed_map, "Close", &editor_close);
	}
	ed = pane_register_root(&ed_handle.c, NULL, sizeof(*ei));
	if (!ed)
		return NULL;
	ei = &ed->data;
	ei->magic = ED_MAGIC;
	attr_set_str(&ed->attrs, "command-name", comm_name ?: "edlib");
	ei->testing = (getenv("EDLIB_TESTING") != NULL);
	ei->map = key_alloc();
	key_add_chain(ei->map, ed_map);
	ei->cmd = ed_handle;
	ei->cmd.m = &ei->map;
	/* This allows the pane to see registered commands */
	pane_update_handle(ed, &ei->cmd.c);

	doc_setup(ed);
	log_setup(ed);
	window_setup(ed);

	return ed;
}
