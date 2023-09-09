/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Minor mode for emacs incremental search.
 *
 * emacs search attaches emacs-search-highlight to the document stack,
 * then adds a popup search box and attaches emacs-search over it.
 * We send a popup-get-target message to collect the target pane.
 * We have a stack of "string,pos" for repeated search requests.
 * We capture "Replace" to repeat search.
 * We send "Move-View-Pos" to target to get it to refresh to a new location.
 * We capture:
 *   :C-S - if we have a match, save end of match as new start
 *   :Backspace - is search string is same as saved start, pop
 *         otherwise remove whatever was last entered, which
 *         must be multiple chars if :Cw was used.
 *   :C-W - collect word from target and add to search string
 *   :C-C - collect char from target and add to search string.
 *   :C-R - search backwards.. tricky.
 *   :A-c - toggle case sensitivity (currently invisible)
 *
 */

#include <stdlib.h>
#include <string.h>
#define PANE_DATA_TYPE struct es_info
#define PANE_DATA_TYPE_2 struct highlight_info
#include "core.h"
#include "rexel.h"

struct es_info {
	struct stk {
		struct stk *next;
		struct mark *m safe; /* Start of search */
		unsigned int len; /* current length of search string */
		short wrapped;
		short case_sensitive;
		short backwards;
	} *s;
	struct mark *start safe; /* where searching starts */
	struct mark *end safe; /* where last success ended */
	struct pane *target;
	struct pane *replace_pane;
	short matched;
	short wrapped;
	short backwards;
	short case_sensitive;
	short replaced;
};

struct highlight_info {
	int view, replace_view;
	char *patn;
	int ci;
	struct mark *start, *end, *match, *rpos, *oldpoint;
	struct pane *popup, *replace_popup;
};
#include "core-pane.h"

static struct map *es_map, *er_map;
DEF_LOOKUP_CMD(search_handle, es_map);
DEF_LOOKUP_CMD(replace_handle, er_map);
static const char must_quote[] = ".|*+?{()?^$\\[";

DEF_CMD(search_forward)
{
	struct es_info *esi = ci->home->data;
	struct stk *s;
	char *str;
	struct mark *newstart;
	const char *suffix;

	if (!esi->target)
		return Efail;

	suffix = ksuffix(ci, "K:C-");
	if (suffix[0])
		esi->backwards = suffix[0] == 'R';

	if (esi->s && mark_same(esi->s->m, esi->end)) {
		if (esi->s->case_sensitive == esi->case_sensitive &&
		    esi->s->backwards == esi->backwards)
			/* already pushed and didn't find anything new */
			return 1;
		esi->s->case_sensitive = esi->case_sensitive;
		esi->s->backwards = esi->backwards;
	}
	str = call_ret(str, "doc:get-str", ci->focus);
	if (!str || !*str) {
		char *ss;
		ss = call_ret(strsave, "history:get-last", ci->focus);
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
	s->backwards = -1;
	free(str);
	s->next = esi->s;
	esi->s = s;
	newstart = NULL;
	if (esi->matched) {
		newstart = mark_dup(esi->end);
		if (esi->matched == 1)
			/* zero length match */
			if (doc_move(esi->target, newstart,
				     esi->backwards ? -1 : 1) == WEOF) {
				mark_free(newstart);
				newstart = NULL;
			}
	}
	if (!newstart) {
		newstart = mark_dup(s->m);
		esi->wrapped = 1;
		call("doc:file", esi->target, esi->backwards ? 1 : -1,
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
	if (doc_following(ci->focus, NULL) != WEOF)
		/* Not at end-of-buffer, just delete one char */
		return Efallthrough;

	mk = call_ret(mark, "doc:point", ci->focus);
	if (!mk)
		return Efallthrough;
	mk = mark_dup(mk);
	do {
		if (doc_prev(ci->focus, mk) == WEOF)
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
	struct mark *m;
	int limit = 1000;
	char *attr = NULL;
	struct mark *addpos = mark_dup(esi->end);
	char *str = call_ret(strsave, "doc:get-str", ci->home);
	int first = 1;

	if (!str)
		return 1;
	if (!esi->target)
		return Efail;

	if (esi->backwards)
		/* Move to end of match */
		call("text-search", esi->target,
		     !esi->case_sensitive, addpos, str);
	m = mark_dup(addpos);
	if (strcmp(ci->key, "K:C-W")==0)
		call("doc:word", esi->target, 1, m);
	else
		call("Move-Char", esi->target, 1, m);

	/* Move cursor to end of search string */
	call("doc:file", ci->focus, 1);
	while (esi->matched
	       && mark_ordered_not_same(addpos, m)) {
		int slash = 0;
		if (limit-- <= 0)
			break;
		wch = doc_next(esi->target, addpos);
		if (wch == WEOF)
			break;
		put_utf8(b, wch);
		if (wch == '\n') {
			slash = 1;
			strcpy(b, "n");
		} else if (strchr(must_quote, wch)) {
			slash = 1;
		}
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
	const char *suffix = ksuffix(ci, "doc:char-");
	char *patn;
	if (strchr(must_quote, suffix[0]) == NULL)
		return Efallthrough;
	patn = call_ret(strsave, "doc:get-str", ci->focus);
	if (patn) {
		char *open = strrchr(patn, '[');
		if (open &&
		    (open == patn || open[-1] != '\\') &&
		     (open[1] == 0 || strchr(open+2, ']') == NULL))
			/* There is an '[' that hasn't been closed, so don't
			 * quote anything.
			 */
			return Efallthrough;
	}
	call("Replace", ci->focus, 1, NULL, "\\");
	call("Replace", ci->focus, 1, NULL, suffix,
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
	const char *k = ksuffix(ci, "K:A-");

	if (strchr(must_quote, *k) == NULL || !ci->mark)
		return Efallthrough;
	bracket = strchr(brackets, *k);
	if (!bracket) {
		call("Replace", ci->focus, 1, NULL, k);
	} else if ((bracket - brackets) % 2) {
		/* Close bracket */
		if (doc_following(ci->focus, ci->mark) == (wint_t)k[0])
			call("Move-Char", ci->focus, 1);
		else
			call("Replace", ci->focus, 1, NULL, k);
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

	if (esi->target)
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

	if (!esi->target)
		return Efail;

	call("search:highlight", esi->target);
	esi->matched = 0;
	esi->replaced = 0;
	m = mark_dup(esi->start);
	str = call_ret(str, "doc:get-str", ci->home);
	if (str == NULL || strlen(str) == 0)
		/* empty string always matches */
		ret = 1;
	else if (esi->backwards && doc_prev(esi->target, m) == WEOF)
		ret = Efail;
	else {
		ret = call("text-search", esi->target,
			   !esi->case_sensitive, m, str, esi->backwards);
	}
	if (ret == 0)
		pfx = "Search (unavailable): ";
	else if (ret == Efail) {
		call("search:highlight", esi->target, 0,NULL, str,
		     !esi->case_sensitive);
		pfx = "Failed Search: ";
	} else if (ret == Einval) {
		pfx = "Search (incomplete): ";
	} else if (ret < 0) {
		pfx = "Search (sys-error): ";
	} else {
		int len = --ret;
		mark_to_mark(esi->end, m);
		if (esi->backwards) {
			while (ret > 0 && doc_next(esi->target, m) != WEOF)
				ret -= 1;
			call("search:highlight", esi->target, len, esi->end, str,
			     !esi->case_sensitive, m);
		} else {
			while (ret > 0 && doc_prev(esi->target, m) != WEOF)
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

	if (!esi->target)
		return Efail;

	if (esi->replace_pane && strcmp(ci->key, "K:Enter") == 0) {
		/* if there is a replace pane, switch to it instead of closing */
		pane_take_focus(esi->replace_pane);
		return 1;
	}
	str = call_ret(str, "doc:get-str", ci->focus);
	/* Move "mark" to last location, found */
	call("Move-to", esi->target, 1);
	mk = call_ret(mark2, "doc:point", esi->target);
	if (mk)
		attr_set_int(&mk->attrs, "emacs:active", 0);
	call("Move-to", esi->target, 0, esi->end, NULL, 1);

	call("popup:close", safe_cast ci->focus->parent, 0, NULL, str);
	free(str);
	return 1;
}

DEF_CMD(search_escape)
{
	return call("search:done", ci->focus);
}

DEF_CMD(search_clip)
{
	struct es_info *esi = ci->home->data;
	struct stk *s;

	mark_clip(esi->start, ci->mark, ci->mark2, !!ci->num);
	mark_clip(esi->end, ci->mark, ci->mark2, !!ci->num);
	for (s = esi->s; s; s = s->next)
		mark_clip(s->m, ci->mark, ci->mark2, !!ci->num);
	return Efallthrough;
}

DEF_CMD(search_recentre)
{
	/* Send this command through to target, at current location */
	struct es_info *esi = ci->home->data;

	if (!esi->target)
		return Efail;
	return call(ci->key, esi->target, ci->num, esi->end, NULL,
		    ci->num2);
}

DEF_CMD(search_toggle_ci)
{
	struct es_info *esi = ci->home->data;

	/* If not at end of doc, fall through */
	if (ci->mark && doc_following(ci->focus, ci->mark) != WEOF)
		return Efallthrough;
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
		pane_take_focus(esi->replace_pane);
		return 1;
	}

	if (!esi->target)
		return Efail;

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
	p = call_ret(pane, "attach-history", p, 0, NULL, "*Replace History*");
	esi->replace_pane = p;
	if (p) {
		pane_add_notify(ci->home, p, "Notify:Close");
		home_call(esi->target, "highlight:set-popup", p, 1);
	}
	if (strcmp(ci->key, "K:A-%") == 0)
		pane_take_focus(ci->focus);
	else
		pane_take_focus(p);
	return 1;
}

DEF_CMD(search_notify_close)
{
	struct es_info *esi = ci->home->data;

	if (ci->focus == esi->replace_pane)
		esi->replace_pane = NULL;
	if (ci->focus == esi->target) {
		esi->target = NULL;
		//pane_close(ci->home);
	}
	return 1;
}

DEF_CMD(do_replace)
{
	struct es_info *esi = ci->home->data;
	const char *new = ci->str;
	struct mark *m;
	int len = esi->matched - 1;

	if (!new)
		return Enoarg;
	if (len < 0)
		return Efail;
	if (esi->replaced)
		return 1;
	if (!esi->target)
		return Efail;
	esi->replaced = 1;
	m = mark_dup(esi->end);
	if (esi->backwards) {
		while (len > 0 && doc_next(esi->target, m) != WEOF)
			len -= 1;
		mark_step(m, 0);
		if (call("doc:replace", esi->target, 0, esi->end, new, 0, m) > 0) {
			call("search:highlight-replace", esi->target,
			     strlen(new), esi->end, NULL, 0, m);
			return 1;
		}
	} else {
		while (len > 0 && doc_prev(esi->target, m) != WEOF)
			len -= 1;
		mark_step(m, 1);
		if (strchr(new, '\\')) {
			char *Pattern = call_ret(strsave, "doc:get-str", ci->home);
			struct command *ptn = call_ret(comm, "make-search",
						       ci->home,
						       RXLF_ANCHORED | RXLF_BACKTRACK,
						       NULL, Pattern);
			if (ptn) {
				char *new2;
				call_comm("doc:content", esi->target, ptn, 0, m);
				new2 = comm_call_ret(strsave, ptn, "interp",
						     esi->target, 0, NULL, new);
				if (new2)
					new = new2;
				command_put(ptn);
			}
		}
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
	struct pane *sp = ci->home->_data;
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
	struct pane *sp = ci->home->_data;
	char *new;

	new = call_ret(str, "doc:get-str", ci->focus);
	if (call("search:replace", sp, 0, NULL, new) > 0)
		call("history:save", ci->focus, 0, NULL, new);
	free(new);
	return 1;
}

DEF_CMD(replace_all)
{
	struct pane *sp = ci->home->_data;
	char *new;
	int replaced = 0;

	new = call_ret(str, "doc:get-str", ci->focus);
	pane_set_time(ci->home);
	while (call("search:replace", sp, 0, NULL, new) > 0 &&
	       call("search:again", sp) > 0 &&
	       !pane_too_long(ci->home, 2000))
		replaced = 1;
	if (replaced)
		call("history:save", ci->focus, 0, NULL, new);
	free(new);

	return 1;
}

DEF_CMD(replace_to_search)
{
	struct pane *sp = ci->home->_data;

	pane_take_focus(sp);
	return 1;
}

DEF_CMD(replace_forward)
{
	struct pane *sp = ci->home->_data;

	call(ci->key, sp);

	return 1;
}

DEF_CMD(replace_undo)
{
	return Efallthrough;
}

DEF_CMD(replace_escape)
{
	struct pane *sp = ci->home->_data;

	return call("search:done", sp);
}

DEF_CMD(replace_prev)
{
	struct pane *home = ci->home->_data;
	struct es_info *esi = home->data;

	if (esi->target)
		call("search:step-replace", esi->target, -1);
	return 1;
}

DEF_CMD(replace_next)
{
	struct pane *home = ci->home->_data;
	struct es_info *esi = home->data;

	if (esi->target)
		call("search:step-replace", esi->target, 1);
	return 1;
}

static void emacs_search_init_map(void)
{
	/* Keys for the 'search' pane */
	es_map = key_alloc();
	key_add(es_map, "K:C-S", &search_forward);
	key_add(es_map, "search:again", &search_forward);
	key_add(es_map, "K:Backspace", &search_retreat);
	key_add(es_map, "K:C-W", &search_add);
	key_add(es_map, "K:C-C", &search_add);
	key_add(es_map, "K:C-R", &search_forward);
	key_add(es_map, "Close", &search_close);
	key_add(es_map, "K:Enter", &search_done);
	key_add(es_map, "search:done", &search_done);
	key_add(es_map, "doc:replaced", &search_again);
	key_add(es_map, "Notify:clip", &search_clip);
	key_add(es_map, "K:C-L", &search_recentre);
	key_add_range(es_map, "doc:char- ", "doc:char-~", &search_insert_quoted);
	key_add_range(es_map, "K:A- ", "K:A-~", &search_insert_meta);
	key_add(es_map, "K:A-c", &search_toggle_ci);
	key_add(es_map, "K:A-r", &search_replace);
	key_add(es_map, "K:S:Tab", &search_replace);
	key_add(es_map, "K:A-%", &search_replace);
	key_add(es_map, "Cancel", &search_escape);

	key_add(es_map, "search:replace", &do_replace);
	key_add(es_map, "Notify:Close", &search_notify_close);

	/* keys for the 'replace' pane */
	er_map = key_alloc();
	key_add(er_map, "K:Enter", &replace_request_next);
	key_add(er_map, "K:A:Enter", &replace_request);
	key_add(er_map, "K:S:Tab", &replace_to_search);
	key_add(er_map, "K:A-!", &replace_all);
	key_add(er_map, "K:C-S", &replace_forward);
	key_add(er_map, "K:C-R", &replace_forward);
	key_add(er_map, "K:C-L", &replace_forward);
	key_add(er_map, "Cancel", &replace_escape);
	key_add(er_map, "K:Up", &replace_prev);
	key_add(er_map, "K:Down", &replace_next);
	key_add(er_map, "doc:reundo", &replace_undo);
}

DEF_CMD(emacs_search)
{
	struct pane *p, *target;
	struct es_info *esi;
	struct mark *m;

	if (!es_map)
		emacs_search_init_map();
	target = call_ret(pane, "popup:get-target", ci->focus);
	if (!target)
		return Efail;

	m = mark_at_point(target, NULL, MARK_POINT);
	if (!m)
		return Efail;

	p = pane_register(ci->focus, 0, &search_handle.c);
	if (!p)
		return Efail;
	esi = p->data;
	esi->target = target;
	esi->end = m;

	esi->start = mark_dup(m);
	esi->s = NULL;
	esi->matched = 1;
	esi->wrapped = 0;
	esi->replaced = 0;
	esi->backwards = ci->num & 1;

	call("doc:request:doc:replaced", p);
	attr_set_str(&p->attrs, "status-line", " Search: case insensitive ");
	comm_call(ci->comm2, "callback:attach", p);
	pane_add_notify(p, esi->target, "Notify:Close");

	if (ci->num & 2)
		call("K:A-%", p);

	return 1;
}

static void do_searches(struct pane *p safe,
			struct pane *owner safe, int view, char *patn,
			int ci,
			struct mark *m, struct mark *end)
{
	int ret;
	struct highlight_info *hi = owner->data2;
	struct mark *start;

	if (!m)
		return;
	m = mark_dup(m);
	start = mark_dup(m);
	while ((ret = call("text-search", p, ci, m, patn, 0, end)) >= 1) {
		struct mark *m2, *m3;
		int len = ret - 1;
		m2 = vmark_new(p, view, owner);
		if (!m2)
			break;
		mark_to_mark(m2, m);
		while (ret > 1 && doc_prev(p, m2) != WEOF)
			ret -= 1;
		m3 = vmark_matching(m2);
		if (m3) {
			mark_free(m2);
			m2 = m3;
		}
		if (attr_find(m2->attrs, "render:search") == NULL) {
			bool match = hi->match && mark_same(hi->match, m2);
			attr_set_int(&m2->attrs,
				     match ? "render:search" : "render:search2",
				     len);
			call("view:changed", p, 0, m2, NULL, 0, m);
			m2 = vmark_new(p, view, owner);
			if (m2) {
				mark_to_mark(m2, m);
				attr_set_int(&m2->attrs,
					     match ? "render:search-end"
					     : "render:search2-end",
					     0);
			}
		}

		if (len == 0 || mark_ordered_or_same(m, start))
			/* Need to move forward, or we'll just match here again*/
			doc_next(p, m);
		mark_free(start);
		start = mark_dup(m);
	}
	mark_free(start);
	mark_free(m);
}

static void queue_highlight_refresh(struct pane *p safe);

DEF_CMD(emacs_search_highlight)
{
	/* from 'mark' for 'num' chars to 'mark2' there is a match for 'str',
	 * or else there are no matches (num==0).
	 * Here we remove any existing highlighting and highlight
	 * just the match.  A subsequent call to emacs_search_reposition
	 * will highlight other near-by matches.
	 */
	struct mark *m, *start;
	struct highlight_info *hi = ci->home->data2;

	if (hi->view < 0)
		return Efail;

	while ((start = vmark_first(ci->focus, hi->view, ci->home)) != NULL)
		mark_free(start);

	free(hi->patn);
	if (ci->str)
		hi->patn = strdup(ci->str);
	else
		hi->patn = NULL;
	hi->ci = ci->num2;
	mark_free(hi->match);
	hi->match = NULL;

	if (hi->oldpoint) {
		call("Move-to", ci->focus, 0, hi->oldpoint);
		mark_free(hi->rpos);
		mark_free(hi->oldpoint);
		hi->rpos = NULL;
		hi->oldpoint = NULL;
	}

	if (ci->mark && ci->num >= 0 && ci->str) {
		m = vmark_new(ci->focus, hi->view, ci->home);
		if (!m)
			return Efail;
		mark_to_mark(m, ci->mark);
		attr_set_int(&m->attrs, "render:search", ci->num);
		call("Move-View-Pos", ci->focus, 0, m);
		hi->match = mark_dup(ci->mark);
		if (ci->mark2 &&
		    (m = vmark_new(ci->focus, hi->view, ci->home)) != NULL) {
			mark_to_mark(m, ci->mark2);
			attr_set_int(&m->attrs, "render:search-end", 0);
		}
	} else if (ci->str) {
		/* No destination to move to, so just refresh whatever
		 * is visible
		 */
		queue_highlight_refresh(ci->focus);
	}
	call("view:changed", ci->focus);
	call("render:request:reposition", ci->focus);
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
	struct highlight_info *hi = ci->home->data2;

	if (hi->replace_view < 0 || !hi->replace_popup)
		return Efail;

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
	struct highlight_info *hi = ci->home->data2;

	if (!ci->str)
		return Efallthrough;
	if (!hi->popup)
		return Efallthrough;

	if (strcmp(ci->str, "render:search") == 0) {
		/* Current search match -  "220" is a priority */
		if (hi->view >= 0 && ci->mark && ci->mark->viewnum == hi->view) {
			int len = atoi(ci->str2) ?: 1;
			return comm_call(ci->comm2, "attr:callback", ci->focus, len,
					 ci->mark, "fg:red,inverse,focus,vis-nl", 220);
		}
	}
	if (strcmp(ci->str, "render:search2") == 0) {
		/* alternate matches in current view */
		if (hi->view >= 0 && ci->mark && ci->mark->viewnum == hi->view) {
			int len = atoi(ci->str2) ?: 1;
			return comm_call(ci->comm2, "attr:callback", ci->focus, len,
					 ci->mark, "fg:blue,inverse,vis-nl", 220);
		}
	}
	if (strcmp(ci->str, "render:replacement") == 0) {
		/* Replacement -  "220" is a priority */
		if (hi->replace_view >= 0 && ci->mark &&
		    ci->mark->viewnum == hi->replace_view) {
			int len = atoi(ci->str2) ?: 1;
			return comm_call(ci->comm2, "attr:callback", ci->focus, len,
					 ci->mark, "fg:green-40,inverse,vis-nl", 220);
		}
	}
	if (strcmp(ci->str, "start-of-line") == 0 && ci->mark && hi->view >= 0) {
		struct mark *m = vmark_at_or_before(ci->focus, ci->mark, hi->view, ci->home);
		if (m && mark_same(m, ci->mark))
			m = NULL;
		if (m && attr_find_int(m->attrs, "render:search") > 0)
			return comm_call(ci->comm2, "attr:callback", ci->focus, 0,
					 ci->mark, "fg:red,inverse,vis-nl", 220);
		if (m && attr_find_int(m->attrs, "render:search2") > 0)
			return comm_call(ci->comm2, "attr:callback", ci->focus, 0,
					 ci->mark, "fg:blue,inverse,vis-nl", 220);
	}
	if (strcmp(ci->str, "render:search-end") ==0) {
		/* Here endeth the match */
		return comm_call(ci->comm2, "attr:callback", ci->focus, -1,
				 ci->mark, "fg:red,inverse,vis-nl", 220);
	}
	if (strcmp(ci->str, "render:search2-end") ==0) {
		/* Here endeth the match */
		return comm_call(ci->comm2, "attr:callback", ci->focus, -1,
				 ci->mark, "fg:blue,inverse,vis-nl", 220);
	}
	if (strcmp(ci->str, "render:replacement-end") ==0) {
		/* Here endeth the replacement */
		return comm_call(ci->comm2, "attr:callback", ci->focus, -1,
				 ci->mark, "fg:green-40,inverse,vis-nl", 220);
	}
	return Efallthrough;
}

DEF_CMD(highlight_draw)
{
	struct highlight_info *hi = ci->home->data2;
	struct pane *pp = hi->popup;
	struct pane *pp2 = hi->replace_popup;
	struct xy xy;

	if (!ci->str2 || !strstr(ci->str2, ",focus") || !pp)
		return Efallthrough;

	/* here is where the user will be looking, make sure
	 * the popup doesn't obscure it.
	 */

	xy = pane_mapxy(ci->focus, ci->home, ci->x, ci->y, False);
	while (pp->parent != pp && pp->z == 0)
		pp = pp->parent;
	while (pp2 && pp2->parent != pp2 && pp2->z == 0)
		pp2 = pp2->parent;
	if (pp->x == 0) {
		/* currently TL, should we move it back */
		if (xy.x < pp->w ||
		    (xy.y > pp->h &&
		     (!pp2 || xy.y > pp2->y + pp2->h)))
			call("popup:style", hi->popup, 0, NULL, "TR2");
	} else {
		/* currently TR, should we move it out of way */
		if (xy.x >= pp->x &&
		    (xy.y <= pp->h ||
		     (pp2 && xy.y <= pp2->y + pp2->h)))
			call("popup:style", hi->popup, 0, NULL, "TL2");
	}
	return Efallthrough;
}

DEF_CMD(emacs_search_reposition_delayed)
{
	struct highlight_info *hi = ci->home->data2;
	struct mark *start = hi->start;
	struct mark *end = hi->end;
	struct mark *vstart, *vend;
	char *patn = hi->patn;

	if (!start || !end)
		return Efalse;

	vstart = vmark_first(ci->focus, hi->view, ci->home);
	vend = vmark_last(ci->focus, hi->view, ci->home);
	if (vstart == NULL || start->seq < vstart->seq) {
		/* search from 'start' to first match or 'end' */
		do_searches(ci->focus, ci->home, hi->view, patn, hi->ci,
			    start, vstart ?: end);
		if (vend)
			do_searches(ci->focus, ci->home, hi->view, patn, hi->ci,
				    vend, end);
	} else if (vend && end->seq > vend->seq) {
		/* search from last match to end */
		do_searches(ci->focus, ci->home, hi->view, patn, hi->ci,
			    vend, end);
	}
	return Efalse;
}

static void queue_highlight_refresh(struct pane *p safe)
{
	call_comm("event:free", p, &emacs_search_reposition_delayed);
	call_comm("event:timer", p, &emacs_search_reposition_delayed,
		  edlib_testing(p) ? 50 : 500);
}

DEF_CMD(emacs_search_reposition)
{
	/*
	 * Delete any matches that are no longer visible.
	 * Then record new end-points and schedule an update shortly
	 * to find any matches in the new range.  If there are multiple
	 * calls to this in quick successes (e.g. when scrolling), the
	 * delayed update won't happen until a suitable time after the last
	 * reposition.
	 */
	struct highlight_info *hi = ci->home->data2;
	struct mark *start = ci->mark;
	struct mark *end = ci->mark2;
	struct mark *m;

	if (hi->view < 0 || hi->patn == NULL || !start || !end || !hi->popup)
		return Efallthrough;

	while ((m = vmark_first(ci->focus, hi->view, ci->home)) != NULL &&
	       mark_ordered_not_same(m, start))
		mark_free(m);

	while ((m = vmark_last(ci->focus, hi->view, ci->home)) != NULL &&
	       mark_ordered_not_same(end, m))
		mark_free(m);

	mark_free(hi->start);
	mark_free(hi->end);
	hi->start = mark_dup(start);
	hi->end = mark_dup(end);

	queue_highlight_refresh(ci->home);
	return Efallthrough;
}

DEF_CMD(emacs_highlight_close)
{
	/* ci->focus is being closed */
	struct highlight_info *hi = ci->home->data2;

	free(hi->patn);
	mark_free(hi->start);
	mark_free(hi->end);
	mark_free(hi->match);
	mark_free(hi->rpos);
	mark_free(hi->oldpoint);
	hi->start = NULL;
	hi->end = NULL;
	hi->match = NULL;
	hi->rpos = NULL;
	hi->oldpoint = NULL;
	return 1;
}

static void free_marks(struct pane *home safe)
{
	struct highlight_info *hi = home->data2;
	struct mark *m;

	while ((m = vmark_first(home, hi->view, home)) != NULL)
		mark_free(m);
	while ((m = vmark_first(home, hi->replace_view, home)) != NULL)
		mark_free(m);
}

DEF_CMD(emacs_search_done)
{
	struct highlight_info *hi = ci->home->data2;

	if (ci->str && ci->str[0])
		call("history:save", ci->focus, 0, NULL, ci->str);

	if (hi->oldpoint) {
		call("Move-to", ci->focus, 0, hi->oldpoint);
		mark_free(hi->rpos);
		mark_free(hi->oldpoint);
		hi->rpos = NULL;
		hi->oldpoint = NULL;
	}

	hi->popup = NULL;
	hi->replace_popup = NULL;
	free_marks(ci->home);
	return 1;
}

DEF_CMD(emacs_highlight_abort)
{
	struct highlight_info *hi = ci->home->data2;
	struct pane *p;

	p = hi->replace_popup;
	hi->replace_popup = NULL;
	if (p)
		call("popup:close", p, 0, NULL, "");
	p = hi->popup;
	hi->popup = NULL;
	if (p)
		call("popup:close", p, 0, NULL, "");
	free_marks(ci->home);

	return Efallthrough;
}

DEF_CMD(emacs_highlight_clip)
{
	struct highlight_info *hi = ci->home->data2;

	marks_clip(ci->home, ci->mark, ci->mark2,
		   hi->view, ci->home, !!ci->num);
	marks_clip(ci->home, ci->mark, ci->mark2,
		   hi->replace_view, ci->home, !!ci->num);
	return Efallthrough;
}

DEF_CMD(emacs_highlight_set_popup)
{
	struct highlight_info *hi = ci->home->data2;

	if (ci->num)
		hi->replace_popup = ci->focus;
	else
		hi->popup = ci->focus;
	pane_add_notify(ci->home, ci->focus, "Notify:Close");
	return 1;
}

DEF_CMD(emacs_highlight_close_notify)
{
	struct highlight_info *hi = ci->home->data2;

	if (ci->focus == hi->replace_popup)
		hi->replace_popup = NULL;
	if (ci->focus == hi->popup)
		hi->popup = NULL;
	return 1;
}

DEF_CMD(emacs_highlight_reattach)
{
	comm_call(ci->comm2, "cb", ci->home);
	return 1;
}

DEF_CMD(emacs_step_replace)
{
	struct highlight_info *hi = ci->home->data2;
	struct mark *m;

	if (!hi->replace_view || !hi->match)
		return Einval;
	if (ci->num > 0 && !hi->rpos)
		return 1;
	if (!hi->rpos) {
		if (!hi->oldpoint) {
			m = call_ret(mark, "doc:point", ci->home);
			if (m)
				hi->oldpoint = mark_dup(m);
		}
		hi->rpos = mark_dup(hi->match);
	}

	if (ci->num > 0) {
		m = vmark_at_or_before(ci->home, hi->rpos, hi->replace_view, ci->home);
		if (m)
			m = vmark_next(m);
		while (m && attr_find_int(m->attrs, "render:replacement") < 0)
			m = vmark_next(m);
	} else {
		doc_prev(ci->home, hi->rpos);
		m = vmark_at_or_before(ci->home, hi->rpos, hi->replace_view, ci->home);
		while (m && attr_find_int(m->attrs, "render:replacement") < 0)
			m = vmark_prev(m);
	}
	if (m) {
		mark_to_mark(hi->rpos, m);
		call("Move-View-Pos", ci->home, 0, m);
		call("Move-to", ci->home, 0, m);
	}
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
	key_add(m, "search:step-replace", &emacs_step_replace);
	key_add(m, "map-attr", &emacs_hl_attrs);
	key_add(m, "Draw:text", &highlight_draw);
	key_add(m, "Close", &emacs_highlight_close);
	key_add(m, "Abort", &emacs_highlight_abort);
	key_add(m, "Notify:clip", &emacs_highlight_clip);
	key_add(m, "highlight:set-popup", &emacs_highlight_set_popup);
	key_add(m, "attach-emacs-search-highlight", &emacs_highlight_reattach);
	key_add(m, "Notify:Close", &emacs_highlight_close_notify);
	hl_map = m;
}

DEF_CMD(emacs_search_attach_highlight)
{
	struct highlight_info *hi;
	struct pane *p;

	if (!hl_map)
		emacs_highlight_init_map();

	p = pane_register_2(ci->focus, 0, &highlight_handle.c);
	if (!p)
		return Efail;
	hi = p->data2;

	hi->view = home_call(ci->focus, "doc:add-view", p) - 1;
	hi->replace_view = home_call(ci->focus, "doc:add-view", p) - 1;
	comm_call(ci->comm2, "callback:attach", p);

	return 1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &emacs_search,
		  0, NULL, "attach-emacs-search");
	call_comm("global-set-command", ed, &emacs_search_attach_highlight,
		  0, NULL, "attach-emacs-search-highlight");
}
