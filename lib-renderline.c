/*
 * Copyright Neil Brown ©2016 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * This module provides render-line and render-line-prev, making use of
 * the chars returned by doc:step.
 * A line is normally text ending with a newline.  However if no newline
 * is found in a long distance, we drop a mark and use that as the start
 * of a line.
 * A vertial tab '\v' acts like a newline but forces it to be a blank line.
 * A "\v" immediately after "\m" or "\v" is exactly like a newline, while
 * "\v" after anything else terminates the line without consuming the newline.
 */

#include <unistd.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>
#include <string.h>

#include <stdio.h>

#include "core.h"
#include "misc.h"

struct rl_info {
	int	view;
};

static struct map *rl_map safe;

#define LARGE_LINE 1000

DEF_CMD(render_prev)
{
	/* In the process of rendering a line we need to find the
	 * start of line.  We use a mark to create an artificial
	 * start-of-line where none can be found.
	 * Search backwards until a newline or start-of-file or the
	 * mark is found.  Move backwards at most LARGE_LINE characters
	 * and if nothing else is found, put a mark there and treat as s-o-l.
	 *
	 * If RPT_NUM == 1, step back at least one character so we get
	 * the previous line and not the line we are on.
	 * If we hit start-of-file without finding newline, return Efail;
	 */
	struct mark *m = ci->mark;
	struct pane *f = ci->focus;
	struct rl_info *rl = ci->home->data;
	struct mark *boundary = NULL;
	int count = 0;
	int rpt = RPT_NUM(ci);
	wint_t ch;

	if (!m)
		return Enoarg;

	while ((ch = mark_prev_pane(f, m)) != WEOF &&
	       (!is_eol(ch) || rpt > 0) &&
	       count < LARGE_LINE &&
	       (!boundary || boundary->seq< m->seq)) {
		rpt = 0;
		if (!count)
			boundary = vmark_at_or_before(f, m, rl->view);
		count += 1;
	}
	if (ch != WEOF && !is_eol(ch)) {
		/* need to ensure there is a stable boundary here */
		if (!boundary || boundary->seq >= m->seq) {
			boundary = vmark_new(f, rl->view);
			if (boundary)
				mark_to_mark(boundary, m);
		}
		return 1;
	}
	if (ch == WEOF && rpt)
		return Efail;
	if (ch == '\n' || (ch == '\v' &&
			   ((ch = doc_prior_pane(f, m)) == WEOF || !is_eol(ch))))
		/* Found a '\n', so step forward over it for start-of-line. */
		mark_next_pane(f, m);
	return 1;
}

struct attr_stack {
	struct attr_stack	*next;
	char			*attr safe;
	int			end;
	int			priority;
};

static int find_finished(struct attr_stack *st, int pos, int *nextp safe)
{
	int depth = 0;
	int fdepth = -1;
	int next = -1;

	for (; st ; st = st->next, depth++) {
		if (st->end <= pos)
			fdepth = depth;
		else if (next < 0 || next > st->end)
			next = st->end;
	}
	*nextp = next;
	return fdepth;
}

static void as_pop(struct attr_stack **fromp safe, struct attr_stack **top safe, int depth,
	    struct buf *b safe)
{
	struct attr_stack *from = *fromp;
	struct attr_stack *to = *top;

	while (from && depth >= 0) {
		struct attr_stack *t;
		buf_concat(b, "</>");
		t = from;
		from = t->next;
		t->next = to;
		to = t;
		depth -= 1;
	}
	*fromp = from;
	*top = to;
}

static void as_repush(struct attr_stack **fromp safe, struct attr_stack **top safe,
		      int pos, struct buf *b safe)
{
	struct attr_stack *from = *fromp;
	struct attr_stack *to = *top;

	while (from) {
		struct attr_stack *t = from->next;
		if (from->end <= pos) {
			free(from->attr);
			free(from);
		} else {
			buf_append(b, '<');
			buf_concat(b, from->attr);
			buf_append(b, '>');
			from->next = to;
			to = from;
		}
		from = t;
	}
	*fromp = from;
	*top = to;
}

static void as_add(struct attr_stack **fromp safe, struct attr_stack **top safe,
		   int end, int prio, char *attr safe)
{
	struct attr_stack *from = *fromp;
	struct attr_stack *to = *top;
	struct attr_stack *new, **here;

	while (from && from->priority > prio) {
		struct attr_stack *t = from->next;
		from->next = to;
		to = from;
		from = t;
	}
	here = &to;
	while (*here && (*here)->priority <= prio)
		here = &(*here)->next;
	new = calloc(1, sizeof(*new));
	new->next = *here;
	new->attr = strdup(attr);
	new->end = end;
	new->priority = prio;
	*here = new;
	*top = to;
	*fromp = from;
}

static void as_clear(struct attr_stack **fromp safe, struct attr_stack **top safe,
		     int end, int prio, char *attr safe)
{
	struct attr_stack *from = *fromp;

	while (from && from->priority >= prio) {
		if (from->priority == prio && strcmp(from->attr, attr) == 0)
			if (from->end >= end)
				from->end = end;
		from = from->next;
	}
}

struct attr_return {
	struct command rtn;
	struct command fwd;
	struct attr_stack *ast, *tmpst;
	int min_end;
	int chars;
};

DEF_CMD(text_attr_forward)
{
	struct attr_return *ar = container_of(ci->comm, struct attr_return, fwd);
	if (!ci->str || !ci->str2)
		return 0;
	return call_comm("map-attr", ci->focus, &ar->rtn, 0, ci->mark, ci->str2,
			 0, NULL, ci->str);
}

DEF_CMD(text_attr_callback)
{
	struct attr_return *ar = container_of(ci->comm, struct attr_return, rtn);
	if (!ci->str)
		return Enoarg;
	if (ci->num >= 0)
		as_add(&ar->ast, &ar->tmpst, ar->chars + ci->num, ci->num2, ci->str);
	else
		as_clear(&ar->ast, &ar->tmpst, ar->chars, ci->num2, ci->str);
	if (ar->min_end < 0 || ar->chars + ci->num < ar->min_end)
		ar->min_end = ar->chars + ci->num;
	// FIXME ->str2 should be inserted
	return 1;
}

static void call_map_mark(struct pane *f safe, struct mark *m safe,
			  struct attr_return *ar safe)
{
	char *key = "render:";
	char *val;

	while ((key = attr_get_next_key(m->attrs, key, -1, &val)) != NULL &&
	       strncmp(key, "render:", 7) == 0)
		call_comm("map-attr", f, &ar->rtn, 0, m, key, 0, NULL, val);
}

DEF_CMD(render_line)
{
	/* Render the line from 'mark' to the first '\n' or until
	 * 'num2' chars.
	 * Convert '<' to '<<' and if a char has the 'highlight' attribute,
	 * include that between '<>'.
	 */
	struct buf b;
	struct pane *focus = ci->focus;
	struct rl_info *rl = ci->home->data;
	struct mark *m = ci->mark;
	struct mark *pm = ci->mark2; /* The location to render as cursor */
	struct mark *boundary;
	int o = ci->num;
	wint_t ch;
	int chars = 0;
	int ret;
	struct attr_return ar;
	int add_newline = 0;
	char *attr;

	if (o == NO_NUMERIC)
		o = -1;

	ar.rtn = text_attr_callback;
	ar.fwd = text_attr_forward;
	ar.ast = ar.tmpst = NULL;
	ar.min_end = -1;

	if (!m)
		return Enoarg;

	ch = doc_following_pane(focus, m);
	if (is_eol(ch) &&
	    (attr = pane_mark_attr(focus, m, "renderline:func")) != NULL) {
		/* An alternate function handles this line */
		ret = call_comm(attr, focus, ci->comm2, o, m, NULL, ci->num2, pm);
		if (ret)
			return ret;
	}
	boundary = vmark_at_or_before(focus, m, rl->view);
	if (boundary)
		boundary = vmark_next(boundary);
	buf_init(&b);
	call_comm("map-attr", focus, &ar.rtn, 0, m, "start-of-line");
	while (1) {
		struct mark *m2;

		if (o >= 0 && b.len >= o)
			break;
		if (pm && mark_same(m, pm))
			break;

		if (ar.ast && ar.min_end <= chars) {
			int depth = find_finished(ar.ast, chars, &ar.min_end);
			as_pop(&ar.ast, &ar.tmpst, depth, &b);
		}

		ar.chars = chars;
		call_comm("doc:get-attr", focus, &ar.fwd, 0, m, "render:", 1);

		/* find all marks "here" - they might be fore or aft */
		for (m2 = doc_prev_mark_all(m); m2 && mark_same(m, m2);
		     m2 = doc_prev_mark_all(m2))
			call_map_mark(focus, m2, &ar);
		for (m2 = doc_next_mark_all(m); m2 && mark_same(m, m2);
		     m2 = doc_next_mark_all(m2))
			call_map_mark(focus, m2, &ar);

		as_repush(&ar.tmpst, &ar.ast, chars, &b);

		if (o >= 0 && b.len >= o)
			break;

		ch = mark_next_pane(focus, m);
		if (ch == WEOF)
			break;
		if (is_eol(ch)) {
			add_newline = 1;
			if (ch == '\v' && b.len > 0)
				mark_prev_pane(focus, m);
			break;
		}
		if (boundary && boundary->seq <= m->seq)
			break;
		if (ch == '<') {
			if (o >= 0 && b.len+1 >= o) {
				mark_prev_pane(focus, m);
				break;
			}
			buf_append(&b, '<');
		}
		if (ch < ' ' && ch != '\t' && !is_eol(ch)) {
			buf_concat(&b, "<fg:red>^");
			buf_append(&b, '@' + ch);
			buf_concat(&b, "</>");
		} else if (ch == 0x7f) {
			buf_concat(&b, "<fg:red>^?</>");
		} else
			buf_append(&b, ch);
		chars++;
	}
	while (ar.ast)
		as_pop(&ar.ast, &ar.tmpst, 100, &b);
	as_repush(&ar.tmpst, &ar.ast, 10000000, &b);
	if (add_newline) {
		if (o >= 0 && b.len >= o)
			/* skip the newline */
			mark_prev_pane(focus, m);
		else
			buf_append(&b, '\n');
	}

	ret = comm_call(ci->comm2, "callback:render", focus, 0, NULL,
			buf_final(&b));
	free(b.b);
	return ret;
}

DEF_LOOKUP_CMD(renderline_handle, rl_map);

static struct pane *do_renderline_attach(struct pane *p safe)
{
	struct pane *ret;
	struct rl_info *rl = calloc(1, sizeof(*rl));

	rl->view = call("doc:add-view", p) - 1;
	ret = pane_register(p, 0, &renderline_handle.c, rl, NULL);

	return ret;
}

DEF_CMD(renderline_attach)
{
	struct pane *ret;

	ret = do_renderline_attach(ci->focus);
	if (!ret)
		return Esys;
	return comm_call(ci->comm2, "callback:attach", ret);
}

DEF_CMD(rl_clone)
{
	struct pane *parent = ci->focus;
	struct pane *child = do_renderline_attach(parent);
	pane_clone_children(ci->home, child);
	return 1;
}

DEF_CMD(rl_clip)
{
	struct rl_info *rl = ci->home->data;

	marks_clip(ci->home, ci->mark, ci->mark2, rl->view);
	return 0;
}

DEF_CMD(rl_close)
{
	struct pane *p = ci->home;
	struct rl_info *rl = p->data;
	struct mark *m;
	while ((m = vmark_first(p, rl->view)) != NULL)
		mark_free(m);
	call("doc:del-view", p, rl->view);
	free(rl);
	p->data = safe_cast NULL;
	p->handle = NULL;
	return 0;
}

void edlib_init(struct pane *ed safe)
{
	rl_map = key_alloc();

	key_add(rl_map, "render-line", &render_line);
	key_add(rl_map, "render-line-prev", &render_prev);
	key_add(rl_map, "Clone", &rl_clone);
	key_add(rl_map, "Close", &rl_close);
	key_add(rl_map, "Notify:clip", &rl_clip);

	call_comm("global-set-command", ed, &renderline_attach, 0, NULL, "attach-renderline");
}
