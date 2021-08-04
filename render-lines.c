/*
 * Copyright Neil Brown ©2015-2021 <neil@brown.name>
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

#define	MARK_DATA_PTR struct pane
#define _GNU_SOURCE /*  for asprintf */
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
	int		skip_height; /* Skip display-lines for first "line" */
	int		skip_line_height; /* height of lines in skip_height */
	int		tail_height; /* display lines at eop not display */
	int		cursor_line; /* line that contains the cursor starts
				      * on this line */
	short		target_x, target_y;
	short		i_moved;	/* I moved cursor, so don't clear
					 * target
					 */
	int		do_wrap;
	short		shift_left;
	short		shift_left_last_refresh;
	struct mark	*header;
	int		typenum;
	int		repositioned; /* send "render:reposition" when we know
				       * full position again.
				       */
	short		lines; /* lines drawn before we hit eof */
	short		cols; /* columns used for longest line */
	short		margin; /* distance from top/bottom required for cursor */
	bool		background_drawn;

	/* If cursor not visible, we add this pane in bottom-right and place
	 * cursor there.
	 */
	struct pane	*cursor_pane;
};

static void vmark_clear(struct mark *m safe)
{
	if (m->mdata) {
		pane_close(m->mdata);
		m->mdata = NULL;
	}
}

static void vmark_free(struct mark *m safe)
{
	vmark_clear(m);
	mark_free(m);
}

static void vmark_set(struct pane *p safe, struct mark *m safe, char *line safe)
{
	if (!m->mdata)
		m->mdata = call_ret(pane, "attach-renderline", p);
	if (m->mdata)
		pane_call(m->mdata, "render-line:set", p, 0, NULL, line);
}

static void vmark_invalidate(struct mark *m safe)
{
	if (m->mdata)
		pane_damaged(m->mdata, DAMAGED_VIEW);
}

static bool vmark_is_valid(struct mark *m safe)
{
	return m->mdata && !(m->mdata->damaged & DAMAGED_VIEW);
}

/* Returns 'true' at end-of-page */
static bool measure_line(struct pane *p safe, struct pane *focus safe,
			 struct mark *mk safe, short cursor_offset)
{
	struct pane *hp = mk->mdata;
	int ret = 0;

	if (hp) {
		pane_resize(hp, hp->x, hp->y, p->w, p->h);
		ret = pane_call(hp, "render-line:measure",
				focus, cursor_offset);
	}
	/* end-of-page flag */
	return ret == 2;
}

/* Returns offset of posx,posy */
static int find_xy_line(struct pane *p safe, struct pane *focus safe,
			struct mark *mk safe, short posx, short posy)
{
	struct pane *hp = mk->mdata;
	int ret = 0;

	if (hp) {
		ret = pane_call(hp,
				"render-line:findxy",
				focus,
				-1, NULL, NULL,
				0, NULL, NULL,
				posx - hp->x, posy - hp->y);
	}
	/* xypos */
	return ret > 0 ? (ret - 1) : -1;
}

static void draw_line(struct pane *p safe, struct pane *focus safe,
		      struct mark *mk safe, short offset, bool refresh_all)
{
	struct pane *hp = mk->mdata;

	if (hp &&
	    (refresh_all || hp->damaged & DAMAGED_REFRESH)) {
		hp->damaged &= ~DAMAGED_REFRESH;
		pane_call(hp, "render-line:draw", focus, offset);
	}
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
		/* if n>0 we can fail because start-of-file was found before
		 * any newline.  In that case ret == Efail, and we return NULL.
		 */
		if (found)
			*found = (ret == Efail);
		mark_free(m);
		return NULL;
	}
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

static void call_render_line(struct pane *home safe, struct pane *p safe,
			     struct mark *start safe, struct mark **end)
{
	struct mark *m, *m2;
	char *s;

	if (vmark_is_valid(start))
		return;

	m = mark_dup_view(start);
	if (doc_following(p, m) == WEOF) {
		/* We only create a subpane for EOF when it is at start
		 * of line, else it is included in the preceding line.
		 */
		call("doc:render-line-prev", p, 0, m);
		if (!mark_same(m, start)) {
			mark_free(m);
			vmark_clear(start);
			return;
		}
		s = "";
	} else
		s = call_ret(strsave, "doc:render-line", p, NO_NUMERIC, m);

	if (s)
		vmark_set(home, start, s);

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
		if (end && m == *end)
			*end = m2;
		vmark_free(m);
	}
	/* Any mark at same location as m2 must go too. */
	while ((m = vmark_next(m2)) != NULL &&
	       mark_same(m, m2)) {
		if (end && m == *end)
			*end = m2;
		vmark_free(m);
	}
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
		while (l >=4 && strncmp(ci->str+l-3, "</>", 3) == 0 &&
		ci->str[l-4] != '<')
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

static bool step_back(struct pane *p safe, struct pane *focus safe,
		      struct mark **startp safe, struct mark **endp,
		      short *y_pre safe, short *line_height_pre safe)
{
	/* step backwards moving start */
	struct rl_data *rl = p->data;
	struct mark *m;
	bool found_start = False;
	struct mark *start = *startp;

	if (!start)
		return True;
	m = call_render_line_prev(focus, mark_dup_view(start),
				  1, &rl->top_sol);
	if (!m) {
		/* no text before 'start' */
		found_start = True;
	} else {
		short h = 0;
		start = m;
		call_render_line(p, focus, start, endp);
		measure_line(p, focus, start, -1);
		h = start->mdata ? start->mdata->h : 0;
		if (h) {
			*y_pre = h;
			*line_height_pre =
				attr_find_int(start->mdata->attrs,
					      "line-height");
		} else
			found_start = True;
	}
	*startp = start;
	return found_start;
}

static bool step_fore(struct pane *p safe, struct pane *focus safe,
		      struct mark **startp safe, struct mark **endp safe,
		      short *y_post safe, short *line_height_post safe)
{
	struct mark *end = *endp;
	bool found_end = False;

	if (!end)
		return True;
	call_render_line(p, focus, end, startp);
	found_end = measure_line(p, focus, end, -1);
	if (end->mdata)
		*y_post = end->mdata->h;
	if (*y_post > 0 && end->mdata)
		*line_height_post =
			attr_find_int(end->mdata->attrs,
				      "line-height");
	if (!end->mdata || !end->mdata->h)
		end = NULL;
	else
		end = vmark_next(end);
	if (!end) {
		found_end = 1;
		if (p->h >= *line_height_post *2)
			*y_post = p->h / 10;
	}

	*endp = end;
	return found_end;
}

static int consume_space(struct pane *p safe, int y,
			 short *y_prep safe, short *y_postp safe,
			 short *lines_above safe, short *lines_below safe,
			 int found_start, int found_end,
			 int line_height_pre, int line_height_post)
{
	int y_pre = *y_prep;
	int y_post = *y_postp;

	if (y_pre > 0 && y_post > 0) {
		int consume = (y_post < y_pre
			       ? y_post : y_pre) * 2;
		int above, below;
		if (consume > p->h - y)
			consume = p->h - y;
		if (y_pre > y_post) {
			above = consume - (consume/2);
			below = consume/2;
		} else {
			below = consume - (consume/2);
			above = consume/2;
		}
		y += above + below;
		y_pre -= above;
		*lines_above += above / (line_height_pre?:1);
		y_post -= below;
		*lines_below += below / (line_height_post?:1);
		/* We have just consumed all of one of
		 * lines_{above,below} so they are no longer
		 * both > 0
		 */
	}
	if (found_end && y_pre) {
		int consume = p->h - y;
		if (consume > y_pre)
			consume = y_pre;
		y_pre -= consume;
		y += consume;
		*lines_above += consume / (line_height_pre?:1);
	}
	if (found_start && y_post) {
		int consume = p->h - y;
		if (consume > y_post)
			consume = y_post;
		y_post -= consume;
		y += consume;
		*lines_below += consume / (line_height_post?:1);
	}
	*y_prep = y_pre;
	*y_postp = y_post;
	return y;
}

/*
 * Choose new start/end to be displayed in the given pane.
 * 'pm' must be displayed, and if vline is not NO_NUMERIC,
 * pm should be displayed on that line of the display, where
 * negative numbers count from the bottom of the page.
 * Otherwise pm should be at least rl->margin from top and bottom,
 * but in no case should start-of-file be *after* top of display.
 * If there is an existing display, move the display as little as
 * possible while complying with the above.
 *
 * We start at 'pm' and move both forward and backward one line at a
 * time measuring each line and assessing space used.
 * - If the space above pm reaches positive vline, that will be top.
 * - If the space below reaches negative vline, that will likely be bottom
 * - If pm was before old top and we reach the old top going down,
 *    and if space measured before pm has reached ->margin, we stop
 *    moving upward.
 * - If pm was after old bottom and we reach the old bottom going up
 *    and if space measured after pm has reached ->margin, we stop
 *    moving downward
 *
 * If we decide to stop moving in both directions, but have not
 * reached EOF or full height of display, keep moving downwards.
 */
static void find_lines(struct mark *pm safe, struct pane *p safe,
		       struct pane *focus safe,
		       int vline)
{
	struct rl_data *rl = p->data;
	struct mark *orig_top, *orig_bot;
	struct mark *top, *bot;  // boundary of previous display
	struct mark *m;
	struct mark *start, *end; // current estimate for new display
	short y = 0;
	short lines_above = 0, lines_below = 0; /* distance from start/end
						 * to pm.
						 */
	short offset; // pos of pm in rendering of that line
	bool found_start = False, found_end = False;
	/* y_pre and y_post are measurement from start/end that
	 * haven't yet been included into lines_above/lines_below.
	 */
	short y_pre = 0, y_post = 0;
	short line_height_pre = 1, line_height_post = 1;

	orig_top = vmark_first(focus, rl->typenum, p);
	orig_bot = vmark_last(focus, rl->typenum, p);
	/* Protect top/bot from being freed by call_render_line */
	if (orig_top)
		orig_top = mark_dup(orig_top);
	if (orig_bot)
		orig_bot = mark_dup(orig_bot);

	start = vmark_new(focus, rl->typenum, p);
	if (!start)
		goto abort;
	rl->repositioned = 1;
	mark_to_mark(start, pm);
	start = call_render_line_prev(focus, start, 0, &rl->top_sol);
	if (!start)
		goto abort;
	offset = call_render_line_to_point(focus, pm, start);
	call_render_line(p, focus, start, NULL);
	end = vmark_next(start);
	/* Note: 'end' might be NULL if 'start' is end-of-file, otherwise
	 * call_render_line() will have created 'end' if it didn't exist.
	 */

	rl->shift_left = 0;

	/* ->cy is top of cursor, we want to measure from bottom */
	if (start->mdata) {
		struct pane *hp = start->mdata;
		int curs_width;
		found_end = measure_line(p, focus, start, offset);

		curs_width = pane_attr_get_int(
			start->mdata, "curs_width", 1);
		while (!rl->do_wrap && curs_width > 0 &&
		       hp->cx + curs_width >= p->w) {
			int shift = 8 * curs_width;
			if (shift > hp->cx)
				shift = hp->cx;
			rl->shift_left += shift;
			measure_line(p, focus, start, offset);
		}
		line_height_pre = attr_find_int(start->mdata->attrs, "line-height");
		if (line_height_pre < 1)
			line_height_pre = 1;
		y_pre = start->mdata->cy + line_height_pre;
		y_post = start->mdata->h - y_pre;
	} else {
		/* Should never happen */
		y_pre = 0;
		y_post = 0;
	}
	if (!end) {
		found_end = True;
		if (p->h > line_height_pre * 2)
			y_post += p->h / 10;
		else
			/* Small display, no space at EOF */
			y_post = 0;
	}
	y = 0;
	if (rl->header && rl->header->mdata)
		y = rl->header->mdata->h;

	/* We have start/end of the focus line.  When rendered this,
	 * plus header and eof-footed would use y_pre + y + y_post
	 * vertical space.
	 */

	if (vline != NO_NUMERIC) {
		/* ignore current position - top/bot irrelevant */
		top = NULL;
		bot = NULL;
	} else {
		top = orig_top;
		bot = orig_bot;
	}

	while ((!found_start || !found_end) && y < p->h) {
		if (vline != NO_NUMERIC) {
			if (!found_start && vline > 0 &&
			    lines_above >= vline-1)
				found_start = True;
			if (!found_end && vline < 0 &&
			    lines_below >= -vline-1)
				found_end = True;
		}
		if (!found_start && y_pre <= 0)
			found_start = step_back(p, focus, &start, &end,
						&y_pre, &line_height_pre);

		if (found_end && y_post && bot &&
		    mark_ordered_or_same(start, bot))
			/* Extra vertical space gets inserted after EOF when
			 * there is a long jump to get there, but if we hit 'bot'
			 * soon when searching back, we discard any unused space.
			 */
			y_post = 0;

		if (!found_end && bot &&
		    (!end || mark_ordered_or_same(bot, end)) &&
		    lines_below >= rl->margin)
			if (mark_ordered_not_same(start, bot) ||
			    /* Overlap original from below, so prefer to
			     * maximize that overlap.
			     */
			    (mark_same(start, bot) &&
			     y_pre - rl->skip_height >= y_post))
				/* No overlap in marks yet, but over-lap in
				 * space, so same result as above.
				 */
				found_end = True;

		if (!found_end && y_post <= 0)
			/* step forwards */
			found_end = step_fore(p, focus, &start, &end,
					      &y_post, &line_height_post);

		if (!found_start && top && end &&
		    mark_ordered_or_same(start, top) &&
		    lines_above >= rl->margin)
			if (mark_ordered_not_same(top, end) ||
			    (mark_same(top, end) &&
			     y_post - rl->tail_height >= y_pre))
				found_start = True;

		y = consume_space(p, y, &y_pre, &y_post,
				  &lines_above, &lines_below,
				  found_start, found_end,
				  line_height_pre, line_height_post);
	}
	/* We might need to continue downwards even after found_end
	 * if there is more space.
	 */
	found_end = end == NULL;
	while (!found_end && y < p->h) {
		if (y_post <= 0)
			found_end = step_fore(p, focus, &start, &end,
					      &y_post, &line_height_post);
		y = consume_space(p, y, &y_pre, &y_post,
				  &lines_above, &lines_below,
				  found_start, found_end,
				  line_height_pre, line_height_post);
	}

	if (start->mdata && start->mdata->h <= y_pre) {
		y_pre = 0;
		m = vmark_next(start);
		vmark_free(start);
		start = m;
	}
	if (!start)
		goto abort;

	rl->skip_height = y_pre;
	rl->skip_line_height = line_height_pre;
	rl->tail_height = y_post;
	/* Now discard any marks outside start-end */
	if (end && end->seq < start->seq)
		/* something confused, make sure we don't try to use 'end' after
		 * freeing it.
		 */
		end = start;
	while ((m = vmark_prev(start)) != NULL)
		vmark_free(m);

	if (end) {
		while ((m = vmark_next(end)) != NULL)
			vmark_free(m);

		vmark_clear(end);
	}

	y = 0;
	if (rl->header && rl->header->mdata)
		y = rl->header->mdata->h;
	y -= rl->skip_height;
	for (m = vmark_first(focus, rl->typenum, p);
	     m && m->mdata ; m = vmark_next(m)) {
		struct pane *hp = m->mdata;
		pane_resize(hp, hp->x, y, hp->w, hp->h);
		y += hp->h;
	}
	pane_damaged(p, DAMAGED_REFRESH);
	m = vmark_first(focus, rl->typenum, p);
	if (!m || !orig_top || !mark_same(m, orig_top))
		rl->repositioned = 1;
	m = vmark_last(focus, rl->typenum, p);
	if (!m || !orig_bot || !mark_same(m, orig_bot))
		rl->repositioned = 1;

abort:
	mark_free(orig_top);
	mark_free(orig_bot);
}

DEF_CMD(cursor_handle)
{
	return 0;
}

static int render(struct mark *pm, struct pane *p safe,
		  struct pane *focus safe)
{
	struct rl_data *rl = p->data;
	short y = 0;
	struct mark *m, *m2;
	struct xy scale = pane_scale(focus);
	char *s;
	int hide_cursor = 0;
	int cursor_drawn = 0;
	bool refresh_all = rl->shift_left != rl->shift_left_last_refresh;

	rl->shift_left_last_refresh = rl->shift_left;
	s = pane_attr_get(focus, "hide-cursor");
	if (s && strcmp(s, "yes") == 0)
		hide_cursor = 1;

	rl->cols = 0;
	m = vmark_first(focus, rl->typenum, p);
	if (!rl->background_drawn)
		refresh_all = True;
	s = pane_attr_get(focus, "background");
	if (s && strncmp(s, "call:", 5) == 0) {
		home_call(focus, "Draw:clear", p, 0, NULL, "");
		home_call(focus, s+5, p, 0, m);
		refresh_all = True;
	} else if (rl->background_drawn)
		;
	else if (!s)
		home_call(focus, "Draw:clear", p, 0, NULL, "");
	else if (strncmp(s, "color:", 6) == 0) {
		char *a = strdup(s);
		strcpy(a, "bg:");
		strcpy(a+3, s+6);
		home_call(focus, "Draw:clear", p, 0, NULL, a);
		free(a);
	} else if (strncmp(s, "image:", 6) == 0) {
		home_call(focus, "Draw:clear", p);
		home_call(focus, "Draw:image", p, 1, NULL, s+6);
	} else
		home_call(focus, "Draw:clear", p, 0, NULL, "");
	rl->background_drawn = True;

	if (rl->header && vmark_is_valid(rl->header)) {
		struct pane *hp = rl->header->mdata;
		draw_line(p, focus, rl->header, -1, refresh_all);
		y = hp->h;
		rl->cols = hp->x + hp->w;
	}
	y -= rl->skip_height;

	p->cx = p->cy = -1;
	rl->cursor_line = 0;

	while (m && m->mdata) {
		m2 = vmark_next(m);
		if (!hide_cursor && p->cx <= 0 && pm &&
		    mark_ordered_or_same(m, pm) &&
		    (!(m2 && doc_following(focus, m2) != WEOF) ||
		     mark_ordered_not_same(pm, m2))) {
			struct xy curs;
			struct pane *hp = m->mdata;
			short len = call_render_line_to_point(focus, pm,
							      m);
			draw_line(p, focus, m, len, True);
			rl->cursor_line = hp->y + hp->cy;
			curs = pane_mapxy(hp, p, hp->cx, hp->cy, False);
			if (hp->cx < 0) {
				p->cx = -1;
				p->cy = -1;
			} else {
				p->cx = curs.x;
				p->cy = curs.y;
			}
			cursor_drawn = 1;
		} else {
			draw_line(p, focus, m, -1, refresh_all);
		}
		if (m->mdata) {
			int cols = m->mdata->x + m->mdata->w;
			if (cols > rl->cols)
				rl->cols = cols;
			y = m->mdata->y + m->mdata->h;
		}
		m = m2;
	}
	if (!cursor_drawn && !hide_cursor) {
		/* Place cursor in bottom right */
		struct pane *cp = rl->cursor_pane;
		short mwidth = -1;
		short lineheight;

		if (!cp) {
			cp = pane_register(p, -1, &cursor_handle);
			rl->cursor_pane = cp;
		}
		if (m)
			m2 = vmark_prev(m);
		else
			m2 = vmark_last(focus, rl->typenum, p);

		while (m2 && mwidth <= 0) {
			if (m2->mdata) {
				mwidth = pane_attr_get_int(
					m2->mdata, "curs_width", -1);
				lineheight = pane_attr_get_int(
					m2->mdata, "line-height", -1);
			}
			m2 = vmark_prev(m2);
		}

		if (mwidth <= 0) {
			mwidth = 1;
			lineheight = 1;
		}
		if (cp) {
			pane_resize(cp,
				    p->w - mwidth,
				    p->h - lineheight,
				    mwidth, lineheight);

			home_call(focus, "Draw:clear", cp);
			home_call(focus, "Draw:text", cp, 0, NULL, " ",
				  scale.x, NULL, "",
				  0, lineheight-1);
		}
	} else if (rl->cursor_pane) {
		pane_close(rl->cursor_pane);
		rl->cursor_pane = NULL;
	}
	return y;
}

DEF_CMD(render_lines_get_attr)
{
	struct rl_data *rl = ci->home->data;

	if (ci->str && strcmp(ci->str, "shift_left") == 0) {
		char ret[10];
		if (rl->do_wrap)
			return comm_call(ci->comm2, "cb", ci->focus,
					 0, NULL, "-1");
		snprintf(ret, sizeof(ret), "%d", rl->shift_left);
		return comm_call(ci->comm2, "cb", ci->focus, 0, NULL, ret);
	}
	return Efallthrough;
}

DEF_CMD(render_lines_point_moving)
{
	struct pane *p = ci->home;
	struct rl_data *rl = p->data;
	struct mark *pt = call_ret(mark, "doc:point", ci->home);
	struct mark *m;

	if (!pt || ci->mark != pt)
		return 1;
	/* Stop igoring point, because it is probably relevant now */
	rl->ignore_point = 0;
	if (!rl->i_moved)
		/* Someone else moved the point, so reset target column */
		rl->target_x = -1;
	m = vmark_at_or_before(ci->focus, pt, rl->typenum, p);
	if (m && vmark_is_valid(m)) {
		pane_damaged(m->mdata, DAMAGED_REFRESH);
		pane_damaged(m->mdata->parent, DAMAGED_REFRESH);
	}
	return 1;
}

static int revalidate_start(struct rl_data *rl safe,
			    struct pane *p safe, struct pane *focus safe,
			    struct mark *start safe, struct mark *pm,
			    bool refresh_all)
{
	int y;
	bool on_screen = False;
	struct mark *m, *m2;
	bool found_end = False;
	bool start_of_file;

	if (pm && !rl->do_wrap) {
		int prefix_len;
		int curs_width;
		/* Need to check if side-shift is needed on cursor line */
		m2 = mark_dup(pm);
		call("doc:render-line-prev", focus, 0, m2);

		m = vmark_at_or_before(focus, m2, rl->typenum, p);
		mark_free(m2);

		if (m && refresh_all)
			vmark_invalidate(m);
		if (m && m->mdata && !vmark_is_valid(m)) {
			pane_damaged(p, DAMAGED_REFRESH);
			call("doc:render-line-prev", focus, 0, m);
			call_render_line(p, focus, m, &start);
		}
		if (m && m->mdata) {
			struct pane *hp = m->mdata;
			int offset = call_render_line_to_point(focus,
							       pm, m);
			measure_line(p, focus, m, offset);
			prefix_len = pane_attr_get_int(
				m->mdata, "prefix_len", -1);
			curs_width = pane_attr_get_int(
				m->mdata, "curs_width", 1);

			while (hp->cx + curs_width >= p->w) {
				int shift = 8 * curs_width;
				if (shift > hp->cx)
					shift = hp->cx;
				rl->shift_left += shift;
				measure_line(p, focus, m, offset);
				refresh_all = 1;
			}
			while (hp->cx < prefix_len &&
			       rl->shift_left > 0 &&
			       hp->cx + curs_width * 8*curs_width < p->w) {
				int shift = 8 * curs_width;
				if (shift > rl->shift_left)
					shift = rl->shift_left;
				rl->shift_left -= shift;
				measure_line(p, focus, m, offset);
				refresh_all = 1;
			}
		}
	}
	y = 0;
	if (rl->header) {
		struct pane *hp = rl->header->mdata;
		if (refresh_all) {
			measure_line(p, focus, rl->header, -1);
			if (hp)
				pane_resize(hp, hp->x, y, hp->w, hp->h);
		}
		if (hp)
			y = hp->h;
	}
	y -= rl->skip_height;
	start_of_file = doc_prior(focus, start) == WEOF;
	for (m = start; m && !found_end && y < p->h; m = vmark_next(m)) {
		struct pane *hp;
		if (refresh_all)
			vmark_invalidate(m);
		call_render_line(p, focus, m, NULL);
		found_end = measure_line(p, focus, m, -1);
		hp = m->mdata;
		if (!hp)
			break;

		if (y != hp->y) {
			pane_damaged(p, DAMAGED_REFRESH);
			pane_resize(hp, hp->x, y, hp->w, hp->h);
		}
		y += hp->h;
		m2 = vmark_next(m);
		if (pm && m == start && rl->skip_height > 0 && m2 &&
		    mark_ordered_not_same(pm, m2)) {
			/* Point might be in this line, but off top
			 * of the screen
			 */
			int offset = call_render_line_to_point(focus,
							       pm, m);
			if (offset >= 0) {
				measure_line(p, focus, m, offset);
				if (hp->cy >= rl->skip_height + rl->margin)
					/* Cursor is visible on this line
					 * and after margin from top.
					 */
					on_screen = True;
				else if (start_of_file && rl->skip_height == 0)
					/* Cannot make more margin space */
					on_screen = True;
			}
		} else if (pm && y >= p->h && m->seq < pm->seq) {
			/* point might be in this line, but off end
			 * of the screen
			 */
			int offset = call_render_line_to_point(focus,
							       pm, m);
			if (offset > 0) {
				int lh;
				measure_line(p, focus, m, offset);
				lh = attr_find_int(hp->attrs,
						   "line-height");
				if (lh <= 0)
					lh = 1;
				if (y - hp->h + hp->cy <= p->h - lh - rl->margin) {
					/* Cursor is on screen */
					on_screen = True;
				}
			}
		} else if (pm && mark_ordered_or_same(m, pm) && m2 &&
			   mark_ordered_or_same(pm, m2)) {
			if (rl->margin == 0)
				on_screen = True;
			else {
				int offset = call_render_line_to_point(
					focus, pm, m);
				if (offset > 0) {
					int lh;
					int cy;
					measure_line(p, focus, m, offset);
					lh = attr_find_int(hp->attrs,
							   "line-height");
					cy = y - hp->h + hp->cy;
					if (cy >= rl->margin &&
					    cy <= p->h - rl->margin - lh)
						/* Cursor at least margin from edge */
						on_screen = True;
				}
			}
		}
	}
	if (y >= p->h)
		rl->tail_height = p->h - y;
	else
		rl->tail_height = 0;
	if (m) {
		vmark_clear(m);
		while ((m2 = vmark_next(m)) != NULL) {
			/* end of view has clearly changed */
			rl->repositioned = 1;
			vmark_free(m2);
		}
	}
	if (!pm || on_screen) {
		if (rl->repositioned) {
			rl->repositioned = 0;
			call("render:reposition", focus,
			     rl->lines, vmark_first(focus,
						    rl->typenum,
						    p), NULL,
			     rl->cols, vmark_last(focus,
						  rl->typenum,
						  p), NULL,
			     p->cx, p->cy);
		}
		return 1;
	}
	return 0;
}

DEF_CMD(render_lines_revise)
{
	struct pane *p = ci->home;
	struct pane *focus = ci->focus;
	struct rl_data *rl = p->data;
	struct mark *pm = NULL;
	struct mark *m1, *m2;
	bool refresh_all = False;
	char *hdr;
	char *a;

	a = pane_attr_get(focus, "render-wrap");
	if (rl->do_wrap != (!a || strcmp(a, "yes") ==0)) {
		rl->do_wrap = (!a || strcmp(a, "yes") ==0);
		refresh_all = True;
	}

	rl->margin = pane_attr_get_int(focus, "render-vmargin", 0);
	if (rl->margin >= p->h/2)
		rl->margin = p->h/2;

	hdr = pane_attr_get(focus, "heading");
	if (hdr && !*hdr)
		hdr = NULL;

	if (hdr) {
		if (!rl->header)
			rl->header = vmark_new(focus, MARK_UNGROUPED, NULL);
		if (rl->header) {
			vmark_set(p, rl->header, hdr);
			measure_line(p, focus, rl->header, -1);
		}
	} else if (rl->header) {
		vmark_free(rl->header);
		rl->header = NULL;
	}

	if (!rl->ignore_point)
		pm = call_ret(mark, "doc:point", focus);
	m1 = vmark_first(focus, rl->typenum, p);
	m2 = vmark_last(focus, rl->typenum, p);

	if (m1 && !vmark_is_valid(m1))
		/* newline before might have been deleted, better check */
		call("doc:render-line-prev", focus, 0, m1);
	// FIXME double check that we invalidate line before any change...

	if (m1 && m2 &&
	    (!pm || (mark_ordered_or_same(m1,pm)))) {
		/* We maybe be able to keep m1 as start, if things work out.
		 * So check all sub-panes are still valid and properly
		 * positioned.
		 */
		if (revalidate_start(rl, p, focus, m1, pm, refresh_all))
			return 1;
	}
	/* Need to find a new top-of-display */
	if (!pm)
		pm = call_ret(mark, "doc:point", focus);
	if (!pm)
		/* Don't know what to do here... */
		return 1;
	find_lines(pm, p, focus, NO_NUMERIC);
	rl->repositioned = 0;
	call("render:reposition", focus,
	     rl->lines, vmark_first(focus, rl->typenum, p), NULL,
	     rl->cols, vmark_last(focus, rl->typenum, p), NULL,
	     p->cx, p->cy);
	return 1;
}

DEF_CMD(render_lines_refresh)
{
	struct pane *p = ci->home;
	struct pane *focus = ci->focus;
	struct rl_data *rl = p->data;
	struct mark *m, *pm = NULL;

	//pane_damaged(p, DAMAGED_VIEW);

	pm = call_ret(mark, "doc:point", focus);

	m = vmark_first(focus, rl->typenum, p);

	if (!m)
		return 1;

	rl->lines = render(pm, p, focus);

	return 1;
}

DEF_CMD(render_lines_close)
{
	struct rl_data *rl = ci->home->data;

	if (rl->header)
		vmark_free(rl->header);
	rl->header = NULL;

	return 1;
}

DEF_CMD(render_lines_close_mark)
{
	struct mark *m = ci->mark;

	if (m)
		vmark_clear(m);
	return 1;
}

DEF_CMD(render_lines_abort)
{
	struct pane *p = ci->home;
	struct rl_data *rl = p->data;

	rl->ignore_point = 0;
	rl->target_x = -1;

	pane_damaged(p, DAMAGED_VIEW);

	/* Allow other handlers to complete the Abort */
	return Efallthrough;
}

DEF_CMD(render_lines_move_view)
{
	/*
	 * Find a new 'top' for the displayed region so that render()
	 * will draw from there.
	 * When moving backwards we move back a line and render it.
	 * When moving forwards we render and then step forward
	 * At each point we count the number of display lines that result.
	 * When we choose a new start, we delete all earlier marks.
	 * We also delete marks before current top when moving forward
	 * where there are more than a page full.
	 */
	struct pane *p = ci->home;
	struct pane *focus = ci->focus;
	int rpt = RPT_NUM(ci);
	struct rl_data *rl = p->data;
	struct mark *top, *old_top;

	top = vmark_first(focus, rl->typenum, p);
	if (!top)
		return Efallthrough;

	old_top = mark_dup(top);
	rpt *= p->h ?: 1;
	rpt /= 1000;

	rl->ignore_point = 1;

	if (rl->skip_line_height <= 0)
		rl->skip_line_height = 1;

	if (rpt < 0) {
		/* Need to add new lines at the top and remove
		 * at the bottom.
		 */
		while (rpt < 0) {
			short y = 0;
			struct mark *m;
			struct mark *prevtop = top;

			if (rl->skip_height) {
				rl->skip_height -= rl->skip_line_height;
				if (rl->skip_height < rl->skip_line_height/2)
					rl->skip_height = 0;
				rpt += rl->skip_line_height;
				if (rpt > 0)
					rpt = 0;
				continue;
			}

			m = mark_dup_view(top);
			top = call_render_line_prev(focus, m,
						    1, &rl->top_sol);
			if (!top && doc_prior(focus, prevtop) != WEOF) {
				/* Double check - maybe a soft top-of-file - Ctrl-L*/
				m = mark_dup(prevtop);
				doc_prev(focus, m);
				top = call_render_line_prev(focus, m,
							    1, &rl->top_sol);
			}
			if (!top)
				break;
			m = top;
			while (m && m->seq < prevtop->seq &&
			       !mark_same(m, prevtop)) {
				call_render_line(p, focus, m, NULL);
				if (m->mdata == NULL) {
					rpt = 0;
					break;
				}
				measure_line(p, focus, m, -1);
				y += m->mdata->h;
				m = vmark_next(m);
			}
			/* FIXME remove extra lines, maybe add */
			rl->skip_height = y;
		}
	} else {
		/* Need to remove lines from top */
		call_render_line(p, focus, top, NULL);
		measure_line(p, focus, top, -1);
		while (top && top->mdata && rpt > 0) {
			short y = 0;

			y = top->mdata->h;
			if (rpt < y - rl->skip_height) {
				rl->skip_height += rpt;
				break;
			}
			rpt -= y - rl->skip_height;
			rl->skip_height = 0;
			top = vmark_next(top);
			if (!top)
				break;
			call_render_line(p, focus, top, NULL);
			measure_line(p, focus, top, -1);
		}
		if (top && top->mdata) {
			/* We didn't fall off the end, so it is OK to remove
			 * everything before 'top'
			 */
			struct mark *old;
			while ((old = vmark_first(focus, rl->typenum, p)) != NULL &&
			       old != top)
				vmark_free(old);
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
	/* ->num is
	 * 1 if this resulted from a click
	 * 2 if from a release
	 * 3 if from motion
	 * 0 any other reason.
	 */
	struct pane *p = ci->home;
	struct pane *focus = ci->focus;
	struct rl_data *rl = p->data;
	struct mark *m;
	struct mark *m2 = NULL;
	struct xy cih;
	int xypos;

	cih = pane_mapxy(ci->focus, ci->home,
			 ci->x >= 0 ? ci->x : p->cx >= 0 ? p->cx : 0,
			 ci->y >= 0 ? ci->y : p->cy >= 0 ? p->cy : 0,
			 False);

	m = vmark_first(p, rl->typenum, p);

	while (m && m->mdata && m->mdata->y + m->mdata->h <= cih.y &&
	       vmark_next(m))
		m = vmark_next(m);

	if (!m)
		/* There is nothing rendered? */
		return 1;
	if (!m->mdata) {
		/* chi is after the last visible content, and m is the end
		 * of that content (possible EOF) so move there
		 */
	} else {
		if (cih.y < m->mdata->y)
			cih.y = m->mdata->y;
		xypos = find_xy_line(p, focus, m, cih.x, cih.y);
		if (xypos >= 0)
			m2 = call_render_line_offset(focus, m, xypos);
	}
	if (m2) {
		char *tag, *xyattr;

		if (ci->num == 2) { /* Mouse release */
			xyattr = pane_attr_get(m->mdata, "xyattr");
			tag = get_active_tag(xyattr);
			if (tag) {
				char *c = NULL;
				asprintf(&c, "Mouse-Activate:%s", tag);
				if (c)
					call(c, focus, 0, m2, tag,
					     0, ci->mark, xyattr);
				free(c);
			}
		}
		m = m2;
	} else {
		/* m is the closest we'll get */
	}

	if (ci->mark)
		mark_to_mark(ci->mark, m);
	else
		call("Move-to", focus, 0, m);
	mark_free(m2);

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
	if (top && rl->skip_height)
		/* top line not fully displayed, being in that line is
		 * not sufficient */
		top = vmark_next(top);
	if (bot)
		/* last line might not be fully displayed, so don't assume */
		bot = vmark_prev(bot);
	if (!top || !bot ||
	    !mark_ordered_or_same(top, pm) ||
	    !mark_ordered_not_same(pm, bot))
		/* pos not displayed */
		find_lines(pm, p, focus, NO_NUMERIC);
	pane_damaged(p, DAMAGED_REFRESH);
	return 1;
}

DEF_CMD(render_lines_view_line)
{
	struct pane *p = ci->home;
	struct pane *focus = ci->focus;
	struct rl_data *rl = p->data;
	struct mark *pm = ci->mark;
	int line = ci->num;

	if (!pm)
		return Enoarg;
	if (line == NO_NUMERIC)
		return Einval;

	rl->ignore_point = 1;
	find_lines(pm, p, focus, line);
	pane_damaged(p, DAMAGED_REFRESH);
	return 1;
}

DEF_CMD(render_lines_move_line)
{
	/* FIXME should be able to select between display lines
	 * and content lines - different when a line wraps.
	 * For now just content lines.
	 * target_x and target_y are the target location in a line
	 * relative to the start of line.
	 * We use doc:EOL to find a suitable start of line, then
	 * render that line and find the last location not after x,y
	 */
	struct pane *p = ci->home;
	struct pane *focus = ci->focus;
	struct rl_data *rl = p->data;
	int num;
	int xypos = -1;
	struct mark *m = ci->mark;
	struct mark *start;

	if (!m)
		m = call_ret(mark, "doc:point", focus);
	if (!m)
		return Efail;

	if (rl->target_x < 0) {
		rl->target_x = p->cx;
		rl->target_y = p->cy - rl->cursor_line;
	}
	if (rl->target_x < 0)
		/* maybe not displayed yet */
		rl->target_x = rl->target_y = 0;

	rl->i_moved = 1;
	num = RPT_NUM(ci);
	if (num < 0)
		num -= 1;
	else
		num += 1;
	if (call("doc:EOL", ci->focus, num, m) <= 0) {
		rl->i_moved = 0;
		return Efail;
	}
	if (RPT_NUM(ci) > 0) {
		/* at end of target line, move to start */
		if (call("doc:EOL", ci->focus, -1, m) <= 0) {
			rl->i_moved = 0;
			return Efail;
		}
	}

	start = vmark_new(focus, rl->typenum, p);

	if (start) {
		mark_to_mark(start, m);
		start = call_render_line_prev(focus, start, 0, NULL);
	}

	if (!start) {
		pane_damaged(p, DAMAGED_VIEW);
		rl->i_moved = 0;
		return 1;
	}
	if (vmark_first(focus, rl->typenum, p) == start) {
		/* New first mark, so view will have changed */
		rl->repositioned = 1;
	}

	if (rl->target_x == 0 && rl->target_y == 0) {
		/* No need to move to target column - already there.
		 * This simplifies life for render-complete which is
		 * always at col 0, and messes with markup a bit.
		 */
		rl->i_moved = 0;
		return 1;
	}
	/* FIXME only do this if point is active/volatile, or
	 * if start->mdata is NULL
	 */
	vmark_invalidate(start);
	call_render_line(p, focus, start, NULL);
	if (start->mdata)
		xypos = find_xy_line(p, focus, start, rl->target_x,
				     rl->target_y + start->mdata->y);

	/* xypos is the distance from start-of-line to the target */
	if (xypos >= 0) {
		struct mark *m2 = call_render_line_offset(
			focus, start, xypos);
		if (m2)
			mark_to_mark(m, m2);
		mark_free(m2);
	}
	rl->i_moved = 0;
	return 1;
}

DEF_CMD(render_lines_notify_replace)
{
	struct pane *p = ci->home;
	struct rl_data *rl = p->data;
	struct mark *start = ci->mark;
	struct mark *end = ci->mark2;
	struct mark *first;

	if (strcmp(ci->key, "doc:replaced") == 0) {
		struct mark *pt = call_ret(mark, "doc:point", ci->home);

		/* If anyone changes the doc, reset the target.  This might
		 * be too harsh, but I mainly want target tracking for
		 * close-in-time movement, so it probably doesn't matter.
		 */
		rl->target_x = -1;

		/* If the replacement happened at 'point', then stop
		 * ignoring it.
		 */
		if (ci->mark2 == pt)
			rl->ignore_point = 0;
	}

	if (strcmp(ci->key, "view:changed") == 0)
		/* Cursor possibly moved, so need to refresh */
		pane_damaged(ci->home, DAMAGED_REFRESH);

	if (!start && !end) {
		/* No marks given - assume everything changed */
		struct mark *m;
		for (m = vmark_first(p, rl->typenum, p);
		     m;
		     m = vmark_next(m))
			vmark_invalidate(m);

		pane_damaged(p, DAMAGED_VIEW);
		return Efallthrough;
	}

	if (start && end && start->seq > end->seq) {
		start = ci->mark2;
		end = ci->mark;
	}

	if (strcmp(ci->key, "doc:replaced") == 0) {
		first = vmark_first(ci->home, rl->typenum, p);
		if (first && start &&  end && mark_same(first, end))
			/* Insert just before visible region */
			mark_to_mark(first, start);
	}

	if (start) {
		start = vmark_at_or_before(ci->home, start, rl->typenum, p);
		if (!start)
			start = vmark_first(ci->home, rl->typenum, p);
	} else {
		start = vmark_at_or_before(ci->home, end, rl->typenum, p);
		if (!start)
			/* change is before visible region */
			return Efallthrough;
		/* FIXME check 'start' is at least 'num' before end */
	}
	if (end) {
		end = vmark_at_or_before(ci->home, end, rl->typenum, p);
		if (!end)
			end = vmark_last(ci->home, rl->typenum, p);
	} else if (start) { /* smatch needs to know start in not NULL */
		end = vmark_at_or_before(ci->home, start, rl->typenum, p);
		if (!end)
			end = vmark_first(ci->home, rl->typenum, p);
		if (!end)
			return Efallthrough;
		if (vmark_next(end))
			end = vmark_next(end);
		/* FIXME check that 'end' is at least 'num' after start */
	}

	if (!end || !start)
		/* Change outside visible region */
		return Efallthrough;

	while (end && mark_ordered_or_same(start, end)) {
		vmark_invalidate(end);
		end = vmark_prev(end);
	}
	/* Must be sure to invalidate the line *before* the change */
	if (end)
		vmark_invalidate(end);

	pane_damaged(p, DAMAGED_VIEW);

	return Efallthrough;
}

DEF_CMD(render_lines_clip)
{
	struct rl_data *rl = ci->home->data;

	marks_clip(ci->home, ci->mark, ci->mark2, rl->typenum, ci->home,
		   !!ci->num);
	if (rl->header)
		mark_clip(rl->header, ci->mark, ci->mark2, !!ci->num);
	return Efallthrough;
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
	struct mark *m;

	for (m = vmark_first(p, rl->typenum, p);
	     m;
	     m = vmark_next(m)) {
		vmark_invalidate(m);
		pane_damaged(m->mdata, DAMAGED_REFRESH);
	}
	rl->background_drawn = False;
	pane_damaged(p, DAMAGED_VIEW);

	/* Allow propagation to children */
	return 0;
}

DEF_CMD(render_send_reposition)
{
	/* Some (probably new) pane wants to know the extend of the
	 * view, so resent render:resposition.
	 */
	struct pane *p = ci->home;
	struct rl_data *rl = p->data;

	rl->repositioned = 1;
	return Efallthrough;
}

static struct map *rl_map;

DEF_LOOKUP_CMD(render_lines_handle, rl_map);

static void render_lines_register_map(void)
{
	rl_map = key_alloc();

	key_add(rl_map, "Move-View", &render_lines_move_view);
	key_add(rl_map, "Move-View-Pos", &render_lines_move_pos);
	key_add(rl_map, "Move-View-Line", &render_lines_view_line);
	key_add(rl_map, "Move-CursorXY", &render_lines_set_cursor);
	key_add(rl_map, "Move-Line", &render_lines_move_line);

	/* Make it easy to stop ignoring point */
	key_add(rl_map, "Abort", &render_lines_abort);

	key_add(rl_map, "Close", &render_lines_close);
	key_add(rl_map, "Close:mark", &render_lines_close_mark);
	key_add(rl_map, "Free", &edlib_do_free);
	key_add(rl_map, "Clone", &render_lines_clone);
	key_add(rl_map, "Refresh", &render_lines_refresh);
	key_add(rl_map, "Refresh:view", &render_lines_revise);
	key_add(rl_map, "Refresh:size", &render_lines_resize);
	key_add(rl_map, "Notify:clip", &render_lines_clip);
	key_add(rl_map, "get-attr", &render_lines_get_attr);
	key_add(rl_map, "point:moving", &render_lines_point_moving);

	key_add(rl_map, "doc:replaced", &render_lines_notify_replace);
	/* view:changed is sent to a tile when the display might need
	 * to change, even though the doc may not have*/
	key_add(rl_map, "view:changed", &render_lines_notify_replace);
	key_add(rl_map, "render:request:reposition", &render_send_reposition);
}

REDEF_CMD(render_lines_attach)
{
	struct rl_data *rl;
	struct pane *p;

	if (!rl_map)
		render_lines_register_map();

	alloc(rl, pane);
	rl->target_x = -1;
	rl->target_y = -1;
	rl->do_wrap = 1;
	p = ci->focus;
	if (strcmp(ci->key, "attach-render-text") == 0)
		p = call_ret(pane, "attach-markup", p);
	p = pane_register(p, 0, &render_lines_handle.c, rl);
	if (!p) {
		free(rl);
		return Efail;
	}
	rl->typenum = home_call(ci->focus, "doc:add-view", p) - 1;
	call("doc:request:doc:replaced", p);
	call("doc:request:point:moving", p);

	return comm_call(ci->comm2, "callback:attach", p);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &render_lines_attach, 0, NULL,
		  "attach-render-lines");
	call_comm("global-set-command", ed, &render_lines_attach, 0, NULL,
		  "attach-render-text");
}
