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
	struct pane *replace_pane;
	short matched;
	short wrapped;
	short backwards;
	short case_sensitive;
};

static struct map *es_map, *er_map;
DEF_LOOKUP_CMD(search_handle, es_map);
DEF_LOOKUP_CMD(replace_handle, er_map);
static const char must_quote[] = ".|*+?{()?^$\\[";
static const char may_quote[] = "<>dDsSwWpPaA";


DEF_CMD(search_forward)
{
	struct es_info *esi = ci->home->data;
	struct stk *s;
	char *str;
	struct mark *newstart;

	if (strncmp(ci->key, "C-Chr-", 6) == 0)
		esi->backwards = ci->key[6] == 'R';

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
			call("Replace", ci->home, 1, NULL, ss);
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
	newstart = NULL;
	if (esi->matched) {
		newstart = mark_dup(esi->end);
		if (esi->matched == 1)
			/* zero length match */
			if (mark_step_pane(esi->target, newstart,
					   !esi->backwards, 1) == WEOF) {
				mark_free(newstart);
				newstart = NULL;
			}
	}
	if (!newstart) {
		newstart = mark_dup(s->m);
		esi->wrapped = 1;
		call("Move-File", esi->target, esi->backwards ? 1 : -1,
		     newstart);
	}
	esi->start = newstart;
	/* Trigger notification so isearch watcher searches again */
	call("Replace", ci->home, 1, NULL, "");

	if (!esi->matched && strcmp(ci->key, "search:again") == 0)
		return Efail;
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
	call("Replace", ci->home, 1, NULL, "");
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
	int first = 1;

	if (!str)
		return 1;

	if (esi->backwards)
		/* Move to end of match */
		call("text-search", esi->target,
		     !esi->case_sensitive, addpos, str);
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
			call("Replace", ci->focus, 1, NULL, "\\",
			     !first, NULL, attr);
			attr = ",auto=1";
			first = 0;
		}
		call("Replace", ci->focus, 1, NULL, b,
		     !first, NULL, attr);
		first = 0;
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
	     1, NULL, ",auto=1");
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
	if (strchr(may_quote, ci->key[6])) {
		call("Replace", ci->focus, 1, NULL, "\\");
		call("Replace", ci->focus, 1, NULL, ci->key+6,
		     1, NULL, ",auto=1");
		return 1;
	}
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
	free(esi);
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
		ret = call("text-search", esi->target,
			   !esi->case_sensitive, m, str, esi->backwards);
	}
	if (ret == 0)
		pfx = "Search (unavailable): ";
	else if (ret == Efail) {
		call("search:highlight", esi->target);
		pfx = "Failed Search: ";
	} else if (ret == Einval) {
		pfx = "Search (incomplete): ";
	} else if (ret < 0) {
		pfx = "Search (sys-error): ";
	} else {
		int len = --ret;
		mark_to_mark(esi->end, m);
		if (esi->backwards) {
			while (ret > 0 && mark_next_pane(esi->target, m) != WEOF)
				ret -= 1;
			call("search:highlight", esi->target, len, esi->end, str,
			     !esi->case_sensitive, m);
		} else {
			while (ret > 0 && mark_prev_pane(esi->target, m) != WEOF)
				ret -= 1;
			call("search:highlight", esi->target, len, m, str,
			     !esi->case_sensitive, esi->end);
		}
		esi->matched = len + 1;
		pfx = esi->backwards ? "Reverse Search: ":"Search: ";
		if (esi->wrapped)
			pfx = esi->backwards ? "Wrapped Reverse Search: ":"Wrapped Search: ";
	}
	/* HACK */
	for (p = ci->home; p != p->parent; p = p->parent) {
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
	char *str;
	struct mark *mk;

	if (esi->replace_pane && strcmp(ci->key, "Enter") == 0) {
		/* if there is a replace pane, switch to it instead of closing */
		pane_focus(esi->replace_pane);
		return 1;
	}
	str = call_ret(str, "doc:get-str", ci->focus);
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
	call("doc:notify:doc:replaced", ci->focus);
	attr_set_str(&ci->home->attrs, "status-line",
		     esi->case_sensitive ? " Search: case sensitive " :
		     " Search: case insensitive ");
	return 1;
}

DEF_CMD(search_replace)
{
	struct pane *p;
	struct es_info *esi = ci->home->data;

	if (esi->replace_pane) {
		pane_focus(esi->replace_pane);
		return 1;
	}

	p = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "P", 0, NULL,
		     "");
	if (!p)
		return Efail;
	attr_set_str(&p->attrs, "prompt", "Replacement");
	attr_set_str(&p->attrs, "status-line", " Replacement ");
	call("doc:set-name", p, 0, NULL, "Replacement");

	p = pane_register(p, 0, &replace_handle.c, ci->focus);
	if (!p)
		return Efail;
	p = call_ret(pane, "attach-history", p, 0, NULL, "*Replace History*",
		     0, NULL, "popup:close");
	esi->replace_pane = p;
	if (p)
		home_call(esi->target, "highlight:set-popup", p, 1);
	if (ci->key[6] == '%')
		pane_focus(ci->focus);
	else
		pane_focus(p);
	return 1;
}

DEF_CMD(do_replace)
{
	struct es_info *esi = ci->home->data;
	char *new = ci->str;
	struct mark *m;
	int len = esi->matched - 1;

	if (!new)
		return Enoarg;
	if (esi->matched <= 0)
		return Efail;
	m = mark_dup(esi->end);
	if (esi->backwards) {
		while (len > 0 && mark_next_pane(esi->target, m) != WEOF)
			len -= 1;
		mark_make_first(m);
		if (call("doc:replace", esi->target, 0, esi->end, new, 0, m) > 0) {
			call("search:highlight-replace", esi->target,
			     strlen(new), esi->end, NULL, 0, m);
			return 1;
		}
	} else {
		while (len > 0 && mark_prev_pane(esi->target, m) != WEOF)
			len -= 1;
		mark_make_last(m);
		if (call("doc:replace", esi->target, 0, m, new, 0, esi->end) > 0) {
			call("search:highlight-replace", esi->target,
			     strlen(new), m, NULL, 0, esi->end);
			return 1;
		}
	}
	return Efail;
}

DEF_CMD(replace_request_next)
{
	struct pane *sp = ci->home->data;
	char *new;

	new = call_ret(str, "doc:get-str", ci->focus);
	if (call("search:replace", sp, 0, NULL, new) > 0) {
		call("history:save", ci->focus, 0, NULL, new);
		call("search:again", sp);
	} else {
		call("search:done", sp);
	}
	return 1;
}

DEF_CMD(replace_request)
{
	struct pane *sp = ci->home->data;
	char *new;

	new = call_ret(str, "doc:get-str", ci->focus);
	if (call("search:replace", sp, 0, NULL, new) > 0)
		call("history:save", ci->focus, 0, NULL, new);
	free(new);
	return 1;
}

DEF_CMD(replace_all)
{
	struct pane *sp = ci->home->data;
	char *new;
	int replaced = 0;

	new = call_ret(str, "doc:get-str", ci->focus);
	while (call("search:replace", sp, 0, NULL, new) > 0 &&
	       call("search:again", sp) > 0)
		replaced = 1;
	if (replaced)
		call("history:save", ci->focus, 0, NULL, new);
	free(new);

	return 1;
}

DEF_CMD(replace_to_search)
{
	struct pane *sp = ci->home->data;

	pane_focus(sp);
	return 1;
}

DEF_CMD(replace_forward)
{
	struct pane *sp = ci->home->data;

	call(ci->key, sp);

	return 1;
}

DEF_CMD(replace_undo)
{
	return 0;
}

static void emacs_search_init_map(void)
{
	/* Keys for the 'search' pane */
	es_map = key_alloc();
	key_add(es_map, "C-Chr-S", &search_forward);
	key_add(es_map, "search:again", &search_forward);
	key_add(es_map, "Backspace", &search_retreat);
	key_add(es_map, "C-Chr-W", &search_add);
	key_add(es_map, "C-Chr-C", &search_add);
	key_add(es_map, "C-Chr-R", &search_forward);
	key_add(es_map, "Close", &search_close);
	key_add(es_map, "Enter", &search_done);
	key_add(es_map, "search:done", &search_done);
	key_add(es_map, "doc:replaced", &search_again);
	key_add(es_map, "Notify:clip", &search_clip);
	key_add(es_map, "C-Chr-L", &search_recentre);
	key_add_range(es_map, "Chr- ", "Chr-~", &search_insert_quoted);
	key_add_range(es_map, "M-Chr- ", "M-Chr-~", &search_insert_meta);
	key_add(es_map, "M-Chr-c", &search_toggle_ci);
	key_add(es_map, "M-Chr-r", &search_replace);
	key_add(es_map, "Tab", &search_replace);
	key_add(es_map, "M-Chr-%", &search_replace);

	key_add(es_map, "search:replace", &do_replace);

	/* keys for the 'replace' pane */
	er_map = key_alloc();
	key_add(er_map, "Enter", &replace_request_next);
	key_add(er_map, "M-Enter", &replace_request);
	key_add(er_map, "Tab", &replace_to_search);
	key_add(er_map, "S-Tab", &replace_to_search);
	key_add(er_map, "M-Chr-!", &replace_all);
	key_add(er_map, "C-Chr-S", &replace_forward);
	key_add(er_map, "C-Chr-R", &replace_forward);
	key_add(er_map, "C-Chr-L", &replace_forward);
	key_add(er_map, "doc:reundo", &replace_undo);
}

DEF_CMD(emacs_search)
{
	struct pane *p;
	struct es_info *esi;
	struct mark *m;

	if (!es_map)
		emacs_search_init_map();
	p = call_ret(pane, "popup:get-target", ci->focus);
	if (!p)
		return Efail;
	esi = calloc(1, sizeof(*esi));
	esi->target = p;
	m = mark_at_point(p, NULL, MARK_POINT);
	if (!m) {
		free(esi);
		return Efail;
	}
	esi->end = m;

	esi->start = mark_dup(m);
	esi->s = NULL;
	esi->matched = 1;
	esi->wrapped = 0;
	esi->backwards = ci->num & 1;

	p = pane_register(ci->focus, 0, &search_handle.c, esi);
	if (p) {
		call("doc:request:doc:replaced", p);
		attr_set_str(&p->attrs, "status-line", " Search: case insensitive ");
		comm_call(ci->comm2, "callback:attach", p);

		if (ci->num & 2)
			call("M-Chr-%", p);
	}
	return 1;
}

static void do_searches(struct pane *p safe,
			struct pane *owner, int view, char *patn,
			int ci,
			struct mark *m, struct mark *end)
{
	int ret;
	if (!m)
		return;
	m = mark_dup(m);
	while ((ret = call("text-search", p, ci, m, patn, 0, end)) >= 1) {
		struct mark *m2, *m3;
		int len = ret - 1;
		m2 = vmark_new(p, view, owner);
		if (!m2)
			break;
		mark_to_mark(m2, m);
		while (ret > 1 && mark_prev_pane(p, m2) != WEOF)
			ret -= 1;
		m3 = vmark_matching(m2);
		if (m3) {
			mark_free(m2);
			m2 = m3;
		}
		if (attr_find(m2->attrs, "render:search") == NULL) {
			attr_set_int(&m2->attrs, "render:search2", len);
			m2 = vmark_new(p, view, owner);
			if (m2) {
				mark_to_mark(m2, m);
				attr_set_int(&m2->attrs, "render:search2-end", 0);
			}
		}
		if (len == 0)
			/* Need to move forward, or we'll just match here again*/
			mark_next_pane(p, m);
	}
	mark_free(m);
}

struct highlight_info {
	int view, replace_view;
	char *patn;
	int ci;
	struct mark *start, *end;
	struct pane *popup, *replace_popup;
};

DEF_CMD(emacs_search_highlight)
{
	/* from 'mark' for 'num' chars to 'mark2' there is a match for 'str',
	 * or else there are no matches (num==0).
	 * Here we remove any existing highlighting and highlight
	 * just the match.  A subsequent call to emacs_search_reposition
	 * will highlight other near-by matches.
	 */
	struct mark *m, *start;
	struct highlight_info *hi = ci->home->data;

	if (hi->view < 0)
		return 0;

	while ((start = vmark_first(ci->focus, hi->view, ci->home)) != NULL)
		mark_free(start);

	free(hi->patn);
	if (ci->str)
		hi->patn = strdup(ci->str);
	else
		hi->patn = NULL;
	hi->ci = ci->num2;

	if (ci->mark && ci->num >= 0 && ci->str) {
		m = vmark_new(ci->focus, hi->view, ci->home);
		if (!m)
			return Efail;
		mark_to_mark(m, ci->mark);
		attr_set_int(&m->attrs, "render:search", ci->num);
		call("Move-View-Pos", ci->focus, 0, m);
		if (ci->mark2 &&
		    (m = vmark_new(ci->focus, hi->view, ci->home)) != NULL) {
			mark_to_mark(m, ci->mark2);
			attr_set_int(&m->attrs, "render:search-end", 0);
		}
	}
	call("view:changed", ci->focus);
	return 1;
}

DEF_CMD(emacs_replace_highlight)
{
	/* from 'mark' for 'num' chars to 'mark2' there is a recent
	 * replacement in a search/replace.
	 * The existing render:search{-end} marks which are near mark2
	 * need to be discarded, and new "render:replacement" need to
	 * be added.
	 */
	struct mark *m;
	struct highlight_info *hi = ci->home->data;

	if (hi->replace_view < 0)
		return 0;

	if (!ci->mark || !ci->mark2)
		return Enoarg;

	while ((m = vmark_at_or_before(ci->focus, ci->mark2,
				       hi->view, ci->home)) != NULL &&
	       (attr_find_int(m->attrs, "render:search") >= 0 ||
		attr_find_int(m->attrs, "render:search-end") >= 0))
		mark_free(m);
	m = vmark_new(ci->focus, hi->replace_view, ci->home);
	if (m) {
		mark_to_mark(m, ci->mark);
		attr_set_int(&m->attrs, "render:replacement", ci->num);
	}
	m = vmark_new(ci->focus, hi->replace_view, ci->home);
	if (m) {
		mark_to_mark(m, ci->mark2);
		attr_set_int(&m->attrs, "render:replacement-end", 0);
	}
	call("view:changed", ci->focus);
	return 1;
}


DEF_CMD(emacs_hl_attrs)
{
	struct highlight_info *hi = ci->home->data;

	if (!ci->str)
		return 0;

	if (strcmp(ci->str, "render:search") == 0) {
		/* Current search match -  "20" is a priority */
		if (hi->view >= 0 && ci->mark && ci->mark->viewnum == hi->view) {
			int  len = atoi(ci->str2);
			return comm_call(ci->comm2, "attr:callback", ci->focus, len,
					 ci->mark, "fg:red,inverse,focus,vis-nl", 20);
		}
	}
	if (strcmp(ci->str, "render:search2") == 0) {
		/* alternate matches in current view */
		if (hi->view >= 0 && ci->mark && ci->mark->viewnum == hi->view) {
			int len = atoi(ci->str2);
			return comm_call(ci->comm2, "attr:callback", ci->focus, len,
					 ci->mark, "fg:blue,inverse,vis-nl", 20);
		}
	}
	if (strcmp(ci->str, "render:replacement") == 0) {
		/* Replacement -  "20" is a priority */
		if (hi->replace_view >= 0 && ci->mark &&
		    ci->mark->viewnum == hi->replace_view) {
			int  len = atoi(ci->str2);
			return comm_call(ci->comm2, "attr:callback", ci->focus, len,
					 ci->mark, "fg:green-40,inverse,vis-nl", 20);
		}
	}
	if (strcmp(ci->str, "start-of-line") == 0 && ci->mark && hi->view >= 0) {
		struct mark *m = vmark_at_or_before(ci->focus, ci->mark, hi->view, ci->home);
		if (m && attr_find_int(m->attrs, "render:search") > 0)
			return comm_call(ci->comm2, "attr:callback", ci->focus, 5000,
					 ci->mark, "fg:red,inverse,vis-nl", 20);
		if (m && attr_find_int(m->attrs, "render:search2") > 0)
			return comm_call(ci->comm2, "attr:callback", ci->focus, 5000,
					 ci->mark, "fg:blue,inverse,vis-nl", 20);
	}
	if (strcmp(ci->str, "render:search-end") ==0) {
		/* Here endeth the match */
		return comm_call(ci->comm2, "attr:callback", ci->focus, -1,
				 ci->mark, "fg:red,inverse,vis-nl", 20);
	}
	if (strcmp(ci->str, "render:search2-end") ==0) {
		/* Here endeth the match */
		return comm_call(ci->comm2, "attr:callback", ci->focus, -1,
				 ci->mark, "fg:blue,inverse,vis-nl", 20);
	}
	if (strcmp(ci->str, "render:replacement-end") ==0) {
		/* Here endeth the replacement */
		return comm_call(ci->comm2, "attr:callback", ci->focus, -1,
				 ci->mark, "fg:green-40,inverse,vis-nl", 20);
	}
	return 0;
}

DEF_CMD(highlight_draw)
{
	struct highlight_info *hi = ci->home->data;
	struct pane *pp = hi->popup;
	struct pane *pp2 = hi->replace_popup;

	if (!ci->str2 || !strstr(ci->str2, ",focus") || !pp)
		return 0;

	/* here is where the user will be looking, make sure
	 * the popup doesn't obscure it.
	 */

	while (pp->parent != pp && pp->z == 0)
		pp = pp->parent;
	while (pp2 && pp2->parent != pp2 && pp2->z == 0)
		pp2 = pp2->parent;
	if (pp->x == 0) {
		/* currently TL, should we move it back */
		if (ci->x < pp->w ||
		    (ci->y > pp->h &&
		     (!pp2 || ci->y > pp2->y + pp2->h)))
			call("popup:style", hi->popup, 0, NULL, "TR2");
	} else {
		/* currently TR, should we move it out of way */
		if (ci->x >= pp->x &&
		    (ci->y <= pp->h ||
		     (pp2 && ci->y <= pp2->y + pp2->h)))
			call("popup:style", hi->popup, 0, NULL, "TL2");
	}
	return 0;
}


DEF_CMD(emacs_search_reposition_delayed)
{
	struct highlight_info *hi = ci->home->data;
	struct mark *start = hi->start;
	struct mark *end = hi->end;
	struct mark *vstart, *vend;
	char *patn = hi->patn;
	int damage = 0;

	if (!start || !end)
		return Efalse;

	vstart = vmark_first(ci->focus, hi->view, ci->home);
	vend = vmark_last(ci->focus, hi->view, ci->home);
	if (vstart == NULL || start->seq < vstart->seq) {
		/* search from 'start' to first match or 'end' */
		do_searches(ci->focus, ci->home, hi->view, patn, hi->ci, start, vstart ?: end);
		if (vend)
			do_searches(ci->focus, ci->home, hi->view, patn, hi->ci,
				    vend, end);
	} else if (vend && end->seq > vend->seq) {
		/* search from last match to end */
		do_searches(ci->focus, ci->home, hi->view, patn, hi->ci, vend, end);
	}
	if (vstart != vmark_first(ci->focus, hi->view, ci->home) ||
	    vend != vmark_last(ci->focus, hi->view, ci->home))
		damage = 1;
	if (damage) {
		pane_damaged(ci->focus, DAMAGED_CONTENT);
		pane_damaged(ci->focus, DAMAGED_VIEW);
	}
	mark_free(hi->start);
	mark_free(hi->end);
	hi->start = hi->end = NULL;
	return 0;
}

DEF_CMD(emacs_search_reposition)
{
	/* If new range and old range don't over-lap, discard
	 * old range and re-fill new range.
	 * Otherwise delete anything in range that is no longer visible.
	 * If they overlap before, search from start to first match.
	 * If they overlap after, search from last match to end.
	 */
	/* delete every match before new start and after end */
	struct highlight_info *hi = ci->home->data;
	struct mark *start = ci->mark;
	struct mark *end = ci->mark2;
	char *patn = hi->patn;
	int damage = 0;
	struct mark *m;

	if (hi->view < 0 || patn == NULL || !start || !end)
		return 0;

	while ((m = vmark_first(ci->focus, hi->view, ci->home)) != NULL &&
	       mark_ordered_not_same(m, start)) {
		mark_free(m);
		damage = 1;
	}
	while ((m = vmark_last(ci->focus, hi->view, ci->home)) != NULL &&
	       mark_ordered_not_same(end, m)) {
		mark_free(m);
		damage = 1;
	}
	mark_free(hi->start);
	mark_free(hi->end);
	hi->start = mark_dup(start);
	hi->end = mark_dup(end);

	if (damage) {
		pane_damaged(ci->focus, DAMAGED_CONTENT);
		pane_damaged(ci->focus, DAMAGED_VIEW);
	}
	call_comm("event:timer", ci->focus, &emacs_search_reposition_delayed,
		  500);
	return 1;
}

DEF_CMD(emacs_highlight_close)
{
	/* ci->focus is being closed */
	struct highlight_info *hi = ci->home->data;

	free(hi->patn);
	if (hi->view >= 0) {
		struct mark *m;

		while ((m = vmark_first(ci->focus, hi->view, ci->home)) != NULL)
			mark_free(m);
		call("doc:del-view", ci->home, hi->view);
	}
	if (hi->replace_view >= 0) {
		struct mark *m;

		while ((m = vmark_first(ci->focus, hi->replace_view,
					ci->home)) != NULL)
			mark_free(m);
		call("doc:del-view", ci->home, hi->replace_view);
	}
	mark_free(hi->start);
	mark_free(hi->end);
	free(hi);
	return 0;
}
DEF_CMD(emacs_search_done)
{
	if (ci->str && ci->str[0]) {
		call("global-set-attr", ci->focus, 0, NULL, "Search String",
		     0, NULL, ci->str);
	}
	pane_close(ci->home);
	return 1;
}

DEF_CMD(emacs_highlight_abort)
{
	pane_close(ci->home);
	return 0;
}

DEF_CMD(emacs_highlight_clip)
{
	struct highlight_info *hi = ci->home->data;

	marks_clip(ci->home, ci->mark, ci->mark2, hi->view, ci->home);
	marks_clip(ci->home, ci->mark, ci->mark2, hi->replace_view, ci->home);
	return 0;
}

DEF_CMD(emacs_highlight_set_popup)
{
	struct highlight_info *hi = ci->home->data;

	if (ci->num)
		hi->replace_popup = ci->focus;
	else
		hi->popup = ci->focus;
	return 1;
}

static struct map *hl_map;
DEF_LOOKUP_CMD(highlight_handle, hl_map);

static void emacs_highlight_init_map(void)
{
	struct map *m;

	m = key_alloc();
	key_add(m, "Search String", &emacs_search_done);
	key_add(m, "render:reposition", &emacs_search_reposition);
	key_add(m, "search:highlight", &emacs_search_highlight);
	key_add(m, "search:highlight-replace", &emacs_replace_highlight);
	key_add(m, "map-attr", &emacs_hl_attrs);
	key_add(m, "Draw:text", &highlight_draw);
	key_add(m, "Close", &emacs_highlight_close);
	key_add(m, "Abort", &emacs_highlight_abort);
	key_add(m, "Notify:clip", &emacs_highlight_clip);
	key_add(m, "highlight:set-popup", &emacs_highlight_set_popup);
	hl_map = m;
}

DEF_CMD(emacs_search_attach_highlight)
{
	struct highlight_info *hi = calloc(1, sizeof(*hi));
	struct pane *p;

	if (!hl_map)
		emacs_highlight_init_map();

	p = pane_register(ci->focus, 0, &highlight_handle.c, hi);
	if (p) {
		hi->view = home_call(ci->focus, "doc:add-view", p) - 1;
		hi->replace_view = home_call(ci->focus, "doc:add-view", p) - 1;
		comm_call(ci->comm2, "callback:attach", p);
	}
	return 1;
}


/* Pre-declare to silence sparse - for now */
void emacs_search_init(struct pane *ed safe);
void emacs_search_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &emacs_search,
		  0, NULL, "attach-emacs-search");
	call_comm("global-set-command", ed, &emacs_search_attach_highlight,
		  0, NULL, "attach-emacs-search-highlight");
}
