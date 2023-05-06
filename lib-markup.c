/*
 * Copyright Neil Brown ©2016-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * This module provides render-line and render-line-prev, making use of
 * the chars returned by doc:char.
 * A line is normally text ending with a newline.  However if no newline
 * is found in a long distance, we drop a mark and use that as the start
 * of a line.
 */

#include <unistd.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>
#include <string.h>

#include <stdio.h>

#include "core.h"
#include "misc.h"

struct mu_info {
	int	view;
};

static struct map *mu_map safe;

#define LARGE_LINE 5000

static int is_render_eol(wint_t ch, struct pane *p safe, struct mark *m safe)
{
	char *attr;
	if (!is_eol(ch))
		return 0;
	attr = pane_mark_attr(p, m, "markup:not_eol");
	if (attr && *attr)
		return 0;
	return 1;
}

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
	struct mu_info *mu = ci->home->data;
	struct mark *boundary = NULL;
	struct mark *doc_boundary = NULL;
	int count = 0;
	int rpt = RPT_NUM(ci);
	wint_t ch;

	if (!m)
		return Enoarg;

	if (!rpt) {
		boundary = vmark_at_or_before(f, m, mu->view, ci->home);
		doc_boundary = call_ret(mark, "doc:get-boundary", f, -1, m);
	}
	while ((ch = doc_prev(f, m)) != WEOF &&
	       (!is_render_eol(ch, f, m) || rpt > 0) &&
	       count < LARGE_LINE &&
	       (!boundary || mark_ordered_not_same(boundary, m)) &&
	       (!doc_boundary || mark_ordered_not_same(doc_boundary, m))) {
		if (rpt) {
			boundary = vmark_at_or_before(f, m, mu->view, ci->home);
			doc_boundary = call_ret(mark, "doc:get-boundary", f, -1, m);
		}
		rpt = 0;
		count += 1;
	}
	if (ch != WEOF && !is_render_eol(ch, f, m) &&
	    (!doc_boundary || !mark_same(doc_boundary, m))) {
		/* Just cross the boundary, or the max count.
		 * Need to step back, and ensure there is a stable boundary
		 * here.
		 */
		mark_free(doc_boundary);
		doc_next(f, m);
		if (!boundary || !mark_same(boundary, m)) {
			boundary = vmark_new(f, mu->view, ci->home);
			if (boundary)
				mark_to_mark(boundary, m);
		}
		return 1;
	}
	mark_free(doc_boundary);
	if (ch == WEOF && rpt)
		return Efail;
	/* Found a '\n', so step forward over it for start-of-line. */
	if (is_render_eol(ch, f, m))
		doc_next(f, m);
	return 1;
}

/* 'ast' is a stack is all the attributes that should be applied "here".
 * They are sorted by priority with the highest first.
 * 'end' is an offset in chars-since-start-of-line where the attribute
 * should stop applying.  The current chars-since-start-of-line is 'chars'.
 * The stack structure reflects the nesting of <attr> and </>.
 * To change an attribute (normally add or delete) we pop it and any attributes
 * above it in the stack and push them onto tmpst, which is then in
 * reverse priority order.  As we do that, we count them in 'popped'.
 * Changes can be made in the secondary stack.
 * When all change have been made, we add 'popped' "</>" marked to the output,
 * then process everything in 'tmpst', either discarding it if end<=chars, or
 * outputting the attributes and pushing back on 'ast'.
 */
struct attr_return {
	struct command rtn;
	struct command fwd;
	struct attr_stack {
		struct attr_stack	*next;
		char			*attr safe;
		int			end;
		unsigned short		priority;
	} *ast, *tmpst;
	int min_end;
	int chars;
	short popped;
};

/* Find which attibutes should be finished by 'pos'.  The depth of
 * to deepest such is returned, and the next time to endpoint is
 * record in *nextp.
 * Everything higher than that returned depth will need to be closed,
 * so that the deepest one can be closed.
 * Then some of the higher ones might get re-opened.
 */
static int find_finished(struct attr_stack *st, int pos, int *nextp safe)
{
	int depth = 1;
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

/* Move the top 'depth' attributes from 'ast' to 'tmpst', updating 'popped' */
static void as_pop(struct attr_return *ar safe, int depth)
{
	struct attr_stack *from = ar->ast;
	struct attr_stack *to = ar->tmpst;

	while (from && depth > 0) {
		struct attr_stack *t;
		ar->popped += 1;
		t = from;
		from = t->next;
		t->next = to;
		to = t;
		depth -= 1;
	}
	ar->ast = from;
	ar->tmpst = to;
}

/* re-push any attriubtes that are still valid, freeing those that aren't */
static void as_repush(struct attr_return *ar safe, struct buf *b safe)
{
	struct attr_stack *from = ar->tmpst;
	struct attr_stack *to = ar->ast;

	while (ar->popped > 0) {
		buf_concat(b, "</>");
		ar->popped -= 1;
	}

	while (from) {
		struct attr_stack *t = from->next;
		if (from->end <= ar->chars) {
			free(from->attr);
			free(from);
		} else {
			buf_append(b, '<');
			buf_concat(b, from->attr);
			buf_append(b, '>');
			from->next = to;
			to = from;
			if (from->end < ar->min_end)
				ar->min_end = from->end;
		}
		from = t;
	}
	ar->tmpst = from;
	ar->ast = to;
}

static void as_add(struct attr_return *ar safe,
		   int end, int prio, const char *attr safe)
{
	struct attr_stack *new, **here;

	while (ar->ast && ar->ast->priority > prio)
		as_pop(ar, 1);

	here = &ar->tmpst;
	while (*here && (*here)->priority <= prio)
		here = &(*here)->next;
	new = calloc(1, sizeof(*new));
	new->next = *here;
	new->attr = strdup(attr);
	if (end == 0 || INT_MAX - end <= ar->chars)
		end = INT_MAX - 1 - ar->chars;
	new->end = ar->chars + end;
	if (prio < 0)
		prio = 0;
	if (prio > 65535)
		prio = 65535;
	new->priority = prio;
	*here = new;
}

static void as_clear(struct attr_return *ar safe,
		     int prio, const char *attr)
{
	struct attr_stack *st;

	while (ar->ast && ar->ast->priority >= prio)
		as_pop(ar, 1);

	for (st = ar->tmpst; st && st->priority <= prio; st = st->next)
		if (st->priority == prio &&
		    (attr == NULL || strcmp(st->attr, attr) == 0))
			st->end = ar->chars;
}

DEF_CB(text_attr_forward)
{
	struct attr_return *ar = container_of(ci->comm, struct attr_return, fwd);
	if (!ci->str || !ci->str2)
		return Enoarg;
	return call_comm("map-attr", ci->focus, &ar->rtn, 0, ci->mark, ci->str2,
			 0, NULL, ci->str);
}

DEF_CB(text_attr_callback)
{
	struct attr_return *ar = container_of(ci->comm, struct attr_return,
					      rtn);
	if (ci->num >= 0) {
		if (!ci->str)
			return Enoarg;
		as_add(ar, ci->num, ci->num2, ci->str);
	} else
		as_clear(ar, ci->num2, ci->str);
	// FIXME ->str2 should be inserted
	return 1;
}

static void call_map_mark(struct pane *f safe, struct mark *m safe,
			  struct attr_return *ar safe)
{
	const char *key = "render:";
	const char *val;

	while ((key = attr_get_next_key(m->attrs, key, -1, &val)) != NULL &&
	       strncmp(key, "render:", 7) == 0)
		call_comm("map-attr", f, &ar->rtn, 0, m, key, 0, NULL, val);
}

static int want_vis_newline(struct attr_stack *as)
{
	while (as && strstr(as->attr, "vis-nl") == NULL)
		as = as->next;
	return as != NULL;
}

DEF_CMD(render_line)
{
	/* Render the line from 'mark' to the first '\n' or until
	 * 'num' chars.
	 * Convert '<' to '<<' and if a char has the 'highlight' attribute,
	 * include that between '<>'.
	 */
	struct buf b;
	struct pane *focus = ci->focus;
	struct mu_info *mu = ci->home->data;
	struct mark *m = ci->mark;
	struct mark *pm = ci->mark2; /* The location to render as cursor */
	struct mark *boundary, *start_boundary = NULL;
	struct mark *doc_boundary;
	int o = ci->num;
	wint_t ch;
	int chars = 0;
	int ret;
	struct attr_return ar;
	int add_newline = 0;
	char *oneline;
	char *noret;
	char *attr;

	if (o == NO_NUMERIC)
		o = -1;

	ar.rtn = text_attr_callback;
	ar.fwd = text_attr_forward;
	ar.ast = ar.tmpst = NULL;
	ar.min_end = -1;
	ar.chars = 0;
	ar.popped = 0;

	if (!m)
		return Enoarg;

	oneline = pane_attr_get(focus, "render-one-line");
	if (oneline && strcmp(oneline, "yes") != 0)
		oneline = NULL;
	noret = pane_attr_get(focus, "render-hide-CR");
	if (noret && strcmp(noret, "yes") != 0)
		noret = NULL;

	ch = doc_following(focus, m);
	if (ch == WEOF)
		return Efail;
	if ((attr = pane_mark_attr(focus, m, "markup:func")) != NULL) {
		/* An alternate function handles this line */
		ret = call_comm(attr, focus, ci->comm2, o, m, NULL, 0, pm);
		if (ret)
			return ret;
	}
	boundary = vmark_at_or_before(focus, m, mu->view, ci->home);
	if (boundary) {
		if (mark_same(m, boundary))
			start_boundary = boundary;
		boundary = vmark_next(boundary);
	}
	doc_boundary = call_ret(mark, "doc:get-boundary", focus, 1, m);

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
			as_pop(&ar, depth);
		}

		ar.chars = chars;
		call_comm("doc:get-attr", focus, &ar.fwd, 0, m, "render:", 1);

		/* find all marks "here". They might get moved when we call map_mark,
		 * so move 'm' among them
		 */
		mark_step(m, 0);
		while ((m2 = mark_next(m)) != NULL &&
		       mark_same(m, m2)) {
			mark_to_mark_noref(m, m2);
			call_map_mark(focus, m2, &ar);
		}

		as_repush(&ar, &b);

		if (o >= 0 && b.len >= o)
			break;

		ch = doc_next(focus, m);
		if (ch == WEOF)
			break;
		if (!oneline && is_eol(ch)) {
			add_newline = 1;
			break;
		}
		if (boundary && mark_ordered_or_same(boundary, m))
			break;
		if (doc_boundary && mark_ordered_or_same(doc_boundary, m))
			break;
		if (ch == '<') {
			if (o >= 0 && b.len+1 >= o) {
				doc_prev(focus, m);
				break;
			}
			buf_append(&b, '<');
		}
		if (ch == '\r' && noret) {
			/* do nothing */
		} else if (ch < ' ' && ch != '\t' && (oneline || !is_eol(ch))) {
			buf_concat(&b, "<fg:red>^");
			buf_append(&b, '@' + ch);
			buf_concat(&b, "</>");
		} else if (ch == 0x7f) {
			buf_concat(&b, "<fg:red>^?</>");
		} else if (ch >= 0x80 && iswcntrl(ch)) {
			/* Extra unicode control */
			buf_concat(&b, "<fg:magenta>^");
			buf_append(&b, 96 + (ch & 0x1f));
			buf_concat(&b, "</>");
		} else
			buf_append(&b, ch);
		chars++;
	}
	if (add_newline && want_vis_newline(ar.ast))
		buf_concat(&b, "↩");
	while (ar.ast)
		as_pop(&ar, 100);
	ar.chars = INT_MAX;
	as_repush(&ar, &b);
	if (add_newline) {
		if (o >= 0 && b.len >= o)
			/* skip the newline */
			doc_prev(focus, m);
		else
			buf_append(&b, '\n');
	}

	if (start_boundary && chars < LARGE_LINE - 5)
		/* This boundary is no longer well-placed. */
		mark_free(start_boundary);

	mark_free(doc_boundary);

	ret = comm_call(ci->comm2, "callback:render", focus, 0, NULL,
			buf_final(&b));
	free(b.b);
	return ret ?: 1;
}

DEF_LOOKUP_CMD(markup_handle, mu_map);

static struct pane *do_markup_attach(struct pane *p safe)
{
	struct pane *ret;
	struct mu_info *mu;

	alloc(mu, pane);
	ret = pane_register(p, 0, &markup_handle.c, mu);
	if (!ret)
		return NULL;
	mu->view = home_call(p, "doc:add-view", ret) - 1;

	return ret;
}

DEF_CMD(markup_attach)
{
	struct pane *ret;

	ret = do_markup_attach(ci->focus);
	if (!ret)
		return Efail;
	return comm_call(ci->comm2, "callback:attach", ret);
}

DEF_CMD(mu_clone)
{
	struct pane *parent = ci->focus;
	struct pane *child = do_markup_attach(parent);
	pane_clone_children(ci->home, child);
	return 1;
}

DEF_CMD(mu_clip)
{
	struct mu_info *mu = ci->home->data;

	marks_clip(ci->home, ci->mark, ci->mark2,
		   mu->view, ci->home, !!ci->num);
	return Efallthrough;
}

void edlib_init(struct pane *ed safe)
{
	mu_map = key_alloc();

	key_add(mu_map, "doc:render-line", &render_line);
	key_add(mu_map, "doc:render-line-prev", &render_prev);
	key_add(mu_map, "Clone", &mu_clone);
	key_add(mu_map, "Free", &edlib_do_free);
	key_add(mu_map, "Notify:clip", &mu_clip);

	call_comm("global-set-command", ed, &markup_attach,
		  0, NULL, "attach-markup");
}
