/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * line/word/char count.
 *
 * This module can be attached to a Document to count lines/words/chars.
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

static struct map *linecount_map;
DEF_LOOKUP_CMD(handle_count_lines, linecount_map);

struct count_info {
	int view_num;
};

static void do_count(struct pane *p safe, struct mark *start safe, struct mark *end,
		     int *linep safe, int *wordp safe, int *charp safe, int add_marks)
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
	while ((end == NULL || (mark_ordered_not_same_pane(p, m, end))) &&
	       (ch = mark_next_pane(p, m)) != WEOF) {
		chars += 1;
		if (is_eol(ch))
			lines += 1;
		if (!inword && (iswprint(ch) && !iswspace(ch))) {
			inword = 1;
			words += 1;
		} else if (inword && !(iswprint(ch) && !iswspace(ch)))
			inword = 0;
		if (add_marks && lines >= 50 &&
		    (end == NULL || (mark_ordered_not_same_pane(p, m, end)))) {
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

static int need_recalc(struct pane *p safe, struct mark *m)
{
	struct mark *next;
	int ret = 0;
	if (!m)
		return 1;
	if (!attr_find(*mark_attr(m), "lines"))
		ret = 1;
	while (1) {
		next = doc_next_mark_view(m);
		if (!next)
			break;
		if (is_eol(doc_prior_pane(p, next)) &&
		    attr_find_int(*mark_attr(next), "lines") > 10)
			break;
		/* discard next - we'll find or create another */
		mark_free(next);
		ret = 1;
	}
	return ret;
}

static void count_calculate(struct pane *p safe,
			    struct mark *start, struct mark *end,
			    int type)
{
	int lines, words, chars, l, w, c;
	struct mark *m, *m2;

	m = vmark_first(p, type);
	if (m == NULL) {
		/* No marks yet, let's make some */
		m = vmark_new(p, type);
		if (!m)
			return;
		do_count(p, m, NULL, &l, &w, &c, 1);
	}
	if (doc_prior_pane(p, m) != WEOF) {
		/* no mark at start of file */
		m2 = vmark_new(p, type);
		if (!m2)
			return;
		do_count(p, m2, m, &l, &w, &c, 1);
		m = m2;
	}

	if (start) {
		/* find the first mark that isn't before 'start', and count
		 * from there.
		 */
		while (m && mark_ordered_not_same_pane(p, m, start)) {
			/* Force and update to make sure spacing stays sensible */
			if (need_recalc(p, m))
				/* need to update this one */
				do_count(p, m, doc_next_mark_view(m), &l, &w, &c, 1);

			m = doc_next_mark_view(m);
		}
		if (!m) {
			/* fell off the end, just count directly */
			do_count(p, start, end, &lines, &words, &chars, 0);
			goto done;
		}
	}
	if (need_recalc(p, m))
		/* need to update this one */
		do_count(p, m, doc_next_mark_view(m), &l, &w, &c, 1);

	/* 'm' is not before 'start', it might be after.
	 * if 'm' is not before 'end' either, just count from
	 * start to end.
	 */
	if (end && m->seq >= end->seq) {
		do_count(p, start?:m, end, &lines, &words, &chars, 0);
		goto done;
	}

	/* OK, 'm' is between 'start' and 'end'.
	 * So count from start to m, then add totals from m and subsequent.
	 * Then count to 'end'.
	 */
	if (!start || mark_same_pane(p, m, start))
		lines = words = chars = 0;
	else
		do_count(p, start, m, &lines, &words, &chars, 0);
	while ((m2 = doc_next_mark_view(m)) != NULL &&
	       (!end || m2->seq < end->seq)) {
		/* Need everything from m to m2 */
		lines += attr_find_int(*mark_attr(m), "lines");
		words += attr_find_int(*mark_attr(m), "words");
		chars += attr_find_int(*mark_attr(m), "chars");
		m = m2;
		if (need_recalc(p, m))
			do_count(p, m, doc_next_mark_view(m), &l, &w, &c, 1);
	}
	/* m is the last mark before end */
	if (!end) {
		lines += attr_find_int(*mark_attr(m), "lines");
		words += attr_find_int(*mark_attr(m), "words");
		chars += attr_find_int(*mark_attr(m), "chars");
	} else if (!mark_same_pane(p, m, end)) {
		do_count(p, m, end, &l, &w, &c, 0);
		lines += l;
		words += w;
		chars += c;
	}
done:
	if (end) {
		struct attrset **attrs = &end->attrs;
		attr_set_int(attrs, "lines", lines);
		attr_set_int(attrs, "words", words);
		attr_set_int(attrs, "chars", chars);
	} else {
		attr_set_int(&p->attrs, "lines", lines);
		attr_set_int(&p->attrs, "words", words);
		attr_set_int(&p->attrs, "chars", chars);
	}
}

DEF_CMD(linecount_close)
{
	struct pane *p = ci->home;
	struct pane *d = ci->focus;
	struct count_info *cli = p->data;
	struct mark *m;
	while ((m = vmark_first(d, cli->view_num)) != NULL)
		mark_free(m);
	doc_del_view(d, cli->view_num);
	free(cli);
	p->data = safe_cast NULL;
	pane_close(p);
	return 1;
}

DEF_CMD(linecount_notify_replace)
{
	struct pane *p = ci->home;
	struct pane *d = ci->focus;
	struct count_info *cli = p->data;
	if (ci->mark) {
		struct mark *end;

		end = vmark_at_or_before(d, ci->mark, cli->view_num);
		if (end) {
			attr_del(mark_attr(end), "lines");
			attr_del(mark_attr(end), "words");
			attr_del(mark_attr(end), "chars");
		}
		attr_del(&d->attrs, "lines");
		attr_del(&d->attrs, "words");
		attr_del(&d->attrs, "chars");
	}
	return 1;
}

DEF_CMD(linecount_notify_count)
{
	struct pane *p = ci->home;
	struct pane *d = ci->focus;
	struct count_info *cli = p->data;
	/* Option mark is "mark2" as "mark" gets the "point" */
	if (ci->num)
		pane_add_notify(p, d, "Notify:Close");
	count_calculate(d, NULL, ci->mark2, cli->view_num);
	return 1;
}

DEF_CMD(linecount_notify_goto)
{
	struct pane *p = ci->home;
	struct pane *d = ci->focus;
	struct count_info *cli = p->data;
	int lineno, l;
	struct mark *m, *m2;
	wint_t ch;

	if (!ci->mark)
		return 1;

	/* FIXME I might need to recalculate here */
	m = vmark_first(d, cli->view_num);
	if (!m)
		return 1;
	lineno = 1;
	while ((m2 = doc_next_mark_view(m)) != NULL &&
	       (l = attr_find_int(*mark_attr(m), "lines")) >= 0 &&
	       lineno + l < ci->num) {
		m = m2;
		lineno += l;
	}
	mark_to_mark(ci->mark, m);
	while (lineno < ci->num && (ch = mark_next_pane(d, ci->mark)) != WEOF) {
		if (is_eol(ch))
			lineno += 1;
	}
	return 1;
}

DEF_CMD(linecount_clip)
{
	struct count_info *cli = ci->home->data;

	marks_clip(ci->home, ci->mark, ci->mark2, cli->view_num);
	return 1;
}

DEF_CMD(count_lines)
{
	/* FIXME optimise this away most of the time */
	if (call("Notify:doc:CountLines", ci->focus) == -2) {
		/* No counter in place, add one */
		struct count_info *cli;
		struct pane *p;

		cli = calloc(1, sizeof(*cli));
		cli->view_num = doc_add_view(ci->focus);
		p = pane_register(NULL, 0, &handle_count_lines.c, cli, NULL);
		home_call(ci->focus, "Request:Notify:doc:Replace", p);
		home_call(ci->focus, "Request:Notify:doc:CountLines", p);
		home_call(ci->focus, "Request:Notify:doc:GotoLine", p);
		call("Notify:doc:CountLines", ci->focus, 1, ci->mark);
	}
	if (ci->mark) {
		if (ci->str && strcmp(ci->str, "goto:line") == 0 &&
		    ci->num != NO_NUMERIC) {
			call("Notify:doc:GotoLine", ci->focus, ci->num, NULL, NULL,
			     0, ci->mark);
		}
		call("Notify:doc:CountLines", ci->focus, 0, NULL, NULL,
		     0, ci->mark);
	}
	if (ci->mark2)
		call("Notify:doc:CountLines", ci->focus, 0, NULL, NULL,
		      0, ci->mark2);
	return 1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &count_lines, 0, NULL, "CountLines");

	if (linecount_map)
		return;

	linecount_map = key_alloc();
	key_add(linecount_map, "Notify:Close", &linecount_close);
	key_add(linecount_map, "Notify:doc:Replace", &linecount_notify_replace);
	key_add(linecount_map, "Notify:doc:CountLines", &linecount_notify_count);
	key_add(linecount_map, "Notify:doc:GotoLine", &linecount_notify_goto);
	key_add(linecount_map, "Notify:clip", &linecount_clip);
}
