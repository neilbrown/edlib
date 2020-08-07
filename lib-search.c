/*
 * Copyright Neil Brown ©2015-2020 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Searching.
 * "text-search" command searches from given mark until it
 * finds the given pattern or end of buffer.
 * If the pattern is found, then 'm' is left at the extremity of
 * the match in the direction of search: so the start if search backwards
 * or the end if searching forwards.
 * The returned value is the length of the match + 1, or an Efail
 * In the case of an error, the location of ->mark is undefined.
 * If mark2 is given, don't go beyond there.
 *
 * "text-match" is similar to text-search forwards, but requires that
 * the match starts at ->mark.  ->mark is moved to the end of the
 * match is the text does, in fact, match.
 * If the match fails, Efalse is returned (different to "text-search")
 */

#include <stdlib.h>
#include <wctype.h>
#include <string.h>
#include "core.h"
#include "rexel.h"

struct search_state {
	struct match_state *st safe;
	struct mark *end;
	struct mark *endmark;
	wint_t prev_ch;
	struct command c;
};

static int is_word(wint_t ch)
{
	return ch == '_' || iswalnum(ch);
}

DEF_CMD(search_test)
{
	wint_t wch = ci->num & 0xFFFFF;
	wint_t flags = 0;
	int maxlen, since_start;
	enum rxl_found found;
	struct search_state *ss = container_of(ci->comm,
					       struct search_state, c);

	if (!ci->mark)
		return Enoarg;

	if ((unsigned int)(ci->num & 0xffffffff) == WEOF) {
		wch = 0;
		flags |= RXL_EOD;
	}
	if (ss->prev_ch == WEOF)
		flags |= RXL_SOD;
	if (is_eol(ss->prev_ch) || ss->prev_ch == WEOF || ss->prev_ch == 0)
		flags |= RXL_SOL;
	switch (is_word(ss->prev_ch) * 2 + is_word(wch)) {
	case 0: /* in space */
	case 3: /* within word */
		flags |= RXL_NOWBRK;
		break;
	case 1: /* start of word */
		flags |= RXL_SOW;
		break;
	case 2: /* end of word */
		flags |= RXL_EOW;
		break;
	}
	if (is_eol(wch))
		flags |= RXL_EOL;

	found = rxl_advance(ss->st, wch | flags);
	rxl_info(ss->st, &maxlen, NULL, NULL, &since_start);

	if (found >= RXL_MATCH && ss->endmark && since_start - maxlen <= 1) {
		mark_to_mark(ss->endmark, ci->mark);
		if (found == RXL_MATCH)
			doc_next(ci->home, ss->endmark);
	}
	if (ss->end &&  ci->mark->seq >= ss->end->seq)
		return 0;
	if (found == RXL_DONE)
		/* No match here */
		return 0;
	ss->prev_ch = wch;
	return 1;
}

static int search_forward(struct pane *p safe,
			  struct mark *m safe, struct mark *m2,
			  unsigned short *rxl safe,
			  struct mark *endmark, bool anchored)
{
	/* Search forward from @m in @p for @rxl looking as far as @m2,
	 * and leaving @endmark at the end point, and returning the
	 * length of the match, or -1.
	 */
	struct search_state ss;
	int maxlen;

	if (m2 && m->seq >= m2->seq)
		return -1;
	ss.st = rxl_prepare(rxl, anchored ? RXL_ANCHORED : 0);
	ss.end = m2;
	ss.endmark = endmark;
	ss.c = search_test;
	ss.prev_ch = doc_prior(p, m);
	call_comm("doc:content", p, &ss.c, 0, m);
	rxl_info(ss.st, &maxlen, NULL, NULL, NULL);
	rxl_free_state(ss.st);
	return maxlen;
}

static int search_backward(struct pane *p safe,
			   struct mark *m safe, struct mark *m2,
			   unsigned short *rxl safe,
			   struct mark *endmark safe)
{
	/* Search backward from @m in @p for a match of @s.  The match
	 * must start at or before m, but may finish later.  Only search
	 * as far as @m2 (if set), and leave endmark pointing at the
	 * start of the match, if one is found.  return length of match,
	 * or negative.
	 */
	struct search_state ss;
	int maxlen;

	ss.end = NULL;
	ss.endmark = NULL;
	ss.c = search_test;

	do {

		ss.st = rxl_prepare(rxl, RXL_ANCHORED);
		ss.prev_ch = doc_prior(p, m);

		mark_to_mark(endmark, m);
		call_comm("doc:content", p, &ss.c, 0, endmark);
		rxl_info(ss.st, &maxlen, NULL, NULL, NULL);
		rxl_free_state(ss.st);

		if (maxlen >= 0)
			/* found a match */
			break;
	} while((!m2 || m2->seq < m->seq) &&
		(doc_prev(p, m) != WEOF));
	mark_to_mark(endmark, m);
	return maxlen;
}

DEF_CMD(text_search)
{
	struct mark *m, *endmark = NULL;
	unsigned short *rxl;
	int since_start;
	int ret;

	if (!ci->str)
		return Enoarg;

	rxl = rxl_parse(ci->str, NULL, ci->num);
	if (!rxl)
		return Einval;

	if (ci->mark) {
		m = ci->mark;
		endmark = mark_dup(m);
		if (!endmark)
			return Efail;
		if (strcmp(ci->key, "text-match") == 0)
			since_start = search_forward(ci->focus, m, ci->mark2,
						     rxl, endmark, True);
		else if (ci->num2)
			since_start = search_backward(ci->focus, m, ci->mark2,
						      rxl, endmark);
		else
			since_start = search_forward(ci->focus, m, ci->mark2,
						     rxl, endmark, False);

		if (since_start >= 0)
			mark_to_mark(m, endmark);
		mark_free(endmark);
		if (since_start < 0) {
			if (strcmp(ci->key, "text-match") == 0)
				ret = Efalse; /* non-fatal */
			else
				ret = Efail;
		} else
			ret = since_start + 1;
	} else if (ci->str2) {
		struct match_state *st = rxl_prepare(
			rxl, strcmp(ci->key, "text-match") == 0 ? RXL_ANCHORED : 0);
		int flags = RXL_SOL|RXL_SOD;
		const char *t = ci->str2;
		int thelen, start;
		enum rxl_found r;
		wint_t prev_ch = WEOF;

		do {
			wint_t wc = get_utf8(&t, NULL);
			if (wc >= WERR) {
				rxl_advance(st, RXL_EOL|RXL_EOD);
				break;
			}
			switch (is_word(prev_ch) * 2 + is_word(wc)) {
			case 0: /* in space */
			case 3: /* within word */
				flags |= RXL_NOWBRK;
				break;
			case 1: /* start of word */
				flags |= RXL_SOW;
				break;
			case 2: /* end of word */
				flags |= RXL_EOW;
				break;
			}
			r = rxl_advance(st, wc | flags);
			flags = 0;
		} while (r != RXL_DONE);
		rxl_info(st, &thelen, NULL, &start, NULL);
		rxl_free_state(st);
		if (thelen < 0)
			ret = Efalse;
		else if (strcmp(ci->key, "text-match") == 0)
			ret = thelen + 1;
		else
			ret = start + 1;
	} else {
		ret = Einval;
	}
	free(rxl);
	return ret;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &text_search, 0, NULL,
		  "text-search");
	call_comm("global-set-command", ed, &text_search, 0, NULL,
		  "text-match");
}
