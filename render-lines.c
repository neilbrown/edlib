/*
 * Copyright Neil Brown ©2015-2019 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Rendering for any document which presents as a sequence of lines.
 *
 * The underlying document, or an intervening filter, must return lines of
 * text in response to the "doc:render-line" command.
 * This takes a mark and moves it to the end of the rendered line
 * so that another call will produce another line.
 * "doc:render-line" must always return a full line including '\n'
 * unless the result would be bigger than the 'max' passed in ->num or,
 * when ->num==-1, unless the rendering would go beyond the location in
 * ->mark2.  In these cases it can stop before a '\n'.  In each case,
 * the mark is moved to the end of the region that was rendered;
 * This allows a mark to be found for a given character position, or a display
 * position found for a given mark.
 * For the standard 'render the whole line' functionality, ->num should
 * be NO_NUMERIC
 *
 * The document or filter must also provide "doc:render-line-prev" which
 * moves mark to a start-of-line.  If num is 0, then don't skip over any
 * newlines.  If it is '1', then skip one newline.
 *
 * The returned line can contain attribute markings as <attr,attr>.  </>
 * is used to pop most recent attributes.  << is used to include a
 * literal '<'.  Lines generally contain UTF-8.  Control character '\n'
 * is end of line and '\t' tabs 1-8 spaces.  '\f' marks end of page -
 * nothing after this will be displayed.
 *
 * Other control characters should be rendered as
 * e.g. <fg:red>^X</> - in particular, nul must not appear in the line.
 *
 * We store all start-of-line the marks found while rendering a pane in
 * a 'view' on the document.  The line returned for a given mark is
 * attached to extra space allocated for that mark.  When a change
 * notification is received for a mark we discard that string.  So the
 * string associated with a mark is certainly the string that would be
 * rendered after that mark (though it may be truncated).  The set of
 * marks in a view should always identify exactly the set of lines to be
 * displayed.  Each mark should be at a start-of-line except possibly
 * for the first and last.  The first may be internal to a long line,
 * but the line rendering attached will always continue to the
 * end-of-line.  We record the number of display lines in that first
 * line.
 * The last mark may also be mid-line, and it must never have an
 * attached rendering.
 * In the worst case of there being no newlines in the document, there
 * will be precisely two marks: one contains a partial line and one that
 * marks the end of that line.  When point moves outside that range a
 * new start will be chosen before point using "doc:render-line-prev"
 * and the old start is discarded.
 *
 * To render the pane we:
 * 1/ call 'render-line-prev' on a mark at the point and look for that mark
 *    in the view.
 * 2/ If the mark matches and has a string, we have a starting point,
 *    else we call "doc:render-line" and store the result, thus
 *    producing a starting point.  We determine how many display lines
 *    are needed to display this text-line and set 'y' accordingly.
 *    At this point we have two marks: start and end, with known text of known
 *    height between.
 * 3/ Then we move outwards, back from the first mark and forward from
 *    the last mark.  If we find a mark already in the view in the
 *    desired direction with text attached it is correct and we use
 *    that.  Otherwise we find start (when going backwards) and render a
 *    new line.  Any old mark that is in the range is discarded.
 * 4/ When we have a full set of marks and the full height of the pane,
 *    we discard marks outside the range and start rendering from the
 *    top.  ARG how is cursor drawn.
 *
 * If we already have correct marks on one side and not the other, we prefer
 * to advance on that first side to maximize the amount of text that was common
 * with the previous rendering of the page.
 *
 * Sometimes we need to render without a point.  In this case we start
 * at the first mark in the view and move forward.  If we can we do this
 * anyway, and only try the slow way if the target point wasn't found.
 */

#define	MARK_DATA_PTR char
#include "core.h"
#include "misc.h"
#include <stdio.h> /* snprintf */

/*
 * All functions involved in sending Draw and size requests
 * to the display are given two panes: p and focus.
 * 'p' is the pane where the drawing happens. 'focus' is the
 * leaf on the current stack.
 * These are different when the drawing is segmented into regions
 * of the target pane, with light-weight panes being used to avoid
 * having to refresh the whole target pane when the only change is
 * in one region.
 * The calls to the display are home_calls with 'focus' as the home
 * pane, and 'p' as the focus.  The x,y co-ords are, as always,
 * relative to the focus pane 'p'.
 */


struct rl_data {
	int		top_sol; /* true when first mark is at a start-of-line */
	int		ignore_point;
	struct mark	*old_point; /* where was 'point' last time we rendered */
	int		skip_lines; /* Skip display-lines for first "line" */
	int		cursor_line; /* line that contains the cursor starts
				      * on this line */
	short		target_x, target_y;
	short		i_moved;	/* I moved cursor, so don't clear
					 * target
					 */
	int		do_wrap;
	short		shift_left;
	short		prefix_len;
	short		header_lines;
	int		typenum;
	short		line_height;
	int		repositioned; /* send "render:reposition" when we know
				       * full position again.
				       */
	short		lines; /* lines drawn before we hit eof */
	short		cols; /* columns used for longest line */

	struct pane	*helper; /* pane where renderlines happens. */
	struct command	c;
	/* following set by render_line() callback */
	short		y;
	short		end_of_page;
	short		xypos;	/* Where in line the x,y pos was found */
	const char	*xyattrs;
	short		cx,cy;
	short		cwidth;
};

DEF_CMD(rl_cb)
{
	struct rl_data *rl = container_of(ci->comm, struct rl_data, c);

	if (strcmp(ci->key, "render-done") == 0) {
		rl->y = ci->num;
		if (ci->str)
			rl->end_of_page = 1;
		rl->cx = ci->x;
		rl->cy = ci->y;
		rl->cwidth = ci->num2;
		return 1;
	}
	if (strcmp(ci->key, "dimensions") == 0) {
		if (ci->num > rl->cols)
			rl->cols = ci->num;
		rl->line_height = ci->num2;
		return 1;
	}
	if (strcmp(ci->key, "prefix_len") == 0) {
		rl->prefix_len = ci->num;
		return 1;
	}
	if (strcmp(ci->key, "xypos") == 0) {
		rl->xypos = ci->num;
		rl->xyattrs = ci->str;
		return 1;
	}
	return 0;
}

static void render_line(struct pane *p safe, struct pane *focus safe,
			char *line safe, short y_start, int dodraw,
			short posx, short posy, short offset,
			struct command *comm2)
{
	struct rl_data *rl = p->data;

	home_call(rl->helper,
		  dodraw ? "render-line:draw" : "render-line:measure",
		  focus,
		  y_start, NULL, line,
		  offset, NULL, NULL,
		  posx, posy, comm2);
}

static struct mark *call_render_line_prev(struct pane *p safe,
					  struct mark *m safe,
					  int n, int *found)
{
	int ret;
	struct mark *m2;

	if (m->viewnum < 0)
		return NULL;
	ret = call("doc:render-line-prev", p, n, m);
	if (ret <= 0) {
		mark_free(m);
		return NULL;
	}
	/* if n>0 we can fail because start-of-file was found before
	 * any newline.  In that case ret == -2, and we return NULL.
	 */
	if (found)
		*found = (ret != -1);
	if (ret < 0) {
		/* current line is start-of-file */
		mark_free(m);
		return NULL;
	}

	m2 = vmark_matching(m);
	if (m2)
		mark_free(m);
	else
		m2 = m;
	return m2;
}

static struct mark *call_render_line(struct pane *p safe,
				     struct mark *start safe)
{
	struct mark *m, *m2;
	char *s;

	m = mark_dup_view(start);

	/* Allow for filling the rest of the pane, given that
	 * some has been used.
	 * 'used' can be negative if the mark is before the start
	 * of the pane
	 */
	s = call_ret(str, "doc:render-line", p, NO_NUMERIC, m);

	if (s) {
		free(start->mdata);
		start->mdata = s;
	}

	m2 = vmark_matching(m);
	if (m2)
		mark_free(m);
	else
		m2 = m;
	/*FIXME shouldn't be needed */
	m2 = safe_cast m2;

	/* Any mark between start and m2 must be discarded,
	 */
	while ((m = vmark_next(start)) != NULL &&
	       m->seq < m2->seq) {
			free(m->mdata);
			m->mdata = NULL;
			mark_free(m);
	}

	return m2;
}

DEF_CMD(no_save)
{
	return 1;
}

static struct mark *call_render_line_offset(struct pane *p safe,
					    struct mark *start safe, int offset)
{
	struct mark *m;

	m = mark_dup_view(start);
	if (call_comm("doc:render-line", p, &no_save, offset, m) <= 0) {
		mark_free(m);
		return NULL;
	}
	return m;
}

DEF_CMD(get_len)
{
	if (ci->str) {
		int l = strlen(ci->str);
		while (l >=3 && strncmp(ci->str+l-3, "</>", 3) == 0)
			l -= 3;
		return l + 1;
	} else
		return 1;
}

static int call_render_line_to_point(struct pane *p safe, struct mark *pm safe,
				     struct mark *start safe)
{
	int len;
	struct mark *m = mark_dup_view(start);

	len = call_comm("doc:render-line", p, &get_len, -1, m, NULL, 0, pm);
	mark_free(m);
	if (len <= 0)
		return 0;

	return len - 1;
}

/* Choose a new set of lines to display, and mark each one with a line marker.
 * We start at pm and move both backwards and forwards one line at a time.
 * We stop moving in one of the directions when
 *  - we hit start/end of file
 *  - when the edge in the *other* direction enters the previously visible
 *     area (if there was one).  This increases stability of display when
 *     we move off a line or 2.
 *  - when we reach the given line count (vline).  A positive count restricts
 *    backward movement, a negative restricts forwards movement.
 */
static void find_lines(struct mark *pm safe, struct pane *p safe,
		       struct pane *focus safe,
		       int vline)
{
	struct rl_data *rl = p->data;
	struct mark *top, *bot;
	struct mark *m;
	struct mark *start, *end;
	short y = 0;
	short y_above = 0, y_below = 0;
	short offset;
	int found_start = 0, found_end = 0;
	short lines_above = 0, lines_below = 0;

	top = vmark_first(focus, rl->typenum, p);
	bot = vmark_last(focus, rl->typenum, p);
	if (!top && vline == 0 && rl->line_height)
		vline = (p->h - rl->header_lines) / rl->line_height / 2;
	/* Don't consider the top or bottom lines as currently being
	 * displayed - they might not be.
	 */
	if (top)
		top = vmark_next(top);
	if (bot)
		bot = vmark_prev(bot);
	/* Protect top/bot from being freed by call_render_line */
	if (top)
		top = mark_dup(top);
	if (bot)
		bot = mark_dup(bot);
	m = vmark_new(focus, rl->typenum, p);
	if (!m)
		goto abort;
	mark_to_mark(m, pm);
	m = call_render_line_prev(focus, m, 0, &rl->top_sol);
	if (!m)
		goto abort;
	start = m;
	offset = call_render_line_to_point(focus, pm, start);
	if (start->mdata == NULL)
		m = call_render_line(focus, start);
	else
		m = vmark_next(start);

	end = m;
	if (!end)
		goto abort; /* FIXME can I prove this? */

	rl->y = y = 0;
	render_line(p, focus, start->mdata ?: "", y, 0,
		    -1, -1, offset, &rl->c);
	lines_above = rl->cy;
	y = rl->y;
	lines_above = lines_below = 0;

	/* We have start/end of the focus line, and its height.
	 * Rendering just that "line" uses a height of 'y', of which
	 * 'lines_above' is above the cursor, and 'lines_below' is below.
	 */
	if (bot && !mark_ordered_or_same(bot, start))
		/* already before 'bot', so will never "cross over" bot, so
		 * ignore 'bot'
		 */
		bot = NULL;
	if (top && !mark_ordered_or_same(end, top))
		top = NULL;

	while ((!found_start || !found_end) && y < p->h - rl->header_lines) {
		if (vline != NO_NUMERIC) {
			if (!found_start && vline > 0 &&
			    y_above >= (vline-1) * rl->line_height)
				found_start = 1;
			if (!found_end && vline < 0 &&
			    y_below >= (-vline-1) * rl->line_height)
				found_end = 1;
		}
		if (!found_start && lines_above == 0) {
			/* step backwards moving start */
			m = call_render_line_prev(focus, mark_dup_view(start),
						  1, &rl->top_sol);
			if (!m) {
				/* no text before 'start' */
				found_start = 1;
			} else {
				short h = 0;
				start = m;
				if (!start->mdata)
					call_render_line(focus, start);
				if (start->mdata) {
					render_line(p, focus, start->mdata, h, 0,
						    -1, -1, -1, &rl->c);
					h = rl->y;
				}
				if (h)
					lines_above = h;
				else
					found_start = 1;
			}
			if (bot && start->seq < bot->seq)
				found_end = 1;
		}
		if (!found_end && lines_below == 0) {
			/* step forwards */
			struct mark *next;
			if (!end->mdata)
				call_render_line(focus, end);
			next = vmark_next(end);
			if (!end->mdata || !next) {
				found_end = 1;
				lines_below = rl->line_height * 2;
			} else {
				short h;
				rl->end_of_page = 0;
				render_line(p, focus, end->mdata, 0, 0,
					    -1, -1, -1, &rl->c);
				h = rl->y;
				found_end = rl->end_of_page;
				end = next;
				if (h)
					lines_below = h;
				else {
					found_end = 1;
					lines_below = rl->line_height * 2;
				}
			}
			if (top && top->seq < end->seq)
				found_start = 1;
		}
		if (lines_above > 0 && lines_below > 0) {
			int consume = (lines_above > lines_below
				       ? lines_below : lines_above) * 2;
			int above, below;
			if (consume > (p->h - rl->header_lines) - y)
				consume = (p->h - rl->header_lines) - y;
			if (lines_above > lines_below) {
				above = consume - (consume/2);
				below = consume/2;
			} else {
				below = consume - (consume/2);
				above = consume/2;
			}
			y += above + below;
			lines_above -= above;
			y_above += above;
			lines_below -= below;
			y_below += below;
			/* We have just consumed all of one of
			 * lines_{above,below} so they are no longer
			 * both > 0 */
		}
		if (found_end && lines_above) {
			int consume = p->h - rl->header_lines - y;
			if (consume > lines_above)
				consume = lines_above;
			lines_above -= consume;
			y += consume;
			y_above += consume;
		}
		if (found_start && lines_below) {
			int consume = p->h - rl->header_lines - y;
			if (consume > lines_below)
				consume = lines_below;
			lines_below -= consume;
			y += consume;
			y_below += consume;
		}
	}
	rl->skip_lines = lines_above;
	/* Now discard any marks outside start-end */
	if (end->seq < start->seq)
		/* something confused, make sure we don't try to use 'end' after
		 * freeing it.
		 */
		end = start;
	while ((m = vmark_prev(start)) != NULL) {
		free(m->mdata);
		m->mdata = NULL;
		mark_free(m);
	}
	while ((m = vmark_next(end)) != NULL) {
		free(m->mdata);
		m->mdata = NULL;
		mark_free(m);
	}
	free(end->mdata);
	end->mdata = NULL;

abort:
	mark_free(top);
	mark_free(bot);
}

static int render(struct mark *pm, struct pane *p safe,
		  struct pane *focus safe)
{
	struct rl_data *rl = p->data;
	short y;
	struct mark *m, *m2;
	int shifted = 0;
	char *hdr;
	struct xy scale = pane_scale(focus);
	char *s;
	int hide_cursor = 0;
	int cursor_drawn = 0;
	short mwidth = -1;

	hdr = pane_attr_get(focus, "heading");
	if (hdr && !*hdr)
		hdr = NULL;
	s = pane_attr_get(focus, "hide-cursor");
	if (s && strcmp(s, "yes") == 0)
		hide_cursor = 1;
	s = pane_attr_get(focus, "background");

restart:
	rl->end_of_page = 0;
	rl->cols = 0;
	m = vmark_first(focus, rl->typenum, p);
	if (!s)
		home_call(focus, "pane-clear", p);
	else if (strncmp(s, "color:", 6) == 0) {
		char *a = strdup(s);
		strcpy(a, "bg:");
		strcpy(a+3, s+6);
		home_call(focus, "pane-clear", p, 0, NULL, a);
		free(a);
	} else if (strncmp(s, "image:", 6) == 0) {
		home_call(focus, "pane-clear", p);
		home_call(focus, "Draw:image", p, 1, NULL, s+6);
	} else if (strncmp(s, "call:", 5) == 0) {
		home_call(focus, "pane-clear", p);
		home_call(focus, s+5, p, 0, m);
	} else
		home_call(focus, "pane-clear", p);

	y = 0;
	if (hdr) {
		rl->header_lines = 0;
		render_line(p, focus, hdr, 0, 1,
			    -1, -1, -1, &rl->c);
		y = rl->y;
		rl->header_lines = y;
	}
	y -= rl->skip_lines;

	p->cx = p->cy = -1;
	rl->cursor_line = 0;

	while (m && y < p->h && !rl->end_of_page) {
		if (m->mdata == NULL) {
			/* This line has changed. */
			call_render_line(focus, m);
		}
		m2 = vmark_next(m);
		if (!hide_cursor && p->cx <= 0 && pm &&
		    mark_ordered_or_same(m, pm) &&
		    (!(m2 && doc_following_pane(focus, m2) != WEOF) ||
		     mark_ordered_not_same(pm, m2))) {
			short len = call_render_line_to_point(focus, pm,
							      m);
			rl->cursor_line = y;
			rl->prefix_len = 0;
			rl->xypos = -1;
			rl->cwidth = 1;
			render_line(p, focus, m->mdata ?: "", y, 1,
				    -1, -1, len,
				    &rl->c);
			p->cx = rl->cx; p->cy = rl->cy;
			y = rl->y;
			if (p->cy < 0)
				p->cx = -1;
			if (!rl->do_wrap && p->cy >= 0 &&
			    p->cx < rl->prefix_len &&
			    shifted != 2) {
				if (mwidth < 0) {
					struct call_return cr =
						home_call_ret(all, focus,
							      "text-size", p,
							      -1, NULL, "M",
							      0,  NULL, "");
					mwidth = cr.x;
					if (mwidth <= 0)
						mwidth = 1;
				}
				if (p->cx + rl->cwidth + 8 * mwidth < p->w) {
					/* Need to shift to right, and there
					 * is room */
					while (rl->shift_left > 0 &&
					       p->cx < rl->prefix_len) {
						if (rl->shift_left < 8* mwidth) {
							p->cx += rl->shift_left;
							rl->shift_left = 0;
						} else {
							p->cx += 8 * mwidth;
							rl->shift_left -=
								8 * mwidth;
						}
					}
					shifted = 1;
					goto restart;
				}
			}
			if (p->cx + rl->cwidth >= p->w && !rl->do_wrap &&
			    shifted != 1) {
				if (mwidth < 0) {
					struct call_return cr =
						home_call_ret(all, focus,
							      "text-size", p,
							      -1, NULL, "M",
							      0, NULL, "");
					mwidth = cr.x;
					if (mwidth <= 0)
						mwidth = 1;
				}
				/* Need to shift to the left */
				while (p->cx + rl->cwidth >= p->w) {
					rl->shift_left += 8 * mwidth;
					p->cx -= 8 * mwidth;
				}
				shifted = 2;
				goto restart;
			}
			cursor_drawn = 1;
		} else {
			render_line(p, focus, m->mdata?:"", y, 1,
				    -1, -1, -1, &rl->c);
			y = rl->y;
		}
		if (!m2 || mark_same(m, m2))
			break;
		m = m2;
	}
	if (!cursor_drawn && !hide_cursor) {
		/* Place cursor in bottom right */
		if (mwidth < 0) {
			struct call_return cr =
				home_call_ret(all, focus, "text-size", p,
					      -1, NULL, "M",
					      0,  NULL, "");
			mwidth = cr.x;
			if (mwidth <= 0)
				mwidth = 1;
		}
		home_call(focus, "Draw:text", p, 0, NULL, " ",
			  scale.x, NULL, "",
			  focus->w - mwidth, focus->h-1);
	}
	/* Any marks after 'm' must be discarded */
	if (m) {
		free(m->mdata);
		m->mdata = NULL;
		while ((m2 = vmark_next(m)) != NULL) {
			free(m2->mdata);
			m2->mdata = NULL;
			mark_free(m2);
		}
	}
	return y;
}

DEF_CMD(render_lines_get_attr)
{
	struct rl_data *rl = ci->home->data;

	if (strcmp(ci->str, "shift_left") == 0) {
		char ret[10];
		if (rl->do_wrap)
			return comm_call(ci->comm2, "cb", ci->focus,
					 0, NULL, "-1");
		snprintf(ret, sizeof(ret), "%d", rl->shift_left);
		return comm_call(ci->comm2, "cb", ci->focus, 0, NULL, ret);
	}
	return 0;
}


DEF_CMD(render_lines_refresh)
{
	struct pane *p = ci->home;
	struct pane *focus = ci->focus;
	struct rl_data *rl = p->data;
	struct mark *m, *pm;
	char *a;

	a = pane_attr_get(focus, "render-wrap");
	rl->do_wrap = (!a || strcmp(a, "yes") == 0);

	pm = call_ret(mark, "doc:point", focus);

	if (pm) {
		if (rl->old_point && !mark_same(pm, rl->old_point)) {
			call("view:changed", focus, 0, rl->old_point,
			     NULL, 0, pm);
			mark_free(rl->old_point);
			rl->old_point = NULL;
			rl->ignore_point = 0;
			if (!rl->i_moved)
				rl->target_x = -1;
			else
				rl->i_moved = 0;
		}
		if (!rl->old_point)
			rl->old_point = mark_dup(pm);
	}

	m = vmark_first(focus, rl->typenum, p);
	if (rl->top_sol && m)
		m = call_render_line_prev(focus, mark_dup_view(m), 0,
					  &rl->top_sol);

	if (m) {
		rl->lines = render(pm, p, focus);
		if (!pm || rl->ignore_point || (p->cx >= 0 && p->cy < p->h)) {
			call("render:reposition", focus,
			     rl->lines, vmark_first(focus, rl->typenum, p), NULL,
			     rl->cols, vmark_last(focus, rl->typenum, p), NULL,
			     p->cx, p->cy);

			return 0;
		}
	}
	m = pm;
	if (!m)
		m = vmark_new(focus, MARK_UNGROUPED, NULL);
	if (!m)
		return Efail;
	find_lines(m, p, focus, NO_NUMERIC);
	rl->lines = render(m, p, focus);
	rl->repositioned = 0;
	call("render:reposition", focus,
	     rl->lines, vmark_first(focus, rl->typenum, p), NULL,
	     rl->cols, vmark_last(focus, rl->typenum, p), NULL,
	     p->cx, p->cy);
	if (!pm)
		mark_free(m);
	return 0;
}

DEF_CMD(render_lines_refresh_view)
{
	struct pane *p = ci->home;
	struct pane *focus = ci->focus;
	struct rl_data *rl = p->data;
	struct mark *m;

	for (m = vmark_first(p, rl->typenum, p);
	     m;
	     m = vmark_next(m)) {
		free(m->mdata);
		m->mdata = NULL;
	}

	if (rl->repositioned) {
		struct mark *pm = call_ret(mark, "doc:point", focus);
		rl->lines = render(pm, p, focus);
	}
	rl->repositioned = 0;
	if (p->damaged & (DAMAGED_CONTENT|DAMAGED_SIZE))
		; /* wait for a proper redraw */
	else
		call("render:reposition", focus,
		     rl->lines, vmark_first(focus, rl->typenum, p), NULL,
		     rl->cols, vmark_last(focus, rl->typenum, p), NULL,
		     p->cx, p->cy);
	return 0;
}

DEF_CMD(render_lines_close)
{
	struct pane *p = ci->home;
	struct rl_data *rl = p->data;
	struct mark *m;

	while ((m = vmark_first(p, rl->typenum, p)) != NULL) {
		free(m->mdata);
		m->mdata = NULL;
		mark_free(m);
	}

	call("doc:del-view", p, rl->typenum);
	mark_free(rl->old_point);
	return 0;
}

DEF_CMD(render_lines_abort)
{
	struct pane *p = ci->home;
	struct rl_data *rl = p->data;

	rl->ignore_point = 0;
	rl->target_x = -1;

	pane_damaged(p, DAMAGED_CONTENT);
	pane_damaged(p, DAMAGED_CURSOR);

	/* Allow other handlers to complete the Abort */
	return 0;
}

DEF_CMD(render_lines_move)
{
	/*
	 * Find a new 'top' for the displayed region so that render()
	 * will draw from there.
	 * When moving backwards we move back a line and render it.
	 * When moving forwards we render and then step forward
	 * At each point we count the number of display lines that result.
	 * When we choose a new start, we delete all earlier marks.
	 * We all delete marks before current top when moving forward
	 * where there are more than a page full.
	 */
	struct pane *p = ci->home;
	struct pane *focus = ci->focus;
	int rpt = RPT_NUM(ci);
	struct rl_data *rl = p->data;
	struct mark *top, *old_top;
	int pagesize = rl->line_height;

	top = vmark_first(focus, rl->typenum, p);
	if (!top)
		return 0;

	old_top = mark_dup(top);
	if (strcmp(ci->key, "Move-View-Large") == 0)
		pagesize = p->h - 2 * rl->line_height;
	rpt *= pagesize;

	rl->ignore_point = 1;

	if (rpt < 0) {
		while (rpt < 0) {
			short y = 0;
			struct mark *m;
			struct mark *prevtop = top;

			if (rl->skip_lines) {
				rl->skip_lines -= 1;
				rpt += 1;
				continue;
			}

			m = mark_dup_view(top);
			top = call_render_line_prev(focus, m,
						    1, &rl->top_sol);
			if (!top && doc_prior_pane(focus, prevtop) != WEOF) {
				/* Double check - maybe a soft top-of-file */
				m = mark_dup(prevtop);
				mark_prev_pane(focus, m);
				top = call_render_line_prev(focus, m,
							    1, &rl->top_sol);
			}
			if (!top)
				break;
			m = top;
			while (m && m->seq < prevtop->seq &&
			       !mark_same(m, prevtop)) {
				if (m->mdata == NULL)
					call_render_line(focus, m);
				if (m->mdata == NULL) {
					rpt = 0;
					break;
				}
				render_line(p, focus, m->mdata, y, 0,
					    -1, -1, -1, &rl->c);
				y = rl->y;
				m = vmark_next(m);
			}
			rl->skip_lines = y;
		}
	} else {
		while (top && rpt > 0) {
			short y = 0;

			if (top->mdata == NULL)
				call_render_line(focus, top);
			if (top->mdata == NULL)
				break;
			rl->end_of_page = 0;
			render_line(p, focus, top->mdata, 0, 0,
				    -1, -1, -1, &rl->c);
			if (rl->end_of_page)
				y = rpt % pagesize;
			else
				y = rl->y;
			if (rl->skip_lines + rpt < y) {
				rl->skip_lines += rpt;
				break;
			}
			top = vmark_next(top);
			if (top && top->mdata == NULL)
				call_render_line(focus, top);
			rpt -= y - rl->skip_lines;
			rl->skip_lines = 0;
		}
		if (top && top->mdata) {
			/* We didn't fall off the end, so it is OK to remove
			 * everything before 'top'
			 */
			struct mark *old;
			while ((old = vmark_first(focus, rl->typenum, p)) != NULL &&
			       old != top) {
				free(old->mdata);
				old->mdata = NULL;
				mark_free(old);
			}
		}
	}
	rl->repositioned = 1;
	pane_damaged(ci->home, DAMAGED_VIEW);
	top = vmark_first(focus, rl->typenum, p);
	if (top && mark_same(top, old_top)) {
		mark_free(old_top);
		return 2;
	}
	mark_free(old_top);
	return 1;
}

static char *get_active_tag(const char *a)
{
	char *t;
	char *c;

	if (!a)
		return NULL;
	t = strstr(a, ",active-tag:");
	if (!t)
		return NULL;
	t += 12;
	c = strchr(t, ',');
	return strndup(t, c?c-t: (int)strlen(t));
}

DEF_CMD(render_lines_set_cursor)
{
	struct pane *p = ci->home;
	struct pane *focus = ci->focus;
	struct rl_data *rl = p->data;
	struct mark *m;
	struct mark *newpoint = NULL;
	short y = rl->header_lines - rl->skip_lines;
	int found = 0;
	short cihx = 0, cihy = 0;

	m = vmark_first(p, rl->typenum, p);

	if (ci->x >= 0)
		cihx = ci->x;
	else if (p->cx >= 0)
		cihx = p->cx;

	if (ci->y >= 0)
		cihy = ci->y;
	else if (p->cy >= 0)
		cihy = p->cy;

	pane_map_xy(ci->focus, ci->home, &cihx, &cihy);

	if (y > cihy)
		/* x,y is in header line - try lower */
		cihy = y;
	while (y <= cihy && m && m->mdata) {
		call_render_line(focus, m);
		rl->xypos = -1;
		rl->xyattrs = NULL;
		render_line(p, focus, m->mdata, y, 0, cihx, cihy,
			    -1, &rl->c);
		y = rl->y;
		if (rl->xypos >= 0) {
			struct mark *m2 = call_render_line_offset(focus, m,
								  rl->xypos);
			if (m2) {
				char *tag = get_active_tag(rl->xyattrs);
				if (tag)
					call("Mouse-Activate", focus, 0, m2, tag,
					     0, ci->mark, rl->xyattrs);
				free(tag);
				if (!newpoint)
					newpoint = mark_dup(m2);
				else
					mark_to_mark(newpoint, m2);
				mark_free(m2);
				found = 1;
			}
		} else if (found)
			break;
		m = vmark_next(m);
	}

	if (newpoint) {
		if (ci->mark)
			mark_to_mark(ci->mark, newpoint);
		else
			call("Move-to", focus, 0, newpoint);
		mark_free(newpoint);
	}
	return 1;
}

DEF_CMD(render_lines_move_pos)
{
	struct pane *p = ci->home;
	struct pane *focus = ci->focus;
	struct rl_data *rl = p->data;
	struct mark *pm = ci->mark;
	struct mark *top, *bot;

	if (!pm)
		return Enoarg;
	rl->ignore_point = 1;
	top = vmark_first(focus, rl->typenum, p);
	bot = vmark_last(focus, rl->typenum, p);
	if (top && rl->skip_lines)
		/* top line not fully displayed, being in that line is
		 * not sufficient */
		top = vmark_next(top);
	if (bot)
		/* last line might not be fully displayed, so don't assume */
		bot = vmark_prev(bot);
	if (top && bot &&
	    top->seq < pm->seq &&
	    pm->seq < bot->seq && !mark_same(pm, bot))
		/* pos already displayed */
		return 1;
	find_lines(pm, p, focus, NO_NUMERIC);
	pane_damaged(p, DAMAGED_VIEW);
	rl->repositioned = 1;
	return 1;
}

DEF_CMD(render_lines_view_line)
{
	struct pane *p = ci->home;
	struct pane *focus = ci->focus;
	struct rl_data *rl = p->data;
	struct mark *pm = ci->mark;
	struct mark *top;
	int line = ci->num;

	if (!pm)
		return Enoarg;
	if (line == NO_NUMERIC)
		return Einval;

	while ((top = vmark_first(focus, rl->typenum, p)) != NULL) {
		free(top->mdata);
		top->mdata = NULL;
		mark_free(top);
	}
	rl->ignore_point = 1;
	find_lines(pm, p, focus, line);
	pane_damaged(p, DAMAGED_VIEW);
	rl->repositioned = 1;
	return 1;
}

DEF_CMD(render_lines_move_line)
{
	/* FIXME should be able to select between display lines
	 * and content lines - different when a line wraps.
	 * For now just content lines.
	 * target_x and target_y are the target location in a line
	 * relative to the start of line.
	 * We use Move-EOL to find a suitable start of line, then
	 * render that line and find the last location not after x,y
	 */
	struct pane *p = ci->home;
	struct pane *focus = ci->focus;
	struct rl_data *rl = p->data;
	short target_x, target_y;
	int num;
	struct mark *m = ci->mark;

	if (!m)
		m = call_ret(mark, "doc:point", focus);
	if (!m)
		return Efail;

	/* save target as it might get changed */
	target_x = rl->target_x;
	target_y = rl->target_y;
	if (target_x < 0) {
		target_x = p->cx;
		target_y = p->cy - rl->cursor_line;
	}

	num = RPT_NUM(ci);
	if (num < 0)
		num -= 1;
	else
		num += 1;
	if (!call("Move-EOL", ci->focus, num, m))
		return Efail;
	if (RPT_NUM(ci) > 0) {
		/* at end of target line, move to start */
		if (!call("Move-EOL", ci->focus, -1, m))
			return Efail;
	}

	/* restore target: Move-EOL might have changed it */
	rl->target_x = target_x;
	rl->target_y = target_y;

	if (target_x >= 0 || target_y >= 0) {
		short y = 0;
		struct mark *start = vmark_new(focus, rl->typenum, p);

		if (start) {
			mark_to_mark(start, m);
			start = call_render_line_prev(focus, start, 0, NULL);
		}

		if (!start) {
			pane_damaged(p, DAMAGED_CONTENT);
			return 1;
		}
		/* FIXME only do this if point is active/volatile, or
		 * if start->mdata is NULL
		 */
		call_render_line(focus, start);
		rl->xypos = -1;
		render_line(p, focus, start->mdata?:"", y, 0,
			    target_x, target_y, -1, &rl->c);
		y = rl->y;
		/* rl->xypos is the distance from start-of-line to the target */
		if (rl->xypos >= 0) {
			struct mark *m2 = call_render_line_offset(
				focus, start, rl->xypos);
			if (m2)
				mark_to_mark(m, m2);
			mark_free(m2);
		}
	}
	rl->i_moved = 1;
	return 1;
}

DEF_CMD(render_lines_notify_replace)
{
	struct pane *p = ci->home;
	struct rl_data *rl = p->data;
	struct mark *start = ci->mark;
	struct mark *end = ci->mark2;

	if (!start && !end) {
		/* No marks given - assume everything changed */
		pane_damaged(p, DAMAGED_VIEW);
		pane_damaged(p, DAMAGED_CONTENT);
		return 0;
	}

	if (start && end && start->seq > end->seq) {
		start = ci->mark2;
		end = ci->mark;
	}

	if (!start) {
		start = vmark_at_or_before(ci->home, end, rl->typenum, p);
		if (!start)
			/* change is before visible region */
			return 0;
		/* FIXME check 'start' is at least 'num' before end */
	}
	if (!end) {
		end = vmark_at_or_before(ci->home, start, rl->typenum, p);
		if (!end)
			return 0;
		end = vmark_next(end);
		if (!end)
			return 0;
		/* FIXME check that 'end' is at least 'num' after start */
	}
	end = vmark_at_or_before(ci->home, end, rl->typenum, p);

	if (!end)
		/* Change before visible region */
		return 1;

	while (end && mark_ordered_or_same(start, end)) {
		free(end->mdata);
		end->mdata = NULL;
		end = vmark_prev(end);
	}
	/* Must be sure to clear the line *before* the change */
	if (end) {
		free(end->mdata);
		end->mdata = NULL;
	}
	pane_damaged(p, DAMAGED_CONTENT);

	return 1;
}

DEF_CMD(render_lines_clip)
{
	struct rl_data *rl = ci->home->data;

	marks_clip(ci->home, ci->mark, ci->mark2, rl->typenum, ci->home);
	if (rl->old_point)
		mark_clip(rl->old_point, ci->mark, ci->mark2);
	return 0;
}

DEF_CMD(render_lines_attach);
DEF_CMD(render_lines_clone)
{
	struct pane *parent = ci->focus;

	render_lines_attach.func(ci);
	pane_clone_children(ci->home, parent->focus);
	return 1;
}

DEF_CMD(render_lines_resize)
{
	struct pane *p = ci->home;
	struct rl_data *rl = p->data;

	pane_resize(p, 0, 0, p->parent->w, p->parent->h);
	pane_resize(rl->helper, 0, 0, p->w, p->h);
	/* Allow propagation to children */
	return 0;
}

static struct map *rl_map;

DEF_LOOKUP_CMD(render_lines_handle, rl_map)

static void render_lines_register_map(void)
{
	rl_map = key_alloc();

	key_add(rl_map, "Move-View-Small", &render_lines_move);
	key_add(rl_map, "Move-View-Large", &render_lines_move);
	key_add(rl_map, "Move-View-Pos", &render_lines_move_pos);
	key_add(rl_map, "Move-View-Line", &render_lines_view_line);
	key_add(rl_map, "Move-CursorXY", &render_lines_set_cursor);
	key_add(rl_map, "Move-Line", &render_lines_move_line);

	/* Make it easy to stop ignoring point */
	key_add(rl_map, "Abort", &render_lines_abort);

	key_add(rl_map, "Close", &render_lines_close);
	key_add(rl_map, "Free", &edlib_do_free);
	key_add(rl_map, "Clone", &render_lines_clone);
	key_add(rl_map, "Refresh", &render_lines_refresh);
	key_add(rl_map, "Refresh:view", &render_lines_refresh_view);
	key_add(rl_map, "Refresh:size", &render_lines_resize);
	key_add(rl_map, "Notify:clip", &render_lines_clip);
	key_add(rl_map, "get-attr", &render_lines_get_attr);

	key_add(rl_map, "doc:replaced", &render_lines_notify_replace);
	/* view:changed is sent to a tile when the display might need
	 * to change, even though the doc may not have*/
	key_add(rl_map, "view:changed", &render_lines_notify_replace);
}

REDEF_CMD(render_lines_attach)
{
	struct rl_data *rl = calloc(1, sizeof(*rl));
	struct pane *p;

	if (!rl_map)
		render_lines_register_map();

	rl->target_x = -1;
	rl->target_y = -1;
	rl->do_wrap = 1;
	rl->c = rl_cb;
	p = ci->focus;
	if (strcmp(ci->key, "attach-render-text") == 0)
		p = call_ret(pane, "attach-markup", p);
	p = pane_register(p, 0, &render_lines_handle.c, rl);
	if (!p) {
		free(rl);
		return Efail;
	}
	rl->helper = call_ret(pane, "attach-renderline", p);
	rl->typenum = home_call(ci->focus, "doc:add-view", p) - 1;
	call("doc:request:doc:replaced", p);

	return comm_call(ci->comm2, "callback:attach", p);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &render_lines_attach, 0, NULL,
		  "attach-render-lines");
	call_comm("global-set-command", ed, &render_lines_attach, 0, NULL,
		  "attach-render-text");
}
