/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Searching.
 * "text-search" command searches from given mark until it
 * finds the given string or end of buffer.
 * Leave mark at end of match and set ->extra to length of match.
 * If mark2 is given, don't go beyond there.
 */

#include <stdlib.h>
#include "core.h"
#include "rexel.h"

DEF_CMD(text_search)
{
	struct mark *m, *endmark = NULL;
	unsigned short *rxl;
	struct match_state *st;
	int since_start, len;

	if (!ci->str|| !ci->mark || !ci->focus)
		return -1;

	m = ci->mark;
	rxl = rxl_parse(ci->str, NULL, 1);
	if (!rxl)
		return -1;
	st = rxl_prepare(rxl);
	since_start = -1;
	while ((since_start < 0 || len != -2) &&
	       (ci->mark2 == NULL || m->seq < ci->mark2->seq)) {
		wint_t wch = mark_next_pane(ci->focus, m);
		if (wch == WEOF)
			break;

		len = rxl_advance(st, wch, 0, since_start < 0);
		if (len >= 0 &&
		    (since_start < 0 || len > since_start)) {
			since_start = len;
			if (endmark)
				mark_free(endmark);
			endmark = mark_dup(m, 1);
		}
	}
	if (since_start > 0 && endmark) {
		mark_to_mark(m, endmark);
		mark_free(endmark);
	}
	rxl_free_state(st);
	free(rxl);
	if (since_start < 0)
		return -2;
	return since_start + 1;
}

void edlib_init(struct pane *ed)
{
	call_comm("global-set-command", ed, 0, NULL, "text-search",
		  0, &text_search);
}
