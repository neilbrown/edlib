/*
 * Copyright Neil Brown ©2016-2020 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * history
 *
 * A history pane supports selection of lines from a separate
 * document.  The underlying document is assumed to be one line
 * and this line can be replaced by various lines from the history document.
 * When a line is replaced, if it had been modified, it is saved first.
 * :A-p - replace current line with previous line from history, if there is one
 * :A-n - replace current line with next line from history.  If none, restore
 *        saved line
 * :A-r - incremental search - later
 * When a selection is committed, it is added to end of history.
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "misc.h"

struct history_info {
	struct pane	*history;
	char		*saved;
	struct buf	search;
	int		changed;
	struct map	*done_map;
	struct lookup_cmd handle;
};

static struct map *history_map;
DEF_LOOKUP_CMD(history_handle, history_map);

DEF_CMD(history_close)
{
	struct history_info *hi = ci->home->data;

	if (hi->history)
		pane_close(hi->history);
	return 1;
}

DEF_CMD(history_free)
{
	struct history_info *hi = ci->home->data;

	free(hi->search.b);
	free(hi->saved);
	unalloc(hi, pane);
	/* handle was in 'hi' */
	ci->home->handle = NULL;
	return 1;
}

DEF_CMD(history_notify_close)
{
	struct history_info *hi = ci->home->data;

	if (ci->focus == hi->history)
		/* The history document is going away!!! */
		hi->history = NULL;
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

DEF_CMD(history_move)
{
	struct history_info *hi = ci->home->data;
	struct mark *m;
	char *l, *e;
	const char *suffix = ksuffix(ci, "K:A-");

	if (!hi->history || !ci->mark)
		return Enoarg;
	if (*suffix == 'p') {
		m = mark_at_point(hi->history, NULL, MARK_UNGROUPED);
		call("doc:EOL", hi->history, -2);
	} else {
		call("doc:EOL", hi->history, 1, NULL, NULL, 1);
		m = mark_at_point(hi->history, NULL, MARK_UNGROUPED);
		call("doc:EOL", hi->history, 1, m, NULL, 1);
	}
	l = call_ret(str, "doc:get-str", hi->history, 0, NULL, NULL, 0, m);
	if (!l || !*l) {
		/* No more history */
		free(l);
		if (*suffix == 'p') {
			mark_free(m);
			return 1;
		} else
			l = hi->saved;
	}
	if (l) {
		e = strchr(l, '\n');
		if (e)
			*e = 0;
	}
	call("doc:EOL", ci->focus, -1, ci->mark);
	m = mark_dup(ci->mark);
	call("doc:EOL", ci->focus, 1, m);
	if (hi->changed) {
		if (l != hi->saved)
			free(hi->saved);
		hi->saved = call_ret(str, "doc:get-str", ci->focus,
				     0, ci->mark, NULL,
				     0, m);
	}
	call("Replace", ci->focus, 1, m, l);
	if (l != hi->saved){
		free(l);
		hi->changed = 0;
	}
	mark_free(m);
	return 1;
}

DEF_CMD(history_attach)
{
	struct history_info *hi;
	struct pane *p;

	if (!ci->str || !ci->str2)
		return Enoarg;

	alloc(hi, pane);
	hi->done_map = key_alloc();
	hi->handle = history_handle;
	hi->handle.m = &hi->done_map;
	key_add_chain(hi->done_map, history_map);
	key_add(hi->done_map, ci->str2, &history_done);
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
	p = pane_register(ci->focus, 0, &hi->handle.c, hi);
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

	m = vmark_new(doc, MARK_UNGROUPED, NULL);
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
	m = vmark_new(doc, MARK_UNGROUPED, NULL);
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
	key_add(history_map, "history:save", &history_save);
	key_add(history_map, "history:get-last", &history_hlast);
}
