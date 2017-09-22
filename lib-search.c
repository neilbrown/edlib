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

static int search_forward(struct pane *p safe, struct mark *m safe, struct mark *m2,
			  unsigned short *rxl safe,
			  struct mark *endmark safe)
{
	/* Search forward from @m in @p for @rxl looking as far as @m2, and leaving
	 * @endmark at the end point, and returning the length of the match, or -1.
	 */
	int since_start = -1, len = 0;
	struct match_state *st;

	st = rxl_prepare(rxl);
	while ((since_start < 0 || len != -2) &&
	       (m2 == NULL || m->seq < m2->seq)) {
		wint_t wch = mark_next_pane(p, m);
		if (wch == WEOF)
			break;

		len = rxl_advance(st, wch, 0, since_start < 0);
		if (len >= 0 &&
		    (since_start < 0 || len > since_start)) {
			since_start = len;
			mark_to_mark(endmark, m);
		}
	}
	rxl_free_state(st);
	return since_start;
}

static int search_backward(struct pane *p safe, struct mark *m safe, struct mark *m2,
			   unsigned short *rxl safe,
			   struct mark *endmark safe)
{
	/* Search backward from @m in @p for a match of @s.  The match must start
	 * before m, but may finish later.
	 * Only search as far as @m2 (if set), and leave endmark pointing at the
	 * start of the match, if one is found.
	 * return length of match, or negative.
	 */

	int since_start, len;
	struct match_state *st = rxl_prepare(rxl);

	do {
		mark_to_mark(endmark, m);
		since_start = 0;
		len = -1;
		while (len == -1) {
			wint_t wch = mark_next_pane(p, m);
			if (wch == WEOF)
				break;
			since_start += 1;
			len = rxl_advance(st, wch, 0, since_start == 1);
		}
		mark_to_mark(m, endmark);
	} while(len < since_start &&
		(!m2 || m2->seq < m->seq) &&
		(mark_prev_pane(p, m) != WEOF));
	rxl_free_state(st);
	return len == since_start ? len : -1;
}

DEF_CMD(text_search)
{
	struct mark *m, *endmark = NULL;
	unsigned short *rxl;
	int since_start;

	if (!ci->str|| !ci->mark)
		return -1;

	m = ci->mark;
	rxl = rxl_parse(ci->str, NULL, 1);
	if (!rxl)
		return -1;
	since_start = -1;
	endmark = mark_dup(m, 1);
	if (!endmark)
		return -1;

	if (ci->extra)
		since_start = search_backward(ci->focus, m, ci->mark2, rxl, endmark);
	else
		since_start = search_forward(ci->focus, m, ci->mark2, rxl, endmark);

	if (since_start > 0)
		mark_to_mark(m, endmark);
	mark_free(endmark);
	free(rxl);
	if (since_start < 0)
		return -2;
	return since_start + 1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, 0, NULL, "text-search",
		  &text_search);
}
