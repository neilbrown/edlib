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
	struct mark	*m safe;
	char		*saved;
	char		*donekey;
	struct buf	search;
	int		changed;
};

DEF_CMD(history_handle)
{
	struct pane *p = ci->home;
	struct history_info *hi = p->data;
	struct mark *m;

	if (strcmp(ci->key, "Close") == 0) {
		mark_free(hi->m);
		free(hi->search.b);
		free(hi->saved);
		free(hi);
		p->data = safe_cast NULL;
		return 1;
	}
	if (!hi->history)
		/* history document was destroyed */
		return 0;

	if (strcmp(ci->key, "Notify:Close") == 0 && ci->focus == hi->history) {
		/* The history document is going away!!! */
		hi->history = NULL;
		return 1;
	}

	if (hi->donekey && strcmp(ci->key, hi->donekey) == 0 && ci->str) {
		call("Move-File", hi->history, 1, hi->m);
		call("doc:replace", hi->history, 1, NULL, ci->str, 1, hi->m);
		call("doc:replace", hi->history, 1, NULL, "\n", 1, hi->m);
		return 0;
	}

	if (strcmp(ci->key, "Notify:doc:Replace") == 0) {
		hi->changed = 1;
		return 1;
	}

	if (ci->mark &&
	    (strcmp(ci->key, "M-Chr-p") == 0 || strcmp(ci->key, "M-Chr-n") == 0)) {
		char *l, *e;
		if (ci->key[6] == 'p') {
			m = mark_dup(hi->m, 1);
			call("Move-EOL", hi->history, -2, hi->m);
		} else {
			call("Move-EOL", hi->history, 1, hi->m);
			call("Move-Char", hi->history, 1, hi->m);
			m = mark_dup(hi->m, 1);
			call("Move-EOL", hi->history, 1, m);
			call("Move-Char", hi->history, 1, m);
		}
		if (mark_same_pane(hi->history, m, hi->m)) {
			/* No more history */
			if (ci->key[6] == 'p') {
				mark_free(m);
				return 1;
			} else
				l = hi->saved;
		} else
			l = doc_getstr(hi->history, m, hi->m);
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
			hi->saved = doc_getstr(ci->focus, ci->mark, m);
		}
		call("Replace", ci->focus, 1, m, l, 1);
		if (l != hi->saved){
			free(l);
			hi->changed = 0;
		}
		mark_free(m);
		return 1;
	}

	if (strcmp(ci->key, "M-Chr-r") == 0) {
	}
	return 0;
}

DEF_CMD(history_attach)
{

	struct history_info *hi;
	struct pane *p;
	struct mark *m;

	if (!ci->str)
		return -1;

	hi = calloc(1, sizeof(*hi));
	hi->donekey = ci->str2;
	p = call_pane7("docs:byname", ci->focus, 0, NULL, 0,
		       ci->str, NULL);
	if (!p)
		p = call_pane7("doc:from-text", ci->focus, 0, NULL, 0,
			       ci->str, "");
	if (!p)
		return 0;
	m = vmark_new(p, MARK_UNGROUPED);
	if (!m) {
		pane_close(p);
		return -1;
	}
	hi->history = p;
	hi->m = m;
	call("Move-File", hi->history, 1, hi->m);
	buf_init(&hi->search);
	p = pane_register(ci->focus, 0, &history_handle, hi, NULL);
	pane_add_notify(p, hi->history, "Notify:Close");
	call("Request:Notify:doc:Replace", p);
	return comm_call(ci->comm2, "callback:attach", p, 0, NULL, NULL, 0);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, 0, NULL, "attach-history",
		  &history_attach);
}
