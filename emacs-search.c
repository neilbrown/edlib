/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Minor mode for emacs incremental search.
 *
 * emacs-search creates a popup search box and the attaches this mode.
 * We send a popup-get-target message to collect the target pane.
 * We have a stack of "string,pos" for repeated search requests.
 * We capture "Replace" to repeat search.
 * We send "Move-View-Pos" to target to get it to refresh to a new location.
 * We capture:
 *   C-s - if we have a match, save end of match as new start
 *   Backspace - is search string is same as saved start, pop
 *   C-w - collect word from target and add to search string
 *   C-c - collect char from target and add to search string.
 *   C-r - search backwards.. tricky.
 *
 */

#include <stdlib.h>
#include <string.h>
#include "core.h"

struct es_info {
	struct stk {
		struct stk *next;
		struct mark *m safe; /* Start of search */
		unsigned int len; /* current length of search string */
		int wrapped;
	} *s;
	struct mark *start safe; /* where searching starts */
	struct mark *end safe; /* where last success ended */
	struct pane *target safe;
	short matched;
	short wrapped;
	short backwards;
};

static struct map *es_map;

DEF_CMD(search_forward)
{
	struct es_info *esi = ci->home->data;
	struct stk *s;
	char *str;
	int backward = ci->key[6] == 'R';

	esi->backwards = backward;
	if (esi->s && mark_same_pane(esi->target, esi->s->m, esi->end)) {
		/* already pushed and didn't find anything new */
		return 1;
	}
	str = call_ret(str, "doc:get-str", ci->focus);
	if (!str || !*str) {
		/* re-use old string; Is there any point to this indirection? */
		char *ss;
		ss = pane_attr_get(ci->focus, "done-key");
		if (ss)
			ss = pane_attr_get(ci->focus, ss);
		if (ss) {
			call("Replace", ci->home, 1, NULL, ss, 1);
			return 1;
		}
		if (!str)
			return -1;
	}
	s = calloc(1, sizeof(*s));
	s->m = esi->start;
	s->len = strlen(str);
	s->wrapped = esi->wrapped;
	free(str);
	s->next = esi->s;
	esi->s = s;
	if (esi->matched)
		esi->start = mark_dup(esi->end, 1);
	else {
		esi->start = mark_dup(s->m, 1);
		esi->wrapped = 1;
		call("Move-File", esi->target, backward ? 1 : -1, esi->start);
	}
	/* Trigger notification so isearch watcher searches again */
	call("Replace", ci->home, 1, NULL, "", 1);
	return 1;
}

DEF_CMD(search_retreat)
{
	struct es_info *esi = ci->home->data;
	char *str;
	struct stk *s;

	if (esi->s == NULL)
		return 0;
	str = call_ret(str, "doc:get-str", ci->focus);
	if (!str)
		return -1;
	if (strlen(str) > esi->s->len) {
		free(str);
		return 0;
	}
	free(str);
	s = esi->s;
	esi->s = s->next;
	mark_free(esi->start);
	esi->start = s->m;
	esi->wrapped = s->wrapped;
	free(s);
	/* Trigger notification so isearch watcher searches again */
	call("Replace", ci->home, 1, NULL, "", 1);
	return 1;
}

DEF_CMD(search_add)
{
	struct es_info *esi = ci->home->data;
	wint_t wch;
	char b[5];
	mbstate_t ps = {};
	int l;

	do {
		wch = mark_next_pane(esi->target, esi->end);
		if (wch == WEOF)
			return 1;
		if (wch == '\n') {
			/* ugly hack */
			/* Sending this will cause a call-back to
			 * close everything down.
			 */
			mark_prev_pane(esi->target, esi->end);
			return 1;
		}
		/* FIXME utf-8! and quote regexp chars */
		if (strchr("|*+?{}()?^$\\", wch)) {
			b[0] = '\\';
			l = wcrtomb(b+1, wch, &ps) + 1;
		} else
			l = wcrtomb(b, wch, &ps);
		b[l] = 0;
		call("Replace", ci->focus, 1, NULL, b);
	} while (strcmp(ci->key, "C-Chr-C") != 0 && wch != ' ');
	return 1;
}

DEF_CMD(search_close)
{
	struct es_info *esi = ci->home->data;

	call("search:highlight", esi->target);
	mark_free(esi->end);
	esi->end = safe_cast NULL;
	mark_free(esi->start);
	while (esi->s) {
		struct stk *n = esi->s;
		esi->s = n->next;
		mark_free(n->m);
		free(n);
	}
	return 1;
}

DEF_CMD(search_again)
{
	/* document has changed, retry search */
	struct es_info *esi = ci->home->data;
	struct pane *p;
	char *a, *pfx;
	int ret;
	struct mark *m;
	char *str;

	call("search:highlight", esi->target);
	m = mark_dup(esi->start, 1);
	str = call_ret(str, "doc:get-str", ci->home);
	if (esi->backwards && mark_prev_pane(esi->target, m) == WEOF)
		ret = -2;
	else
		ret = call("text-search", esi->target, 0, m, str, esi->backwards);
	if (ret == 0)
		pfx = "Search (unavailable): ";
	else if (ret == -2) {
		esi->matched = 0;
		pfx = "Failed Search: ";
	} else if (ret < 0) {
		pfx = "Search (incomplete): ";
	} else {
		int len = --ret;
		point_to_mark(esi->end, m);
		if (!esi->backwards)
			while (ret > 0 && mark_prev_pane(esi->target, m) != WEOF)
				ret -= 1;
		call("search:highlight", esi->target, len, m, str);
		esi->matched = 1;
		pfx = esi->backwards ? "Reverse Search: ":"Search: ";
		if (esi->wrapped)
			pfx = esi->backwards ? "Wrapped Reverse Search: ":"Wrapped Search: ";
	}
	/* HACK */
	for (p = ci->home; p; p = p->parent) {
		a = attr_find(p->attrs, "prefix");
		if (!a)
			continue;
		if (strcmp(a, pfx) != 0)
			attr_set_str(&p->attrs, "prefix", pfx);
	}
	if (m)
		mark_free(m);
	free(str);
	return 1;
}

DEF_CMD(search_done)
{
	/* need to advance the target view to 'start', leaving
	 * mark at point
	 */
	struct es_info *esi = ci->home->data;
	char *str = call_ret(str, "doc:get-str", ci->focus);

	if (esi->matched) {
		struct mark *mk;
		call("Move-to", esi->target, 1);
		mk = call_ret(mark2, "doc:point", esi->target);
		if (mk)
			attr_set_int(&mk->attrs, "emacs:active", 0);
		call("Move-to", esi->target, 0, esi->end);
	}
	call("popup:close", safe_cast ci->focus->parent, 0, NULL, str);
	free(str);
	return 1;
}

static void emacs_search_init_map(void)
{
	es_map = key_alloc();
	key_add(es_map, "C-Chr-S", &search_forward);
	key_add(es_map, "Backspace", &search_retreat);
	key_add(es_map, "C-Chr-W", &search_add);
	key_add(es_map, "C-Chr-C", &search_add);
	key_add(es_map, "C-Chr-R", &search_forward);
	key_add(es_map, "Close", &search_close);
	key_add(es_map, "Return", &search_done);
	key_add(es_map, "Notify:doc:Replace", &search_again);
}

DEF_LOOKUP_CMD(search_handle, es_map);

DEF_CMD(emacs_search)
{
	struct pane *p;
	struct es_info *esi;
	struct mark *m;

	if (!es_map)
		emacs_search_init_map();
	p = call_pane("popup:get-target", ci->focus);
	if (!p)
		return -1;
	esi = calloc(1, sizeof(*esi));
	esi->target = p;
	m = mark_at_point(p, NULL, MARK_POINT);
	if (!m) {
		free(esi);
		return -1;
	}
	esi->end = m;

	esi->start = mark_dup(m, 1);
	esi->s = NULL;
	esi->matched = 0;
	esi->wrapped = 0;
	esi->backwards = ci->num;

	p = pane_register(ci->focus, 0, &search_handle.c, esi, NULL);
	if (p) {
		call("Request:Notify:doc:Replace", p);
		comm_call(ci->comm2, "callback:attach", p);
	}
	return 1;
}

/* Pre-declare to silence sparse - for now */
void emacs_search_init(struct pane *ed safe);
void emacs_search_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &emacs_search, 0, NULL, "attach-emacs-search");
}
