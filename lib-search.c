/*
 * Copyright Neil Brown ©2015-2018 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Searching.
 * "text-search" command searches from given mark until it
 * finds the given string or end of buffer.
 * Leave mark at end of match and set ->num2 to length of match.
 * If mark2 is given, don't go beyond there.
 */

#include <stdlib.h>
#include "core.h"
#include "rexel.h"

struct search_state {
	struct match_state *st safe;
	struct mark *end;
	struct mark *endmark safe;
	int since_start;
	struct command c;
};

DEF_CMD(search_test)
{
	wint_t wch = ci->num & 0xFFFFF;
	int len;
	int i;
	struct search_state *ss = container_of(ci->comm, struct search_state, c);

	if (!ci->mark)
		return 0;

	for (i = -1; i <= 1; i++) {
		switch(i) {
		case -1:
			if (wch == '\n')
				len = rxl_advance(ss->st, WEOF, RXL_EOL, ss->since_start < 0);
			else
				continue;
			break;
		case 0:
			len = rxl_advance(ss->st, wch, 0, ss->since_start < 0);
			break;
		case 1:
			if (wch == '\n')
				len = rxl_advance(ss->st, WEOF, RXL_SOL, ss->since_start < 0);
			else
				continue;
			break;
		}
		if (len >= 0 &&
		    (ss->since_start < 0 || len > ss->since_start)) {
			ss->since_start = len;
			mark_to_mark(ss->endmark, ci->mark);
			if (i >= 0)
				mark_next_pane(ci->home, ss->endmark);
		}
		if ((ss->since_start < 0 || len != -2) &&
		    (ss->end == NULL || ci->mark->seq < ss->end->seq))
			/* I like that one, more please */
			continue;
		return 0;
	}
	return 1;
}

static int search_forward(struct pane *p safe, struct mark *m safe, struct mark *m2,
			  unsigned short *rxl safe,
			  struct mark *endmark safe)
{
	/* Search forward from @m in @p for @rxl looking as far as @m2, and leaving
	 * @endmark at the end point, and returning the length of the match, or -1.
	 */
	struct search_state ss;
	wint_t ch;

	if (m2 && m->seq >= m2->seq)
		return -1;
	ss.st = rxl_prepare(rxl);
	ss.since_start = -1;
	ss.end = m2;
	ss.endmark = endmark;
	ss.c = search_test;
	ch = doc_following_pane(p, m);
	if (ch == WEOF || is_eol(ch))
		rxl_advance(ss.st, WEOF, RXL_EOL, 1);
	ch = doc_prior_pane(p, m);
	if (ch == WEOF || is_eol(ch))
		rxl_advance(ss.st, WEOF, RXL_SOL, 1);

	call_comm("doc:content", p, &ss.c, 0, m);
	rxl_free_state(ss.st);
	return ss.since_start;
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
		return Enoarg;

	m = ci->mark;
	rxl = rxl_parse(ci->str, NULL, ci->num);
	if (!rxl)
		return Einval;
	since_start = -1;
	endmark = mark_dup(m);
	if (!endmark)
		return Efail;

	if (ci->num2)
		since_start = search_backward(ci->focus, m, ci->mark2, rxl, endmark);
	else
		since_start = search_forward(ci->focus, m, ci->mark2, rxl, endmark);

	if (since_start > 0)
		mark_to_mark(m, endmark);
	mark_free(endmark);
	free(rxl);
	if (since_start < 0)
		return Efail;
	return since_start + 1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &text_search, 0, NULL, "text-search");
}
