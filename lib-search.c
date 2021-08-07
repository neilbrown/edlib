/*
 * Copyright Neil Brown ©2015-2021 <neil@brown.name>
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
	struct mark *point;
	wint_t prev_ch;
	struct command c;

	unsigned short *rxl safe;
};

static void state_free(struct command *c safe)
{
	struct search_state *ss = container_of(c, struct search_state, c);

	free(ss->rxl);
	rxl_free_state(ss->st);
	mark_free(ss->end);
	mark_free(ss->endmark);
	mark_free(ss->point);
	free(ss);
}

static int is_word(wint_t ch)
{
	return ch == '_' || iswalnum(ch);
}

/*
 * 'search_test' together with 'stuct search_state' encapsulates
 * a parsed regexp and some matching state.  If called as 'consume'
 * (or anything starting 'c') it processes one char into the match
 * and returns 1 if it is worth providing more characters.
 * Other options for ci->key are:
 * - reinit - state is re-initialised with flags from ->num, end and
 *            endmark from ->mark and ->mark2
 * - getinfo - extract total, start, len, since-start from match
 * - getcapture - get "start" or "len" for a capture in ->num
 * - interp - interpolate \N captures in ->str
 */
DEF_CB(search_test)
{
	struct search_state *ss = container_of(ci->comm,
					       struct search_state, c);

	if (ci->key[0] == 'c') {
		/* consume */
		wint_t wch = ci->num & 0xFFFFF;
		wint_t flags = 0;
		int maxlen, since_start;
		enum rxl_found found;

		if ((unsigned int)ci->num == WEOF) {
			wch = 0;
			flags |= RXL_EOD;
		}
		if (ss->prev_ch == WEOF)
			flags |= RXL_SOD;
		if (is_eol(ss->prev_ch) || ss->prev_ch == WEOF ||
		    ss->prev_ch == 0)
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
		if (ss->point && ci->mark && mark_same(ss->point, ci->mark))
			found = rxl_advance(ss->st, RXL_POINT);
		rxl_info(ss->st, &maxlen, NULL, NULL, &since_start);

		if (found >= RXL_MATCH && ss->endmark && ci->mark &&
		    since_start - maxlen <= 1) {
			mark_to_mark(ss->endmark, ci->mark);
			if (found == RXL_MATCH_FLAG)
				doc_prev(ci->home, ss->endmark);
		}
		if (ss->end && ci->mark &&
		    (mark_ordered_or_same(ss->end, ci->mark)))
			return Efalse;
		if (found == RXL_DONE)
			/* No match here */
			return Efalse;
		ss->prev_ch = wch;
		return 1;
	}
	if (strcmp(ci->key, "reinit") == 0) {
		rxl_free_state(ss->st);
		ss->st = rxl_prepare(ss->rxl, ci->num);
		ss->prev_ch = (unsigned int)ci->num2 ?: WEOF;
		mark_free(ss->end);
		mark_free(ss->endmark);
		if (ci->mark)
			ss->end = mark_dup(ci->mark);
		else
			ss->end = NULL;
		if (ci->mark2)
			ss->endmark = mark_dup(ci->mark2);
		else
			ss->endmark = NULL;
		return 1;
	}
	if (strcmp(ci->key, "setpoint") == 0 && ci->mark) {
		mark_free(ss->point);
		ss->point = mark_dup(ci->mark);
		return 1;
	}
	if (strcmp(ci->key, "getinfo") == 0 && ci->str) {
		int len, total, start, since_start;
		rxl_info(ss->st, &len, &total, &start, &since_start);
		if (strcmp(ci->str, "len") == 0)
			return len < 0 ? Efalse : len+1;
		if (strcmp(ci->str, "total") == 0)
			return total+1;
		if (strcmp(ci->str, "start") == 0)
			return start < 0 ? Efalse : start + 1;
		if (strcmp(ci->str, "since-start") == 0)
			return since_start < 0 ? Efalse : since_start + 1;
		return Einval;
	}
	if (strcmp(ci->key, "getcapture") == 0 && ci->str) {
		int start, len;
		if (rxl_capture(ss->st, ci->num, ci->num2, &start, &len)) {
			if (strcmp(ci->str, "start") == 0)
				return start + 1;
			if (strcmp(ci->str, "len") == 0)
				return len + 1;
			return Einval;
		}
		return Efalse;
	}
	if (strcmp(ci->key, "interp") == 0 && ci->str) {
		char *ret;
		ret = rxl_interp(ss->st, ci->str);
		comm_call(ci->comm2, "cb", ci->focus, 0, NULL, ret);
		free(ret);
		return 1;
	}
	return Efail;
}

static int search_forward(struct pane *p safe,
			  struct mark *m safe, struct mark *m2,
			  struct mark *point,
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
	ss.point = point;
	ss.c = search_test;
	ss.prev_ch = doc_prior(p, m);
	call_comm("doc:content", p, &ss.c, 0, m, NULL, 0, m2);
	rxl_info(ss.st, &maxlen, NULL, NULL, NULL);
	rxl_free_state(ss.st);
	return maxlen;
}

static int search_backward(struct pane *p safe,
			   struct mark *m safe, struct mark *m2,
			   struct mark *point,
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
	ss.point = point;
	ss.c = search_test;

	pane_set_time(p);
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
		if (pane_too_long(p)) {
			/* FIXME returning success is wrong if we timed out
			 * But I want to move the point, and this is easiest.
			 * What do I really want here?
			 * Do I just need to make reverse search faster?
			 */
			maxlen = 0;
			break;
		}
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
		struct mark *point;

		m = ci->mark;
		endmark = mark_dup(m);
		point = call_ret(mark, "doc:point", ci->focus);
		if (!endmark)
			return Efail;
		if (strcmp(ci->key, "text-match") == 0)
			since_start = search_forward(ci->focus, m, ci->mark2,
						     point, rxl, endmark, True);
		else if (ci->num2)
			since_start = search_backward(ci->focus, m, ci->mark2,
						      point, rxl, endmark);
		else
			since_start = search_forward(ci->focus, m, ci->mark2,
						     point, rxl, endmark, False);

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

DEF_CMD(make_search)
{
	struct search_state *ss;
	unsigned short *rxl;

	if (!ci->str)
		return Enoarg;

	rxl = rxl_parse(ci->str, NULL, ci->num2);
	if (!rxl)
		return Einval;
	ss = calloc(1, sizeof(*ss));
	ss->rxl = rxl;
	ss->c = search_test;
	ss->c.free = state_free;
	command_get(&ss->c);
	comm_call(&ss->c, "reinit", ci->focus,
		  ci->num, ci->mark, NULL, 0, ci->mark2);
	comm_call(ci->comm2, "cb", ci->focus,
		  0, NULL, NULL,
		  0, NULL, NULL, 0,0, &ss->c);
	command_put(&ss->c);
	return 1;
}

struct texteql {
	struct command c;
	const char *text safe;
	bool matched;
};

DEF_CB(equal_test)
{
	struct texteql *te = container_of(ci->comm, struct texteql, c);
	wint_t have, want;
	int i;

	if (!te->text[0])
		return Efalse;
	have = ci->num & 0xFFFFF;
	want = get_utf8(&te->text, NULL);
	if (have != want)
		return Efalse;
	for (i = 0;
	     i < ci->num2 && ci->str;
	     i++)
		if (!te->text[i] || te->text[i] != ci->str[i])
			break;
	te->text += i;
	if (!te->text[0])
		te->matched = True;
	if (ci->str && i < ci->num2)
		/* Stop looking */
		return Efalse;
	return 1 + i;
}

DEF_CMD(text_equals)
{
	struct texteql te;
	struct mark *m;

	if (!ci->str || !ci->mark)
		return Enoarg;

	m = mark_dup(ci->mark);
	te.c = equal_test;
	te.text = ci->str;
	te.matched = False;
	call_comm("doc:content", ci->focus, &te.c, 0, m);
	mark_free(m);
	return te.matched ? 1 : Efalse;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &text_search, 0, NULL,
		  "text-search");
	call_comm("global-set-command", ed, &text_search, 0, NULL,
		  "text-match");
	call_comm("global-set-command", ed, &make_search, 0, NULL,
		  "make-search");
	call_comm("global-set-command", ed, &text_equals, 0, NULL,
		  "text-equals");
}
