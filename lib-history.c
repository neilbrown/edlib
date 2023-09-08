/*
 * Copyright Neil Brown ©2016-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * history
 *
 * A history pane supports selection of lines from a separate
 * document.  The underlying document is assumed to be one line
 * and this line can be replaced by various lines from the history document.
 * When a line is replaced, if it had been modified, it is saved first so it
 * can be revisited when "down" movement gets back to the end.
 * When a selection is committed (:Enter), it is added to end of history.
 * :A-p - replace current line with previous line from history, if there is one
 * :A-n - replace current line with next line from history.  If none, restore
 *        saved line
 * :A-r - enter incremental search, looking back
 * :A-s - enter incremental search, looking forward
 *
 * In incremental search mode the current search string appears in the
 * prompt and:
 *   -glyph appends to the search string and repeats search from start
 *          in current direction
 *   :Backspace strips a glyph and repeats search
 *   :A-r - sets prev line as search start and repeats search
 *   :A-s - sets next line as search start and repeats.
 *   :Enter - drops out of search mode
 * Anything else drops out of search mode and repeats the command as normal
 *
 * For each history document a number of "favourites" can be registered.
 * These are accessed by moving "down" from the start point rather than "up"
 * for previous history items.
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "core.h"
#include "misc.h"

struct history_info {
	struct pane	*history;
	char		*saved;
	char		*prompt;
	struct buf	search;
	int		search_back;
	int		favourite;
	struct si {
		int i;
		struct si *prev;
		struct mark *line;
	} *prev;
	int		changed;
};

static struct map *history_map;
DEF_LOOKUP_CMD(history_handle, history_map);

static void free_si(struct si **sip safe)
{
	struct si *i;;

	while ((i = *sip) != NULL) {
		*sip = i->prev;
		if (i->prev == NULL || i->prev->line != i->line)
			mark_free(i->line);
		free(i);
	}
}

DEF_CMD(history_close)
{
	struct history_info *hi = ci->home->data;

	free_si(&hi->prev);
	if (hi->history)
		pane_close(hi->history);
	return 1;
}

DEF_CMD(history_free)
{
	struct history_info *hi = ci->home->data;

	free(hi->search.b);
	free(hi->saved);
	free(hi->prompt);
	unalloc(hi, pane);
	/* handle was in 'hi' */
	ci->home->handle = NULL;
	return 1;
}

DEF_CMD(history_notify_close)
{
	struct history_info *hi = ci->home->data;

	if (ci->focus == hi->history) {
		/* The history document is going away!!! */
		free_si(&hi->prev);
		hi->history = NULL;
	}
	return 1;
}

DEF_CMD(history_save)
{
	struct history_info *hi = ci->home->data;
	const char *eol;
	const char *line = ci->str;
	const char *prev;

	if (!hi->history || !ci->str)
		/* history document was destroyed */
		return 1;
	/* Must never include a newline in a history entry! */
	eol = strchr(ci->str, '\n');
	if (eol)
		line = strnsave(ci->home, ci->str, eol - ci->str);

	prev = call_ret(strsave, "history:get-last", ci->focus);
	if (prev && line && strcmp(prev, line) == 0)
		return 1;

	call("doc:file", hi->history, 1);
	call("Replace", hi->history, 1, NULL, line);
	call("Replace", hi->history, 1, NULL, "\n", 1);
	return 1;
}

DEF_CMD(history_done)
{
	history_save_func(ci);
	return Efallthrough;
}

DEF_CMD(history_notify_replace)
{
	struct history_info *hi = ci->home->data;

	if (hi->history)
		hi->changed = 1;
	return 1;
}

static void recall_line(struct pane *p safe, struct pane *focus safe, int fore)
{
	struct history_info *hi = p->data;
	struct mark *m;
	char *l, *e;

	if (!hi->history)
		return;
	m = mark_at_point(hi->history, NULL, MARK_UNGROUPED);
	call("doc:EOL", hi->history, 1, m, NULL, 1);
	l = call_ret(str, "doc:get-str", hi->history, 0, NULL, NULL, 0, m);
	mark_free(m);
	if (!l || !*l) {
		/* No more history */
		free(l);
		if (!fore)
			return;

		l = hi->saved;
	}
	if (l) {
		e = strchr(l, '\n');
		if (e)
			*e = 0;
	}
	call("doc:EOL", focus, -1);
	m = mark_at_point(focus, NULL, MARK_UNGROUPED);
	call("doc:EOL", focus, 1, m);
	if (hi->changed) {
		if (l != hi->saved)
			free(hi->saved);
		hi->saved = call_ret(str, "doc:get-str", focus,
				     0, NULL, NULL,
				     0, m);
	}
	call("Replace", focus, 1, m, l);
	if (l != hi->saved){
		free(l);
		hi->changed = 0;
	}
	mark_free(m);
}

DEF_CMD(history_move)
{
	struct history_info *hi = ci->home->data;
	const char *suffix = ksuffix(ci, "K:A-");
	char attr[sizeof("doc:favourite-") + 12];

	if (!hi->history)
		return Enoarg;
	if (*suffix == 'p') {
		if (hi->favourite > 0)
			hi->favourite -= 1;
		else
			call("doc:EOL", hi->history, -2);
	} else {
		if (hi->favourite > 0)
			hi->favourite += 1;
		else if (call("doc:EOL", hi->history, 1, NULL, NULL, 1) < 0)
			hi->favourite = 1;
	}
	while (hi->favourite > 0) {
		char *f;
		struct mark *m;
		snprintf(attr, sizeof(attr)-1, "doc:favourite-%d",
			 hi->favourite);
		f = pane_attr_get(hi->history, attr);
		if (!f) {
			hi->favourite -= 1;
			continue;
		}
		call("doc:EOL", ci->focus, -1);
		m = mark_at_point(ci->focus, NULL, MARK_UNGROUPED);
		call("doc:EOL", ci->focus, 1, m);
		call("Replace", ci->focus, 1, m, f);
		mark_free(m);
		return 1;
	}
	recall_line(ci->home, ci->focus, *suffix == 'n');
	return 1;
}

DEF_CMD(history_add_favourite)
{
	struct history_info *hi = ci->home->data;
	char attr[sizeof("doc:favourite-") + 10];
	int f;
	char *l;

	if (!hi->history)
		return 1;
	l = call_ret(strsave, "doc:get-str", ci->focus);
	if (!l || !*l)
		return 1;
	for (f = 1; f < 100; f++) {
		snprintf(attr, sizeof(attr)-1, "doc:favourite-%d", f);
		if (pane_attr_get(hi->history, attr))
			continue;
		call("doc:set:", hi->history, 0, NULL, l, 0, NULL, attr);
		call("Message:modal", ci->focus, 0, NULL, "Added as favourite");
		break;
	}
	return 1;
}

DEF_CMD(history_attach)
{
	struct history_info *hi;
	struct pane *p;

	if (!ci->str)
		return Enoarg;

	alloc(hi, pane);
	p = call_ret(pane, "docs:byname", ci->focus, 0, NULL, ci->str);
	if (!p)
		p = call_ret(pane, "doc:from-text", ci->focus, 0, NULL, ci->str);

	if (!p) {
		free(hi);
		return Efail;
	}
	hi->history = call_ret(pane, "doc:attach-view", p, -1, NULL, "invisible");
	if (!hi->history)
		return Efail;
	call("doc:file", hi->history, 1);
	buf_init(&hi->search);
	buf_concat(&hi->search, "?0"); /* remaining chars are searched verbatim */
	p = pane_register(ci->focus, 0, &history_handle.c, hi);
	if (!p)
		return Efail;
	pane_add_notify(p, hi->history, "Notify:Close");
	call("doc:request:doc:replaced", p);
	return comm_call(ci->comm2, "callback:attach", p);
}

DEF_CMD(history_hlast)
{
	struct history_info *hi = ci->home->data;
	struct pane *doc = hi->history;
	struct mark *m, *m2;
	int rv;

	if (!doc)
		return Einval;

	m = mark_new(doc);
	if (!m)
		return 1;
	call("doc:set-ref", doc, 0, m);
	call("doc:set", doc, 0, m, NULL, 1);
	doc_prev(doc,m);
	m2 = mark_dup(m);
	while (doc_prior(doc, m) != '\n')
		if (doc_prev(doc,m) == WEOF)
			break;
	rv = call_comm("doc:get-str", doc, ci->comm2, 0, m, NULL, 0, m2);
	mark_free(m);
	mark_free(m2);
	return rv;
}

static bool has_name(struct pane *doc safe, struct mark *m safe,
		     const char *name safe)
{
	char *a;

	a = call_ret(strsave, "doc:get-attr", doc, 0, m, "history:name");
	return a && strcmp(a, name) == 0;
}

DEF_CMD(history_last)
{
	/* Get last line from the given history document
	 * If ci->num > 1 get nth last line
	 * else if ci->str, get the line with given name
	 * If both set, assign str to the nth last line
	 * Names are assign with attribute "history:name"
	 */
	struct pane *doc;
	struct mark *m, *m2;
	int num = ci->num;
	const char *name = ci->str2;
	int rv;

	doc = call_ret(pane, "docs:byname", ci->focus, 0, NULL, ci->str);
	if (!doc)
		return 1;
	m = mark_new(doc);
	if (!m)
		return 1;
	call("doc:set-ref", doc, 0, m);
	call("doc:set", doc, 0, m, NULL, 1);
	do {
		doc_prev(doc,m);
		m2 = mark_dup(m);
		while (doc_prior(doc, m) != '\n')
			if (doc_prev(doc,m) == WEOF)
				break;
	} while (!mark_same(m, m2) && num > 1 &&
		 (name == NULL || has_name(doc, m, name)));
	if (mark_same(m, m2) || num > 1)
		rv = Efail;
	else {
		if (num == 1 && name)
			call("doc:set-attr", doc, 0, m, "history:name",
			     0, NULL, name);
		rv = call_comm("doc:get-str", doc, ci->comm2,
			       0, m, NULL, 0, m2);
	}
	mark_free(m);
	mark_free(m2);
	return rv;
}

DEF_CMD(history_search)
{
	struct history_info *hi = ci->home->data;
	char *prompt, *prefix;

	if (!hi->history)
		return 1;
	call("Mode:set-mode", ci->focus, 0, NULL, ":History-search");
	buf_reinit(&hi->search);
	buf_concat(&hi->search, "?0");
	free_si(&hi->prev);
	prompt = pane_attr_get(ci->focus, "prompt");
	if (!prompt)
		prompt = "?";
	free(hi->prompt);
	hi->prompt = strdup(prompt);
	prefix = strconcat(ci->focus, prompt, " (): ");
	attr_set_str(&ci->focus->attrs, "prefix", prefix);
	call("view:changed", ci->focus);

	hi->search_back = (toupper(ci->key[4]) == 'R');
	return 1;
}

static void update_search(struct pane *p safe, struct pane *focus safe,
			  int offset)
{
	struct history_info *hi = p->data;
	struct si *i;
	struct mark *m;
	const char *prefix;
	int ret;

	if (!hi->history)
		return;
	if (offset >= 0) {
		alloc(i, pane);
		i->i = offset;
		i->line = mark_at_point(hi->history, NULL, MARK_UNGROUPED);
		i->prev = hi->prev;
		hi->prev = i;
	}
	prefix = strconcat(focus, hi->prompt?:"?",
			   " (", buf_final(&hi->search)+2, "): ");
	attr_set_str(&focus->attrs, "prefix", prefix);
	call("view:changed", focus);
	call("Mode:set-mode", focus, 0, NULL, ":History-search");
	m = mark_at_point(hi->history, NULL, MARK_UNGROUPED);
	/* Alway search backwards from the end-of-line of last match */
	call("doc:EOL", hi->history, 1, m);
	ret = call("text-search", hi->history, 1, m, buf_final(&hi->search),
		   hi->search_back);
	if (ret <= 0) {
		// clear line
		mark_free(m);
		return;
	}
	/* Leave point at start-of-line */
	call("doc:EOL", hi->history, -1, m);
	call("Move-to", hi->history, 0, m);
	mark_free(m);
	recall_line(p, focus, 0);
}

DEF_CMD(history_search_again)
{
	struct history_info *hi = ci->home->data;
	const char *k;

	k = ksuffix(ci, "K:History-search-");
	if (*k) {
		int l = hi->search.len;
		buf_concat(&hi->search, k);
		update_search(ci->home, ci->focus, l);
	}
	return 1;
}

DEF_CMD(history_search_retry);

DEF_CMD(history_search_bs)
{
	struct history_info *hi = ci->home->data;
	struct si *i = hi->prev;

	if (!i || !hi->history) {
		history_search_retry_func(ci);
		return 1;
	}

	call("Mode:set-mode", ci->focus, 0, NULL, ":History-search");

	hi->search.len = i->i;
	call("Move:to", hi->history, 0, i->line);
	if (!i->prev || i->line != i->prev->line)
		mark_free(i->line);
	hi->prev = i->prev;
	free(i);
	update_search(ci->home, ci->focus, -1);
	return 1;
}

DEF_CMD(history_search_repeat)
{
	struct history_info *hi = ci->home->data;
	const char *suffix = ksuffix(ci, "K:History-search:C-");

	if (!hi->history)
		return Enoarg;
	hi->search_back = toupper(*suffix) == 'R';
	if (hi->search_back)
		call("doc:EOL", hi->history, -2);
	else
		call("doc:EOL", hi->history, 1, NULL, NULL, 1);

	update_search(ci->home, ci->focus, hi->search.len);
	return 1;
}

DEF_CMD(history_search_cancel)
{
	struct history_info *hi = ci->home->data;
	const char *prefix;

	prefix = strconcat(ci->focus, hi->prompt?:"?", ": ");
	attr_set_str(&ci->focus->attrs, "prefix", prefix);
	call("view:changed", ci->focus);
	return 1;
}

REDEF_CMD(history_search_retry)
{
	struct history_info *hi = ci->home->data;
	const char *prefix;
	char *k = strconcat(ci->home, "K", ksuffix(ci, "K:History-search"));

	prefix = strconcat(ci->focus, hi->prompt?:"?", ": ");
	attr_set_str(&ci->focus->attrs, "prefix", prefix);
	call("view:changed", ci->focus);
	return call(k, ci->focus, ci->num, ci->mark, ci->str,
		    ci->num2, ci->mark2, ci->str2);
}

DEF_CMD(history_add)
{
	const char *docname = ci->str;
	const char *line = ci->str2;
	struct pane *doc;

	if (!docname || !line || strchr(line, '\n'))
		return Einval;
	doc = call_ret(pane, "docs:byname", ci->focus, 0, NULL, ci->str);
	if (!doc) {
		doc = call_ret(pane, "doc:from-text", ci->focus,
			       0, NULL, ci->str);
		if (doc)
			call("global-multicall-doc:appeared-", doc);
	}
	if (!doc)
		return Efail;
	call("doc:replace", doc, 1, NULL, line, 1);
	call("doc:replace", doc, 1, NULL, "\n", 1);
	return 1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &history_attach, 0, NULL, "attach-history");
	call_comm("global-set-command", ed, &history_last, 0, NULL, "history:get-last");
	call_comm("global-set-command", ed, &history_add, 0, NULL, "history:add");

	if (history_map)
		return;

	history_map = key_alloc();
	key_add(history_map, "Close", &history_close);
	key_add(history_map, "Free", &history_free);
	key_add(history_map, "Notify:Close", &history_notify_close);
	key_add(history_map, "doc:replaced", &history_notify_replace);
	key_add(history_map, "K:A-p", &history_move);
	key_add(history_map, "K:A-n", &history_move);
	key_add(history_map, "K:A-r", &history_search);
	key_add(history_map, "K:A-s", &history_search);
	key_add(history_map, "K:A-*", &history_add_favourite);
	key_add_prefix(history_map, "K:History-search-", &history_search_again);
	key_add_prefix(history_map, "K:History-search:",
		       &history_search_retry);
	key_add(history_map, "K:History-search:Backspace",
		       &history_search_bs);
	key_add(history_map, "K:History-search:A-r",
		       &history_search_repeat);
	key_add(history_map, "K:History-search:A-s",
		       &history_search_repeat);
	key_add(history_map, "K:History-search:Enter",
		       &history_search_cancel);
	key_add(history_map, "K:History-search:ESC",
		       &history_search_cancel);
	key_add(history_map, "history:save", &history_save);
	key_add(history_map, "history:get-last", &history_hlast);
	key_add(history_map, "popup:close", &history_done);
}
