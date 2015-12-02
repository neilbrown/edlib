/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Searching.
 * "text-search" command searches from given mark until it
 * finds the given string or end of buffer.
 * Leave mark at end of match and set ->extra to length of match.
 */

#include <stdlib.h>
#include "core.h"
#include "rexel.h"

DEF_CMD(text_search)
{
	struct mark *m;
	struct doc *d;
	unsigned short *rxl;
	struct match_state *st;
	int since_start, len;

	if (!ci->str|| !ci->mark)
		return -1;

	d = doc_from_pane(ci->focus);
	if (!d)
		return -1;
	m = ci->mark;
	rxl = rxl_parse(ci->str, NULL, 0);
	if (!rxl)
		return -1;
	st = rxl_prepare(rxl);
	since_start = -1;
	while (since_start < 0 || len > 0) {
		wint_t wch = mark_next(d, m);
		if (wch == WEOF)
			break;
		if (since_start >= 0)
			since_start += 1;
		len = rxl_advance(st, wch, 0, since_start < 0);
		if (len >= 0 &&
		    (since_start < 0 || len > since_start))
			since_start = len;
	}
	if (since_start > 0)
		mark_prev(d, m);
	ci->extra = since_start;
	rxl_free_state(st);
	free(rxl);
	return 1;
}

void edlib_init(struct editor *ed)
{
	key_add(ed->commands, "text-search", &text_search);
}
