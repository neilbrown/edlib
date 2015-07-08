/*
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

#include "text.h"
#include "mark.h"
#include "attr.h"
#include "pane.h"
#include "keymap.h"

static void do_count(struct text *t, struct mark *start, struct mark *end,
		     int *linep, int *wordp, int *charp, int add_marks)
{
	/* if 'end' is NULL, go all the way to EOF */
	int lines = 0;
	int words = 0;
	int chars = 0;
	int inword = 0;
	wint_t ch;
	struct mark *m = mark_dup(start, !add_marks);

	*linep = 0;
	*wordp = 0;
	*charp = 0;
	while ((end == NULL || (mark_ordered(m, end) && !mark_same(t, m, end))) &&
	       (ch = mark_next(t, m)) != WEOF) {
		chars += 1;
		if (ch == '\n')
			lines += 1;
		if (!inword && (iswprint(ch) && !iswspace(ch))) {
			inword = 1;
			words += 1;
		} else if (inword && !(iswprint(ch) && !iswspace(ch)))
			inword = 0;
		if (add_marks && lines >= 50 &&
		    (end == NULL || (mark_ordered(m, end) && !mark_same(t, m, end)))) {
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

static int count_notify(struct command *c, struct cmd_info *ci)
{
	if (ci->key != EV_REPLACE)
		return 0;

	if (ci->mark != NULL) {
		attr_del(mark_attr(ci->mark), "lines");
		attr_del(mark_attr(ci->mark), "words");
		attr_del(mark_attr(ci->mark), "chars");
	}
	return 1;
}
static struct command count_cmd = {count_notify, "count-notify", NULL};

static int need_recalc(struct text *t, struct mark *m)
{
	struct mark *next;
	int ret = 0;
	if (!m)
		return 1;
	if (!attr_find(*mark_attr(m), "lines"))
		ret = 1;
	while (1) {
		next = text_next_mark(t, m);
		if (!next)
			break;
		if (mark_prior(t, next) == '\n' &&
		    attr_find_int(*mark_attr(next), "lines") > 10)
			break;
		/* discard next - we'll find or create another */
		mark_free(next);
		ret = 1;
	}
	return ret;
}

int count_calculate(struct text *t, struct mark *start, struct mark *end,
		    int *linep, int *wordp, int *charp)
{
	int type = text_find_type(t, &count_cmd);
	int lines, words, chars, l, w, c;
	struct mark *m, *m2;

	if (type < 0)
		type = text_add_type(t, &count_cmd);

	m = text_first_mark(t, type);
	if (m == NULL) {
		/* No marks yet, let's make some */
		m = text_new_mark(t, type);
		do_count(t, m, NULL, &l, &w, &c, 1);
	}
	if (mark_prior(t, m) != WEOF) {
		/* no mark at start of file */
		m2 = text_new_mark(t, type);
		do_count(t, m2, m, &l, &w, &c, 1);
		m = m2;
	}

	if (start) {
		/* find the first mark that isn't before 'start', and count
		 * from there.
		 */
		while (m && mark_ordered(m, start) && !mark_same(t, m, start)) {
			/* Force and update to make sure spacing stays sensible */
			if (need_recalc(t, m))
				/* need to update this one */
				do_count(t, m, text_next_mark(t, m), &l, &w, &c, 1);

			m = text_next_mark(t, m);
		}
		if (!m) {
			/* fell off the end, just count directly */
			do_count(t, start, end, linep, wordp, charp, 0);
			return 1;
		}
	}
	if (need_recalc(t, m))
		/* need to update this one */
		do_count(t, m, text_next_mark(t, m), &l, &w, &c, 1);

	/* 'm' is not before 'start', it might be after.
	 * if 'm' is not before 'end' either, just count from
	 * start to end.
	 */
	if (end && !mark_ordered(m, end)) {
		do_count(t, start, end, linep, wordp, charp, 0);
		return 0;
	}

	/* OK, 'm' is between 'start' and 'end'.
	 * So count from start to m, then add totals from m and subsequent.
	 * Then count to 'end'.
	 */
	if (!start || mark_same(t, m, start))
		lines = words = chars = 0;
	else
		do_count(t, start, m, &lines, &words, &chars, 0);
	while ((m2 = text_next_mark(t, m)) != NULL &&
	       (!end || mark_ordered(m2, end))) {
		/* Need everything from m to m2 */
		lines += attr_find_int(*mark_attr(m), "lines");
		words += attr_find_int(*mark_attr(m), "words");
		chars += attr_find_int(*mark_attr(m), "chars");
		m = m2;
		if (need_recalc(t, m))
			do_count(t, m, text_next_mark(t, m), &l, &w, &c, 1);
	}
	/* m is the last mark before end */
	if (!end) {
		lines += attr_find_int(*mark_attr(m), "lines");
		words += attr_find_int(*mark_attr(m), "words");
		chars += attr_find_int(*mark_attr(m), "chars");
	} else if (!mark_same(t, m, end)) {
		do_count(t, m, end, &l, &w, &c, 0);
		lines += l;
		words += w;
		chars += c;
	}
	*linep = lines;
	*wordp = words;
	*charp = chars;
	return 1;
}
