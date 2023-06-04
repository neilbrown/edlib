/*
 * Copyright Neil Brown ©2015-2022 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * line/word/char count.
 *
 * This module can be attached to a Document to count lines/words/chars.
 *
 * It attaches an active mark at the start, then one every 100 lines or so
 * and records the counts between the marks.  These are stored as attributes
 * 'lines' 'words' 'chars' on the mark at the start of the range.
 * When a change is notified, the attributes one the preceeding
 * mark are cleared.
 * When a count is requested, all marks from top-of-file to target
 * are examined.  If attributes are not present they are calculated.
 * Then they are summed.
 * The text from the last active mark at the target is always calculated.
 *
 * When recalculating a range, we drop a new mark every 100 lines.
 * When we find a mark the needs updating, we discard it if previous mark is
 * closer than 10 lines.
 *
 * When CountLines is called on a doc-pane, pane attributes are set
 * to record the number of lines, words, chars.
 * When it is called on a mark in the pane, attributes are set on the
 * mark to indicate the line, work and char where the mark is.
 * These are always at least 1.
 */

#include <unistd.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>
#include <string.h>

#include "core.h"

static struct map *linecount_map;
DEF_LOOKUP_CMD(handle_count_lines, linecount_map);

static const int batch_marks = 1;
static const int batch_timer = 20;
static bool testing = False;

struct count_info {
	int view_num;
};

struct clcb {
	int lines, words, chars;
	int inword;
	int *linep safe, *wordp safe, *charp safe;
	int add_marks;
	struct mark *start;
	struct mark *end;
	struct command c;
	struct pane *owner safe;
};

DEF_CB(clcb)
{
	struct clcb *cl = container_of(ci->comm, struct clcb, c);
	wint_t ch = ci->num;
	struct mark *m = ci->mark;
	struct count_info *cli = cl->owner->data;
	const char *s;
	int i = 0;

	if (!m)
		return Enoarg;

	while (1) {
		cl->chars += 1;
		if (is_eol(ch))
			cl->lines += 1;
		if (!cl->inword && (iswprint(ch) && !iswspace(ch))) {
			cl->inword = 1;
			cl->words += 1;
		} else if (cl->inword && !(iswprint(ch) && !iswspace(ch)))
			cl->inword = 0;
		if (cl->add_marks &&
		    cl->start &&
		    (cl->lines >= 100 || cl->words >= 1000 || cl->chars >= 10000))
			break;
		if (!ci->str || i >= ci->num2)
			return i+1;
		s = ci->str + i;
		ch = get_utf8(&s, ci->str + ci->num2);
		if (ch == WEOF || ch == WERR)
			return i+1;
		i = s - ci->str;
	}

	if (i > 0) {
		/* m isn't where we are, so we cannot update
		 * anything yet - need to return an get called again
		 */
		return i+1;
	}
	attr_set_int(mark_attr(cl->start), "lines", cl->lines);
	attr_set_int(mark_attr(cl->start), "words", cl->words);
	attr_set_int(mark_attr(cl->start), "chars", cl->chars);
	*cl->linep += cl->lines;
	*cl->wordp += cl->words;
	*cl->charp += cl->chars;
	cl->lines = 0;
	cl->words = 0;
	cl->chars = 0;
	cl->start = vmark_new(ci->focus, cli->view_num, cl->owner);
	if (cl->start)
		mark_to_mark(cl->start, m);
	cl->add_marks -= 1;
	if (!cl->add_marks)
		/* Added enough marks, abort */
		return Efalse;
	return 1;
}

static void do_count(struct pane *p safe, struct pane *owner safe,
		     struct mark *start safe, struct mark *end,
		     int *linep safe, int *wordp safe, int *charp safe, int add_marks)
{
	/* if 'end' is NULL, go all the way to EOF */
	struct clcb cl;

	cl.lines = 0;
	cl.words = 0;
	cl.chars = 0;
	cl.inword = 0;
	cl.end = end;
	cl.start = start;
	cl.add_marks = add_marks;
	cl.c = clcb;
	cl.linep = linep;
	cl.wordp = wordp;
	cl.charp = charp;
	cl.owner = owner;

	*linep = 0;
	*wordp = 0;
	*charp = 0;
	if (call_comm("doc:content", p, &cl.c, 0, start, NULL, 0, end) <= 0 ||
	    (add_marks && cl.add_marks == 0))
		return;

	if (cl.add_marks && cl.start && cl.start != start && cl.chars == 0) {
		mark_free(cl.start);
		cl.start = NULL;
	}
	if (cl.add_marks && cl.start) {
		attr_set_int(mark_attr(cl.start), "lines", cl.lines);
		attr_set_int(mark_attr(cl.start), "words", cl.words);
		attr_set_int(mark_attr(cl.start), "chars", cl.chars);
	}
	*linep += cl.lines;
	*wordp += cl.words;
	*charp += cl.chars;
}

DEF_CMD(linecount_restart)
{
	pane_notify("doc:CountLines", ci->focus, 1);
	return Efalse;
}

static int need_recalc(struct pane *p safe, struct mark *m)
{
	struct mark *next;
	int ret = 0;
	if (!m)
		return 1;
	if (!attr_find(*mark_attr(m), "lines"))
		ret = 1;
	next = vmark_next(m);
	if (next && attr_find_int(*mark_attr(m), "lines") < 20) {
		/* This is tiny, recalc */
		attr_del(mark_attr(m), "lines");
		mark_free(next);
		ret = 1;
	}
	if (ret)
		/* The background task needs to be stopped */
		call_comm("event:free", p, &linecount_restart);
	return ret;
}

static void count_calculate(struct pane *p safe,
			    struct mark *end,
			    struct pane *owner safe, int type,
			    bool sync)
{
	int lines, words, chars, l, w, c;
	struct mark *m, *m2;
	char *disable;

	if (testing)
		sync = True;

	disable = pane_attr_get(p, "linecount-disable");
	if (disable && strcmp(disable, "yes") == 0) {
		if (end) {
			attr_set_str(&end->attrs, "line", "??");
			attr_set_str(&end->attrs, "word", "??");
			attr_set_str(&end->attrs, "char", "??");
		}
		attr_set_str(&p->attrs, "lines", "-");
		attr_set_str(&p->attrs, "words", "-");
		attr_set_str(&p->attrs, "chars", "-");
		return;
	}

	if (!end && attr_find(p->attrs, "lines"))
		/* nothing to do */
		return;

	m = vmark_first(p, type, owner);
	if (m == NULL || doc_prior(p, m) != WEOF) {
		/* No mark at doc start, make some */
		m = vmark_new(p, type, owner);
		if (!m)
			return;
		call("doc:set-ref", p, 1, m);
		do_count(p, owner, m, vmark_next(m), &l, &w, &c, sync ? -1 : batch_marks);
		if (!sync) {
			call_comm("event:timer", p, &linecount_restart, batch_timer, end);
			return;
		}
	}

	if (need_recalc(p, m)) {
		/* need to update this one */
		do_count(p, owner, m, vmark_next(m), &l, &w, &c, sync ? -1 : batch_marks);
		if (!sync) {
			call_comm("event:timer", p, &linecount_restart, batch_timer, end);
			return;
		}
	}
	/* Add totals from m to before end. Then count to 'end'.
	 */
	lines = words = chars = 0;
	while ((m2 = vmark_next(m)) != NULL &&
	       (!end || m2->seq < end->seq)) {
		/* Need everything from m to m2 */
		lines += attr_find_int(*mark_attr(m), "lines");
		words += attr_find_int(*mark_attr(m), "words");
		chars += attr_find_int(*mark_attr(m), "chars");
		m = m2;
		if (!need_recalc(p, m))
			continue;
		do_count(p, owner, m, vmark_next(m), &l, &w, &c, sync ? -1 : batch_marks);
		if (!sync) {
			call_comm("event:timer", p, &linecount_restart, batch_timer, end);
			return;
		}
	}
	/* m is the last mark before end */
	if (!end) {
		lines += attr_find_int(*mark_attr(m), "lines");
		words += attr_find_int(*mark_attr(m), "words");
		chars += attr_find_int(*mark_attr(m), "chars");
	} else if (!mark_same(m, end)) {
		do_count(p, owner, m, end, &l, &w, &c, 0);
		lines += l;
		words += w;
		chars += c;
	}

	if (end) {
		struct attrset **attrs = &end->attrs;
		attr_set_int(attrs, "line", lines + 1);
		attr_set_int(attrs, "word", words + 1);
		attr_set_int(attrs, "char", chars + 1);
	} else {
		attr_set_int(&p->attrs, "lines", lines);
		attr_set_int(&p->attrs, "words", words);
		attr_set_int(&p->attrs, "chars", chars);
		if (!testing)
			pane_notify("doc:status-changed", p);
	}
}

DEF_CMD(linecount_close)
{
	struct pane *d = ci->focus;
	struct count_info *cli = ci->home->data;
	struct mark *m;

	call_comm("event:free", d, &linecount_restart);
	while ((m = vmark_first(d, cli->view_num, ci->home)) != NULL)
		mark_free(m);
	home_call(d, "doc:del-view", ci->home, cli->view_num);
	pane_close(ci->home);
	return 1;
}

DEF_CMD(linecount_notify_replace)
{
	struct pane *d = ci->focus;
	struct count_info *cli = ci->home->data;
	struct mark *m, *m2;

	attr_del(&d->attrs, "lines");
	attr_del(&d->attrs, "words");
	attr_del(&d->attrs, "chars");

	if (ci->mark)
		m = vmark_at_or_before(d, ci->mark, cli->view_num, ci->home);
	else
		m = vmark_first(d, cli->view_num, ci->home);
	if (!m)
		return 1;

	attr_del(mark_attr(m), "lines");
	attr_del(mark_attr(m), "words");
	attr_del(mark_attr(m), "chars");

	while ((m2 = vmark_next(m)) != NULL &&
	       (!ci->mark2 || mark_ordered_or_same(m2, ci->mark2)))
		mark_free(m2);

	call_comm("event:free", d, &linecount_restart);
	return 1;
}

DEF_CMD(linecount_notify_count)
{
	struct pane *d = ci->focus;
	struct count_info *cli = ci->home->data;
	/* Option mark is "mark2" as "mark" gets the "point" */
	/* num==1 means we don't want to wait for precision */
	count_calculate(d, ci->mark2, ci->home, cli->view_num,
			ci->mark2 && ci->num != 1);
	return 1;
}

DEF_CMD(linecount_notify_goto)
{
	struct pane *d = ci->focus;
	struct count_info *cli = ci->home->data;
	int lineno, l;
	struct mark *m, *m2;
	wint_t ch;

	if (!ci->mark)
		return 1;

	/* Ensure counts are up-to-date */
	count_calculate(d, NULL, ci->home, cli->view_num, True);
	m = vmark_first(d, cli->view_num, ci->home);
	if (!m)
		return 1;
	lineno = 1;
	while ((m2 = vmark_next(m)) != NULL &&
	       (l = attr_find_int(*mark_attr(m), "lines")) >= 0 &&
	       lineno + l < ci->num) {
		m = m2;
		lineno += l;
	}
	mark_to_mark(ci->mark, m);
	if (lineno == ci->num) {
		/* might not be at start of line */
		while ((ch = doc_prior(d, ci->mark)) != WEOF &&
		       !is_eol(ch))
			doc_prev(d, ci->mark);
	}
	while (lineno < ci->num && (ch = doc_next(d, ci->mark)) != WEOF) {
		if (is_eol(ch))
			lineno += 1;
	}
	return 1;
}

DEF_CMD(count_lines)
{
	int async = strcmp(ci->key, "CountLinesAsync") == 0;

	/* FIXME optimise this away most of the time */
	if (call("doc:notify:doc:CountLines", ci->focus, 1) == 0) {
		/* No counter in place, add one */
		struct count_info *cli;
		struct pane *p;

		alloc(cli, pane);
		p = pane_register(NULL, 0, &handle_count_lines.c, cli);
		if (!p)
			return Efail;
		cli->view_num = home_call(ci->focus, "doc:add-view", p) - 1;
		home_call(ci->focus, "doc:request:doc:replaced", p);
		home_call(ci->focus, "doc:request:doc:CountLines", p);
		home_call(ci->focus, "doc:request:doc:GotoLine", p);
		home_call(ci->focus, "doc:request:Notify:Close", p);
		call("doc:notify:doc:CountLines", ci->focus, 1);
	}
	if (ci->mark) {
		if (ci->str && strcmp(ci->str, "goto:line") == 0 &&
		    ci->num != NO_NUMERIC) {
			call("doc:notify:doc:GotoLine", ci->focus, ci->num, NULL, NULL,
			     0, ci->mark);
		}
		call("doc:notify:doc:CountLines", ci->focus, async, NULL, NULL,
		     0, ci->mark);
	}
	if (ci->mark2)
		call("doc:notify:doc:CountLines", ci->focus, async, NULL, NULL,
		     0, ci->mark2);
	return 1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &count_lines, 0, NULL, "CountLines");
	call_comm("global-set-command", ed, &count_lines, 0, NULL, "CountLinesAsync");

	if (getenv("EDLIB_TESTING"))
		testing = True;

	if (linecount_map)
		return;

	linecount_map = key_alloc();
	key_add(linecount_map, "Notify:Close", &linecount_close);
	key_add(linecount_map, "doc:replaced", &linecount_notify_replace);
	key_add(linecount_map, "doc:CountLines", &linecount_notify_count);
	key_add(linecount_map, "doc:GotoLine", &linecount_notify_goto);
	key_add(linecount_map, "Free", &edlib_do_free);
}
