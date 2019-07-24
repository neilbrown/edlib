/*
 * Copyright Neil Brown ©2015-2019 <neil@brown.name>
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
 *         otherwise remove whatever was last entered, which
 *         must be multiple chars if C-w was used.
 *   C-w - collect word from target and add to search string
 *   C-c - collect char from target and add to search string.
 *   C-r - search backwards.. tricky.
 *   M-c - toggle case sensitivity (currently invisible)
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
		short wrapped;
		short case_sensitive;
	} *s;
	struct mark *start safe; /* where searching starts */
	struct mark *end safe; /* where last success ended */
	struct pane *target safe;
	short matched;
	short wrapped;
	short backwards;
	short case_sensitive;
};

static struct map *es_map;
static const char must_quote[] = ".|*+?{}()?^$\\[]";


DEF_CMD(search_forward)
{
	struct es_info *esi = ci->home->data;
	struct stk *s;
	char *str;
	int backward = ci->key[6] == 'R';

	esi->backwards = backward;
	if (esi->s && mark_same(esi->s->m, esi->end)) {
		if (esi->s->case_sensitive == esi->case_sensitive)
			/* already pushed and didn't find anything new */
			return 1;
		esi->s->case_sensitive = esi->case_sensitive;
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
			return Einval;
	}
	s = calloc(1, sizeof(*s));
	s->m = esi->start;
	s->len = strlen(str);
	s->wrapped = esi->wrapped;
	s->case_sensitive = esi->case_sensitive;
	free(str);
	s->next = esi->s;
	esi->s = s;
	if (esi->matched)
		esi->start = mark_dup(esi->end);
	else {
		esi->start = mark_dup(s->m);
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
	char *attr;
	struct mark *mk;

	if (esi->s == NULL)
		goto just_delete;
	str = call_ret(str, "doc:get-str", ci->focus);
	if (!str)
		return Einval;
	if (strlen(str) > esi->s->len) {
		free(str);
		goto just_delete;
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

 just_delete:
	if (call("doc:step", ci->focus, 1) != CHAR_RET(WEOF))
		/* Not at end-of-buffer, just delete one char */
		return Efallthrough;

	mk = call_ret(mark, "doc:point", ci->focus);
	if (!mk)
		return Efallthrough;
	mk = mark_dup(mk);
	do {
		if (call("doc:step", ci->focus, 0, mk, NULL, 1) == CHAR_RET(WEOF))
			break;
		attr = call_ret(strsave, "doc:get-attr", ci->focus, 0, mk, "auto");
	} while (attr && strcmp(attr, "1") == 0);

	call("Replace", ci->focus, 1, mk);
	mark_free(mk);
	return 1;
}

DEF_CMD(search_add)
{
	struct es_info *esi = ci->home->data;
	wint_t wch;
	char b[5];
	mbstate_t ps = {};
	int l;
	struct mark *m;
	int limit = 1000;
	char *attr = NULL;
	struct mark *addpos = mark_dup(esi->end);
	char *str = call_ret(strsave, "doc:get-str", ci->home);

	if (!str)
		return 1;

	if (esi->backwards)
		/* Move to end of match */
		call("text-search", esi->target, !esi->case_sensitive, addpos, str);
	m = mark_dup(addpos);
	if (strcmp(ci->key, "C-Chr-W")==0)
		call("Move-Word", esi->target, 1, m);
	else
		call("Move-Char", esi->target, 1, m);

	while (esi->matched
	       && addpos->seq < m->seq && !mark_same(addpos, m)) {
		int slash = 0;
		if (limit-- <= 0)
			break;
		wch = mark_next_pane(esi->target, addpos);
		if (wch == WEOF)
			break;
		l = wcrtomb(b, wch, &ps);
		if (wch == '\n') {
			slash = 1;
			strcpy(b, "n");
			l = 1;
		} else if (strchr(must_quote, wch)) {
			slash = 1;
		}
		b[l] = 0;
		if (slash) {
			call("Replace", ci->focus, 1, NULL, "\\", 0, NULL, attr);
			attr = ",auto=1";
		}
		call("Replace", ci->focus, 1, NULL, b, 0, NULL, attr);
		attr = ",auto=1";
	}
	return 1;
}

DEF_CMD(search_insert_quoted)
{
	if (strchr(must_quote, ci->key[4]) == NULL)
		return 0;
	call("Replace", ci->focus, 1, NULL, "\\");
	call("Replace", ci->focus, 1, NULL, ci->key + 4,
	     0, NULL, ",auto=1");
	return 1;
}

#include <stdio.h>
DEF_CMD(search_insert_meta)
{
	/* Insert a regexp meta char.
	 * If it is 'open', insert the 'close' too.
	 * If it is 'close', skip over a close instead if possible
	 */
	char *bracket;
	const char *brackets = "{}()[]";
	if (strchr(must_quote, ci->key[6]) == NULL || !ci->mark)
		return 0;
	bracket = strchr(brackets, ci->key[6]);
	if (!bracket) {
		call("Replace", ci->focus, 1, NULL, ci->key + 6);
	} else if ((bracket - brackets) % 2) {
		/* Close bracket */
		if (doc_following_pane(ci->focus, ci->mark) == (wint_t)ci->key[6])
			call("Move-Char", ci->focus, 1);
		else
			call("Replace", ci->focus, 1, NULL, ci->key + 6);
	} else {
		/* Open bracket */
		char b[3];
		strncpy(b, bracket, 2);
		b[2] = 0;
		call("Replace", ci->focus, 1, NULL, b);
		call("Move-Char", ci->focus, -1);
	}
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
	esi->matched = 0;
	m = mark_dup(esi->start);
	str = call_ret(str, "doc:get-str", ci->home);
	if (str == NULL || strlen(str) == 0)
		/* empty string always matches */
		ret = 1;
	else if (esi->backwards && mark_prev_pane(esi->target, m) == WEOF)
		ret = -2;
	else {
		if (mark_same(esi->start, esi->end))
			mark_step_pane(esi->target, m, !esi->backwards, 1);
		ret = call("text-search", esi->target,
		           !esi->case_sensitive, m, str, esi->backwards);
	}
	if (ret == 0)
		pfx = "Search (unavailable): ";
	else if (ret == Efail) {
		call("search:highlight", esi->target, 0, m, str);
		pfx = "Failed Search: ";
	} else if (ret == Einval) {
		pfx = "Search (incomplete): ";
	} else if (ret < 0) {
		pfx = "Search (sys-error): ";
	} else {
		int len = --ret;
		mark_to_mark(esi->end, m);
		if (!esi->backwards)
			while (ret > 0 && mark_prev_pane(esi->target, m) != WEOF)
				ret -= 1;
		call("search:highlight", esi->target, len, m, str,
		     !esi->case_sensitive);
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
	struct mark *mk;

	/* More to last location, found */
	call("Move-to", esi->target, 1);
	mk = call_ret(mark2, "doc:point", esi->target);
	if (mk)
		attr_set_int(&mk->attrs, "emacs:active", 0);
	call("Move-to", esi->target, 0, esi->end);

	call("popup:close", safe_cast ci->focus->parent, 0, NULL, str);
	free(str);
	return 1;
}

DEF_CMD(search_clip)
{
	struct es_info *esi = ci->home->data;
	struct stk *s;

	mark_clip(esi->start, ci->mark, ci->mark2);
	mark_clip(esi->end, ci->mark, ci->mark2);
	for (s = esi->s; s; s = s->next)
		mark_clip(s->m, ci->mark, ci->mark2);
	return Efallthrough;
}

DEF_CMD(search_recentre)
{
	/* Send this command through to target, at current location */
	struct es_info *esi = ci->home->data;

	return call(ci->key, esi->target, ci->num, esi->end, NULL,
	            ci->num2);
}

DEF_CMD(search_toggle_ci)
{
	struct es_info *esi = ci->home->data;

	/* If not at end of doc, fall through */
	if (ci->mark && doc_following_pane(ci->focus, ci->mark) != WEOF)
		return 0;
	esi->case_sensitive = !esi->case_sensitive;
	call("Call:Notify:doc:Replace", ci->focus);
	attr_set_str(&ci->home->attrs, "status-line",
	             esi->case_sensitive ? " Search: case sensitive " :
	             " Search: case insensitive ");
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
	key_add(es_map, "Enter", &search_done);
	key_add(es_map, "Notify:doc:Replace", &search_again);
	key_add(es_map, "Notify:clip", &search_clip);
	key_add(es_map, "C-Chr-L", &search_recentre);
	key_add_range(es_map, "Chr- ", "Chr-~", &search_insert_quoted);
	key_add_range(es_map, "M-Chr- ", "M-Chr-~", &search_insert_meta);
	key_add(es_map, "M-Chr-c", &search_toggle_ci);
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
		return Esys;
	esi = calloc(1, sizeof(*esi));
	esi->target = p;
	m = mark_at_point(p, NULL, MARK_POINT);
	if (!m) {
		free(esi);
		return Esys;
	}
	esi->end = m;

	esi->start = mark_dup(m);
	esi->s = NULL;
	esi->matched = 1;
	esi->wrapped = 0;
	esi->backwards = ci->num;

	p = pane_register(ci->focus, 0, &search_handle.c, esi, NULL);
	if (p) {
		call("Request:Notify:doc:Replace", p);
		attr_set_str(&p->attrs, "status-line", " Search: case insensitive ");
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
