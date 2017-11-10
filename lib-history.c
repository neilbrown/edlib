/*
 * Copyright Neil Brown ©2016 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * history
 *
 * A history pane supports selection of lines from a separate
 * document.  The underlying document is assumed to be one line
 * and this line can be replaced by various lines from the history document.
 * When a line is replaced, if it had been modified, it is saved first.
 * M-p - replace current line with previous line from history, if there is one
 * M-n - replace current line with next line from history.  If none, restore
 *       saved line
 * M-r - incremental search - later
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
	struct pane *p = ci->home;
	struct history_info *hi = p->data;

	if (hi->history)
		pane_close(hi->history);
	free(hi->search.b);
	free(hi->saved);
	free(hi);
	p->data = safe_cast NULL;
	return 1;
}

DEF_CMD(history_notify_close)
{
	struct pane *p = ci->home;
	struct history_info *hi = p->data;

	if (ci->focus == hi->history)
		/* The history document is going away!!! */
		hi->history = NULL;
	return 1;
}

DEF_CMD(history_done)
{
	struct pane *p = ci->home;
	struct history_info *hi = p->data;

	if (!hi->history)
		/* history document was destroyed */
		return 0;
	call("Move-File", hi->history, 1);
	call("Replace", hi->history, 1, NULL, ci->str, 1);
	call("Replace", hi->history, 1, NULL, "\n", 1);
	return 0;
}


DEF_CMD(history_notify_replace)
{
	struct pane *p = ci->home;
	struct history_info *hi = p->data;

	if (hi->history)
		hi->changed = 1;
	return 1;
}

DEF_CMD(history_move)
{
	struct pane *p = ci->home;
	struct history_info *hi = p->data;
	struct mark *m;
	char *l, *e;

	if (!hi->history || !ci->mark)
		return 0;
	if (ci->key[6] == 'p') {
		m = mark_at_point(hi->history, NULL, MARK_UNGROUPED);
		call("Move-EOL", hi->history, -2);
	} else {
		call("Move-EOL", hi->history, 1);
		call("Move-Char", hi->history, 1);
		m = mark_at_point(hi->history, NULL, MARK_UNGROUPED);
		call("Move-EOL", hi->history, 1, m);
		call("Move-Char", hi->history, 1, m);
	}
	l = call_ret(str, "doc:get-str", hi->history, 0, NULL, NULL, 0, m);
	if (!l || !*l) {
		/* No more history */
		free(l);
		if (ci->key[6] == 'p') {
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
	call("Move-EOL", ci->focus, -1, ci->mark);
	m = mark_dup(ci->mark, 1);
	call("Move-EOL", ci->focus, 1, m);
	if (hi->changed) {
		if (l != hi->saved)
			free(hi->saved);
		hi->saved = call_ret(str, "doc:get-str", ci->focus,
				     0, ci->mark, NULL,
				     0, m);
	}
	call("Replace", ci->focus, 1, m, l, 1);
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
		return -1;

	hi = calloc(1, sizeof(*hi));
	hi->done_map = key_alloc();
	hi->handle = history_handle;
	hi->handle.m = &hi->done_map;
	key_add_chain(hi->done_map, history_map);
	key_add(hi->done_map, ci->str2, &history_done);
	p = call_pane("docs:byname", ci->focus, 0, NULL, ci->str);
	if (!p)
		p = call_pane("doc:from-text", ci->focus, 0, NULL, ci->str,
			      0, NULL, "");
	if (!p) {
		free(hi);
		return 0;
	}
	hi->history = call_pane("doc:attach", p);
	if (!hi->history)
		return 0;
	home_call(hi->history, "doc:assign", p);
	call("Move-File", hi->history, 1);
	buf_init(&hi->search);
	p = pane_register(ci->focus, 0, &hi->handle.c, hi, NULL);
	pane_add_notify(p, hi->history, "Notify:Close");
	call("Request:Notify:doc:Replace", p);
	return comm_call(ci->comm2, "callback:attach", p);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &history_attach, 0, NULL, "attach-history");

	if (history_map)
		return;

	history_map = key_alloc();
	key_add(history_map, "Close", &history_close);
	key_add(history_map, "Notify:Close", &history_notify_close);
	key_add(history_map, "Notify:doc:Replace", &history_notify_replace);
	key_add(history_map, "M-Chr-p", &history_move);
	key_add(history_map, "M-Chr-n", &history_move);
}
