/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * line/word/char count.
 *
 * This module can be attached to a text to count lines/words/chars.
 *
 * It attaches active marks every 50 lines or so and records the
 * counts between the marks.  These are stored as attributes
 * 'lines' 'words' 'chars'.
 * When a change is notified, the attributes are cleared.
 * When a count is requested, all marks from top-of-file to target
 * are examined.  If attributes are not present they are calculated.
 * Then they are summed.
 * The text from the last active mark at the target is always calculated.
 *
 * When recalculating a range, we drop a new mark every 50 lines.
 * When we find a mark the needs updating, we discard it if previous mark is
 * closer than 10 lines.
 */

#include <unistd.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>
#include <string.h>

#include "core.h"

static void do_count(struct doc *d, struct mark *start, struct mark *end,
		     int *linep, int *wordp, int *charp, int add_marks)
{
	/* if 'end' is NULL, go all the way to EOF */
	int lines = 0;
	int words = 0;
	int chars = 0;
	int inword = 0;
	wint_t ch;
	struct mark *m;

	m = mark_dup(start, !add_marks);

	*linep = 0;
	*wordp = 0;
	*charp = 0;
	while ((end == NULL || (mark_ordered_not_same(d, m, end))) &&
	       (ch = mark_next(d, m)) != WEOF) {
		chars += 1;
		if (ch == '\n')
			lines += 1;
		if (!inword && (iswprint(ch) && !iswspace(ch))) {
			inword = 1;
			words += 1;
		} else if (inword && !(iswprint(ch) && !iswspace(ch)))
			inword = 0;
		if (add_marks && lines >= 50 &&
		    (end == NULL || (mark_ordered_not_same(d, m, end)))) {
			/* leave a mark here and keep going */
			attr_set_int(mark_attr(start), "lines", lines);
			attr_set_int(mark_attr(start), "words", words);
			attr_set_int(mark_attr(start), "chars", chars);
			start = m;
			*linep += lines;
			*wordp += words;
			*charp += chars;
			lines = words = chars = 0;
			m = mark_dup(m, 0);
		}
	}
	if (add_marks) {
		attr_set_int(mark_attr(start), "lines", lines);
		attr_set_int(mark_attr(start), "words", words);
		attr_set_int(mark_attr(start), "chars", chars);
	}
	*linep += lines;
	*wordp += words;
	*charp += chars;
	mark_free(m);
}

DEF_CMD(count_notify)
{
	if (strcmp(ci->key, "Notify:Replace") == 0) {
		if (ci->mark != NULL) {
			attr_del(mark_attr(ci->mark), "lines");
			attr_del(mark_attr(ci->mark), "words");
			attr_del(mark_attr(ci->mark), "chars");
		}
		return 1;
	}
	if (strcmp(ci->key, "Release") == 0) {
		struct mark *m;
		int i = doc_find_view(ci->home, ci->comm);
		if (i < 0)
			return 0;
		while ((m = vmark_first(ci->home, i)) != NULL)
			mark_free(m);
		doc_del_view(ci->home, ci->comm);
	}
	return 0;
}

static int need_recalc(struct doc *d, struct mark *m)
{
	struct mark *next;
	int ret = 0;
	if (!m)
		return 1;
	if (!attr_find(*mark_attr(m), "lines"))
		ret = 1;
	while (1) {
		next = doc_next_mark(m);
		if (!next)
			break;
		if (doc_prior(d, next) == '\n' &&
		    attr_find_int(*mark_attr(next), "lines") > 10)
			break;
		/* discard next - we'll find or create another */
		mark_free(next);
		ret = 1;
	}
	return ret;
}

static void count_calculate(struct doc *d, struct mark *start, struct mark *end)
{
	int type = doc_find_view(d->home, &count_notify);
	int lines, words, chars, l, w, c;
	struct mark *m, *m2;
	struct attrset **attrs;

	if (type < 0)
		type = doc_add_view(d->home, &count_notify, 0);

	m = vmark_first(d->home, type);
	if (m == NULL) {
		/* No marks yet, let's make some */
		m = doc_new_mark(d, type);
		do_count(d, m, NULL, &l, &w, &c, 1);
	}
	if (doc_prior(d, m) != WEOF) {
		/* no mark at start of file */
		m2 = doc_new_mark(d, type);
		do_count(d, m2, m, &l, &w, &c, 1);
		m = m2;
	}

	if (start) {
		/* find the first mark that isn't before 'start', and count
		 * from there.
		 */
		while (m && mark_ordered_not_same(d, m, start)) {
			/* Force and update to make sure spacing stays sensible */
			if (need_recalc(d, m))
				/* need to update this one */
				do_count(d, m, doc_next_mark(m), &l, &w, &c, 1);

			m = doc_next_mark(m);
		}
		if (!m) {
			/* fell off the end, just count directly */
			do_count(d, start, end, &lines, &words, &chars, 0);
			goto done;
		}
	}
	if (need_recalc(d, m))
		/* need to update this one */
		do_count(d, m, doc_next_mark(m), &l, &w, &c, 1);

	/* 'm' is not before 'start', it might be after.
	 * if 'm' is not before 'end' either, just count from
	 * start to end.
	 */
	if (end && !mark_ordered(m, end)) {
		do_count(d, start?:m, end, &lines, &words, &chars, 0);
		goto done;
	}

	/* OK, 'm' is between 'start' and 'end'.
	 * So count from start to m, then add totals from m and subsequent.
	 * Then count to 'end'.
	 */
	if (!start || mark_same(d, m, start))
		lines = words = chars = 0;
	else
		do_count(d, start, m, &lines, &words, &chars, 0);
	while ((m2 = doc_next_mark(m)) != NULL &&
	       (!end || mark_ordered(m2, end))) {
		/* Need everything from m to m2 */
		lines += attr_find_int(*mark_attr(m), "lines");
		words += attr_find_int(*mark_attr(m), "words");
		chars += attr_find_int(*mark_attr(m), "chars");
		m = m2;
		if (need_recalc(d, m))
			do_count(d, m, doc_next_mark(m), &l, &w, &c, 1);
	}
	/* m is the last mark before end */
	if (!end) {
		lines += attr_find_int(*mark_attr(m), "lines");
		words += attr_find_int(*mark_attr(m), "words");
		chars += attr_find_int(*mark_attr(m), "chars");
	} else if (!mark_same(d, m, end)) {
		do_count(d, m, end, &l, &w, &c, 0);
		lines += l;
		words += w;
		chars += c;
	}
done:
	if (end)
		attrs = &end->attrs;
	else
		attrs = &d->attrs;
	attr_set_int(attrs, "lines", lines);
	attr_set_int(attrs, "words", words);
	attr_set_int(attrs, "chars", chars);
}

DEF_CMD(count_lines)
{
	struct doc *d = doc_from_pane(ci->home);

	/* FIXME optimise this away most of the time */
	count_calculate(d, NULL, NULL);
	if (ci->mark)
		count_calculate(d, NULL, ci->mark);
	if (ci->mark2)
		count_calculate(d, NULL, ci->mark2);
	return 1;
}

void edlib_init(struct editor *ed)
{
	key_add(ed->commands, "CountLines", &count_lines);
}
