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
		struct mark *m; /* Start of search */
		unsigned int len; /* current length of match string */
		int wrapped;
	} *s;
	struct mark *start; /* where searching starts */
	struct mark *end; /* where last success ended */
	struct pane *target;
	short matched;
	short wrapped;
};

static struct map *es_map;

DEF_CMD(search_forward)
{
	struct es_info *esi = ci->home->data;
	struct stk *s;
	char *str;

	if (esi->s && mark_same_pane(esi->target, esi->s->m, esi->end, NULL)) {
		/* already pushed and didn't find anything new */
		return 1;
	}
	str = doc_getstr(ci->focus, NULL, NULL);
	if (!*str) {
		/* re-use old string; Is there any point to this indirection? */
		char *ss;
		ss = pane_attr_get(ci->focus, "done-key");
		if (ss)
			ss = pane_attr_get(ci->focus, ss);
		if (ss) {
			call5("Replace", ci->home, 1, NULL, ss, 1);
			return 1;
		}
	}
	s = malloc(sizeof(*s));
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
		call3("Move-File", esi->target, -1, esi->start);
	}
	/* Trigger notification so isearch watcher searches again */
	call5("Replace", ci->home, 1, NULL, "", 1);
	return 1;
}

DEF_CMD(search_retreat)
{
	struct es_info *esi = ci->home->data;
	char *str;
	struct stk *s;

	if (esi->s == NULL)
		return 0;
	str = doc_getstr(ci->focus, NULL, NULL);
	if (strlen(str) > esi->s->len) {
		free(str);
		return 0;
	}
	s = esi->s;
	esi->s = s->next;
	mark_free(esi->start);
	esi->start = s->m;
	esi->wrapped = s->wrapped;
	free(s);
	/* Trigger notification so isearch watcher searches again */
	call5("Replace", ci->home, 1, NULL, "", 1);
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
		/* TEMP HACK - please fix */
		doc_set_attr(esi->target, esi->end, "highlight", NULL);
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
		call5("Replace", ci->focus, 1, NULL, b, 0);
	} while (strcmp(ci->key, "C-Chr-C") != 0 && wch != ' ');
	return 1;
}

DEF_CMD(search_backward)
{
	return 1;
}

DEF_CMD(search_close)
{
	struct es_info *esi = ci->home->data;

	/* TEMP HACK - please fix */
	doc_set_attr(esi->target, esi->end, "highlight", NULL);
	mark_free(esi->end);
	esi->end = NULL;
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

	/* TEMP HACK - please fix */
	doc_set_attr(esi->target, esi->end, "highlight", NULL);
	m = mark_dup(esi->start, 1);
	str = doc_getstr(ci->home, NULL, NULL);
	ret = call5("text-search", esi->target, 0, m, str, 0);
	if (ret == 0)
		pfx = "Search (unavailable): ";
	else if (ret == -2) {
		esi->matched = 0;
		pfx = "Failed Search: ";
	} else if (ret < 0) {
		pfx = "Search (incomplete): ";
	} else {
		point_to_mark(esi->end, m);
		/* TEMP HACK - please fix */
		doc_set_attr(esi->target, esi->end, "highlight","fg:red,inverse");
		call3("Move-View-Pos", esi->target, 0, esi->end);
		esi->matched = 1;
		pfx = "Search: ";
		if (esi->wrapped)
			pfx = "Wrapped Search: ";
	}
	/* HACK */
	for (p = ci->home; p; p = p->parent) {
		a = attr_find(p->attrs, "prefix");
		if (!a)
			continue;
		if (strcmp(a, pfx) != 0)
			attr_set_str(&p->attrs, "prefix", pfx, -1);
	}
	mark_free(m);
	free(str);
	return 1;
}

DEF_CMD(search_done)
{
	/* need to advance the target view to 'start' */
	struct es_info *esi = ci->home->data;
	char *str = doc_getstr(ci->focus, NULL, NULL);

	call3("Move-to", esi->target, 0, esi->start);
	call5("popup:close", ci->focus->parent, 0, NULL, str, 0);
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
	key_add(es_map, "C-Chr-R", &search_backward);
	key_add(es_map, "Close", &search_close);
	key_add(es_map, "Return", &search_done);
	key_add(es_map, "Notify:Replace", &search_again);
}

DEF_LOOKUP_CMD(search_handle, es_map);

DEF_CMD(emacs_search)
{
	struct pane *p;
	struct es_info *esi;
	struct mark *m;

	if (!es_map)
		emacs_search_init_map();
	p = call_pane("popup:get-target", ci->focus, 0, NULL, 0);
	if (!p)
		return -1;
	esi = malloc(sizeof(*esi));
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

	p = pane_register(ci->focus, 0, &search_handle.c, esi, NULL);
	if (p) {
		call3("Request:Notify:Replace", p, 0, NULL);
		comm_call(ci->comm2, "callback:attach", p, 0, NULL, NULL, 0);
	}
	return 1;
}

/* Pre-declare to silence sparse - for now */
void emacs_search_init(struct pane *ed);
void emacs_search_init(struct pane *ed)
{
	call_comm("global-set-command", ed, 0, NULL, "attach-emacs-search",
		  0, &emacs_search);
}
