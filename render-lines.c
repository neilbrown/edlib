/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Rendering for any document which presents as a sequence of lines.
 * The underlying document must return lines of text in response to
 * the "render-line" command.
 * This takes a mark and moves it to the end of the rendered line
 * so that another call will produce another line.
 * "render-line" must always return a full line including '\n' unless the result
 * would be bigger than the 'max' passed in ->extra.  In that case it can stop
 * after 'max' and before a '\n'.
 * If ->numeric is >= 0, then rendering must only preceed for that many bytes
 * in the returned string.  It then returns with the mark only moved part way.
 * This allows a mark to be found for a given character position.
 * If ->numeric is -1, then rendering only continues until 'point' is reached.
 * This allows the cursor position to be determined.
 * For the standard 'render the whole line' functionality, ->numeric should
 * be NO_NUMERIC
 *
 * The document must also provide "render-line-prev" which moves mark to a
 * start-of-line.  If numeric is 0, then don't skip over any newlines.
 * If it is '1', then skip one newline.
 *
 * The returned line can contain attribute markings as <attr,attr>.
 * </> is used to pop most recent attributes.  << is used to include a literal '<'.
 * Lines generally contains UTF-8.  Control character '\n' is end of line and
 * '\t' tabs 1-8 spaces.  Other control characters should be rendered as
 * e.g. <fg:red>^X</> - in particular, nul must not appear in the line.
 *
 * We currently assume a constant-width font 1x1.
 *
 * We store all the marks found while rendering a pane in a 'view' on
 * the document.  The line returned for a given mark is attached to
 * extra space allocated for that mark.
 * When a change notification is received for a mark we discard that string.
 * So the string associated with a mark is certainly the string that would be rendered
 * after that mark (though it may be truncated).
 * The set of marks in a view should always identify exactly the set of lines
 * to be displayed.  Each mark should be at a start-of-line except possibly for the
 * first and last.  The first may be internal to a long line, but the line
 * rendering attached will always continue to the end-of-line.  We record the number
 * of display lines in that first line.
 * The last mark may also be mid-line, and it must never have an attached rendering.
 *
 * In the worst case of there being no newlines in the document, there will be precisely
 * two marks: one contains a partial line and one that marks the end of that line.
 * When point moves outside that range a new start will be chosen before point
 * using "render-line-prev' and the old start is discarded.
 *
 * To render the pane we:
 * 1/ call 'render-line-prev' on a mark at the point and look for that mark
 *    in the view.
 * 2/ If the mark matches and has a string, we have a starting point, else we
 *    call "render-line" and store the result, thus producing a starting point.
 *    We determine how many display lines are needed to display this text-line and
 *    set 'y' accordingly.
 *    At this point we have two marks: start and end, with known text of known
 *    height between.
 * 3/ Then we move outwards, back from the first mark and forward from the last mark.
 *    If we find a mark already in the view in the desired direction with texted
 *    attached it is correct and we use that.  Otherwise we find start (when going
 *    backwards) and render a new line.  Any old mark that is in the range is discarded.
 * 4/ When we have a full set of marks and the full height of the pane, we discard
 *    marks outside the range and start rendering from the top.
 *    ARG how is cursor drawn.
 *
 * If we already have correct marks on one side and not the other, we prefer
 * to advance on that one side.
 *
 * Sometimes we need to render without a point.  In this case we start at the first
 * mark in the view and move forward.  If we can we do this anyway, and only try
 * the slow way if the target point wasn't found.
 */

#include <unistd.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include <stdio.h> // sscanf

#define	MARK_DATA_PTR char
#include "core.h"
#include "misc.h"

struct rl_data {
	int		top_sol; /* true when first mark is at a start-of-line */
	int		ignore_point;
	int		skip_lines; /* Skip display-lines for first "line" */
	int		cursor_line; /* line that contains the cursor starts
				      * on this line */
	int		target_x, target_y;
	int		do_wrap;
	int		shift_left;
	int		prefix_len;
	int		header_lines;
	int		typenum;
	struct pane	*pane;
	int		line_height;
};

DEF_CMD(text_size_callback)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);
	cr->x = ci->x;
	cr->y = ci->y;
	cr->i = ci->numeric;
	cr->i2 = ci->extra;
	return 1;
}

#define WRAP 1
#define CURS 2

static int draw_some(struct pane *p, int *x, int y, char *start, char **endp,
		     char *attr, int margin, int cursorpos, int cursx, int scale)
{
	/* draw some text from 'start' for length 'len' into p[x,y].
	 * Update 'x' and 'startp' past what as drawn.
	 * Everything is drawn with the same attributes: attr.
	 * If the text would get closer to right end than 'margin',
	 * when stop drawing before then.  If this happens, WRAP is returned.
	 * If drawing would pass curx, then it stops before cursx and CURS is returned.
	 * If cursorpos is between 0 and len inclusive, a cursor is drawn there.
	 */
	int len = *endp - start;
	char *str = strndup(start, len);
	struct call_return cr = {0};
	int max;
	int ret = WRAP;
	int rmargin = p->w - margin;

	if (cursx >= 0 && cursx >= *x && cursx < rmargin) {
		rmargin = cursx;
		ret = CURS;
	}

	cr.c = text_size_callback;
	call_comm7("text-size", p, rmargin - *x, NULL, str, scale, attr, &cr.c);
	max = cr.i;
	if (max == 0 && ret == CURS) {
		/* must already have CURS position. */
		ret = WRAP;
		rmargin = p->w - margin;
		call_comm7("text-size", p, rmargin - *x, NULL, str, scale, attr, &cr.c);
		max = cr.i;
	}
	if (max < len) {
		str[max] = 0;
		call_comm7("text-size", p, rmargin - *x, NULL, str, scale, attr, &cr.c);
	}
	if (y >= 0) {
		if (cursorpos >= 0 && cursorpos <= len)
			call_xy7("text-display", p, cursorpos, scale, str, attr, *x, y, NULL, NULL);
		else
			call_xy7("text-display", p, -1, scale, str, attr, *x, y, NULL, NULL);
	}
	free(str);
	*x += cr.x;
	if (max >= len)
		return 0;
	/* Didn't draw everything. */
	*endp = start + max;
	return ret;
}

static void update_line_height_attr(struct pane *p, int *h, int *a, int *w, char *attr, char *str, int scale)
{
	struct call_return cr;
	cr.c = text_size_callback;
	call_comm7("text-size", p, -1, NULL, str, scale, attr, &cr.c);
	if (cr.y > *h)
		*h = cr.y;
	if (cr.i2 > *a)
		*a = cr.i2;
	if (w)
		*w += cr.x;
}

static void update_line_height(struct pane *p, int *h, int *a, int *w,
			       int *center, char *line, int scale)
{
	struct buf attr;
	int attr_found = 0;
	char *segstart = line;
	int above = 0, below = 0;

	buf_init(&attr);
	buf_append(&attr, ',');
	while (*line) {
		char c = *line++;
		char *st = line;
		if (c != '<' || *line == '<')
			continue;

		if (line - 1 > segstart) {
			char *l = strndup(segstart, line - 1 - segstart);
			update_line_height_attr(p, h, a, w, buf_final(&attr),
						l, scale);
			free(l);
		}
		while (*line && line[-1] != '>')
			line += 1;
		segstart = line;
		if (st[0] != '/') {
			char *c;
			char *b = buf_final(&attr);

			buf_concat_len(&attr, st, line-st);
			attr.b[attr.len-1] = ',';
			buf_append(&attr, ',');
			if (center && strstr(b, ",center,"))
				*center = 1;
			if (center && (c=strstr(b, ",left:")) != NULL)
				*center = atoi(c+6) * scale / 1000;
			if (center && (c=strstr(b, ",right:")) != NULL)
				*center = - atoi(c+7) * scale / 1000;
			if ((c=strstr(b, ",space-above:")) != NULL)
				above = atoi(c+13) * scale / 1000;
			if ((c=strstr(b, ",space-below:")) != NULL)
				below = atoi(c+13) * scale / 1000;
			attr_found = 1;
			update_line_height_attr(p, h, a, w, b, "", scale);
		} else {
			/* strip back to ",," */
			if (attr.len > 0)
				attr.len -= 2;
			while (attr.len > 0 &&
			       (attr.b[attr.len] != ',' ||
				attr.b[attr.len+1] != ','))
				attr.len -= 1;
		}
	}
	if (line[-1] == '\n')
		line -= 1;
	if (line > segstart || !attr_found) {
		char *l = strndup(segstart, line - segstart);
		update_line_height_attr(p, h, a, w, buf_final(&attr), l, scale);
		free(l);
	}
	*h += above + below;
	*a += above;
	free(buf_final(&attr));
}

/* render a line, with attributes and wrapping.  Report line offset where
 * cursor point cx,cy is passed. -1 if never seen.
 */
static void render_line(struct pane *p, char *line, int *yp, int dodraw, int scale,
		       int *cxp, int *cyp, int *offsetp)
{
	int x = 0;
	int y = *yp;
	char *line_start = line;
	char *start = line;
	struct buf attr;
	unsigned char ch;
	int cy = -1, cx = -1, offset = -1;
	struct rl_data *rl = p->data;
	int wrap = rl->do_wrap;
	char *prefix = pane_attr_get(p, "prefix");
	int line_height = -1;
	int ascent = -1;
	int mwidth = -1;
	int ret = 0;
	int twidth = 0;
	int center = 0;

	update_line_height(p, &line_height, &ascent, &twidth, &center, line, scale);

	if (prefix) {
		char *s = prefix + strlen(prefix);
		update_line_height_attr(p, &line_height, &ascent, NULL,
					"bold", prefix, scale);
		draw_some(p, &x, dodraw?y+ascent:-1, prefix, &s, "bold", 0, -1, -1, scale);
	}
	rl->prefix_len = x;
	if (center == 1)
		x += (p->w - x - twidth) / 2;
	if (center > 1)
		x += center;
	if (center < 0)
		x = p->w - x - twidth + center;

	rl->line_height = line_height;

	buf_init(&attr);
	buf_append(&attr, ' '); attr.len = 0;
	if (cxp && cyp && offsetp) {
		/* If cx and cy are non-negative, set *offsetp to
		 * the length when we reach that cursor pos.
		 * if offset is non-negative, set cx and cy to cursor
		 * pos when we reach that length
		 */
		cy = *cyp;
		cx = *cxp;
		offset = *offsetp;
	}
	if (cy >= 0 && cy < y) {
		/* cursor is not here */
		*offsetp = -1;
		cx = cy = -1;
	}

	if (!wrap)
		x -= rl->shift_left;

	while (*line && y < p->h) {
		int CX;
		int CP;

		if (mwidth < 0) {
			struct call_return cr;
			cr.c = text_size_callback;
			call_comm7("text-size", p, -1, NULL, "M", 0,
				   buf_final(&attr), &cr.c);
			mwidth = cr.x;
		}

		if (ret == WRAP) {
			char buf[2], *b;
			strcpy(buf, "\\");
			b = buf+1;
			x = p->w - mwidth;
			draw_some(p, &x, dodraw?y+ascent:-1, buf, &b, "underline,fg:blue",
				  0, -1, -1, scale);

			x = 0;
			y += line_height;
		}

		ch = *line;

		if (y+line_height < cy ||
		    (y <= cy && x <= cx) ||
		    ret == CURS)
			if (offsetp)
				/* haven't passed the cursor yet */
				*offsetp = start - line_start;
		if (ret == CURS) {
			/* Found the cursor, stop looking */
			cy = -1; cx = -1;
		}
		if (y+line_height >= cy &&
		    y <= cy && x <= cx)
			CX = cx;
		else
			CX = -1;
		if (offset < start - line_start)
			CP = -1;
		else
			CP = offset - (start - line_start);
		ret = 0;

		if (offset >= 0 && start - line_start <= offset) {
			*cyp = y;
			*cxp = x;
		}

		if (ch >= ' ' && ch != '<') {
			line += 1;
			/* only flush out if string is getting a bit long */
			if ((ch & 0xc0) == 0x80)
				/* In the middle of a UTF-8 */
				continue;
			if (offset == (line - line_start) ||
			    (line-start) * mwidth > p->w - x) {
				ret = draw_some(p, &x, dodraw?y+ascent:-1, start,
						&line,
						buf_final(&attr), mwidth, CP, CX, scale);
				start = line;
			}
			continue;
		}
		ret = draw_some(p, &x, dodraw?y+ascent:-1, start, &line,
				buf_final(&attr), mwidth, CP, CX, scale);
		start = line;
		if (ret)
			continue;
		if (ch == '<') {
			line += 1;
			if (*line == '<') {
				if (offset == start - line_start)
					offset += 1;
				start = line;
				line += 1;
			} else {
				char *a = line;

				while (*line && line[-1] != '>')
					line += 1;

				if (a[0] != '/') {
					buf_concat_len(&attr, a, line-a);
					/* mark location with ",," */
					attr.b[attr.len-1] = ',';
					buf_append(&attr, ',');
				} else {
					/* strip back to ",," */
					if (attr.len > 0)
						attr.len -= 2;
					while (attr.len >=2 &&
					       (attr.b[attr.len-1] != ',' ||
						attr.b[attr.len-2] != ','))
						attr.len -= 1;
					if (attr.len == 1)
						attr.len = 0;
				}
				if (offset == start - line_start)
					offset += line-start;
				start = line;
				mwidth = -1;
			}
			continue;
		}

		if (y+line_height < cy ||
		    (y <= cy && x <= cx) ||
		    ret == CURS)
			if (offsetp)
				/* haven't passed the cursor yet */
				*offsetp = start - line_start;

		line += 1;
		if (ch == '\n') {
			x = 0;
			y += line_height;
			start = line;
		} else if (ch == '\t') {
			int xc = x / mwidth;
			int w = 8 - xc % 8;
			x += w * mwidth;
			start = line;
		} else {
			char buf[4], *b;
			int l = attr.len;
			buf[0] = '^';
			buf[1] = ch + '@';
			buf[2] = 0;
			b = buf+2;
			buf_concat(&attr, ",underline,fg:red");
			ret = draw_some(p, &x, dodraw?y+ascent:-1, buf, &b,
					buf_final(&attr),
					mwidth*2, CP, CX, scale);
			attr.len = l;
			start = line;
		}
		continue;
	}
	if (!*line && (line > start || offset == start - line_start)) {
		/* Some more to draw */
		draw_some(p, &x, dodraw?y+ascent:-1, start, &line,
			  buf_final(&attr), mwidth, offset - (start - line_start), cx, scale);
	}
	if (y + line_height < cy ||
	    (y <= cy && x <= cx))
		/* haven't passed the cursor yet */
		if (offsetp)
			*offsetp = line - line_start;
	if (offset >= 0 && line - line_start <= offset) {
		*cyp = y;
		*cxp = x;
	}
	if (x > 0)
		/* No newline at the end .. but we must render as whole lines */
		y += line_height;
	*yp = y;
	free(buf_final(&attr));
}

static struct mark *call_render_line_prev(struct pane *p,
					  struct mark *m, int n, int *found)
{
	struct cmd_info ci = {0};
	int ret;

	ci.key = "render-line-prev";
	ci.mark = m;
	ci.focus = p;
	ci.numeric = n;
	ret = key_handle(&ci);
	if (ret == 0) {
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

	m = vmark_matching(p, ci.mark);
	if (m)
		mark_free(ci.mark);
	else
		m = ci.mark;
	return m;
}

DEF_CMD(save_str)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);
	cr->s = ci->str ? strdup(ci->str) : NULL;
	return 1;
}

static struct mark *call_render_line(struct pane *p, struct mark *start)
{
	struct cmd_info ci = {0};
	struct call_return cr;
	struct mark *m, *m2;

	ci.key = "render-line";
	ci.focus = p;
	ci.mark = mark_dup(start, 0);
	ci.numeric = NO_NUMERIC;
	cr.c = save_str;
	cr.s = NULL;
	ci.comm2 = &cr.c;
	/* Allow for filling the rest of the pane, given that
	 * some has been used.
	 * 'used' can be negative if the mark is before the start
	 * of the pane
	 */
	if (key_handle(&ci) == 0) {
		mark_free(ci.mark);
		return NULL;
	}

	if (start->mdata)
		free(start->mdata);
	start->mdata = cr.s;

	m = vmark_matching(p, ci.mark);
	if (m)
		mark_free(ci.mark);
	else
		m = ci.mark;
	/* Any mark between start and m must be discarded,
	 */
	while ((m2 = vmark_next(start)) != NULL &&
	       mark_ordered(m2, m)) {
			free(m2->mdata);
			m2->mdata = NULL;
			mark_free(m2);
	}

	return m;
}

DEF_CMD(no_save)
{
	return 1;
}

static struct mark *call_render_line_offset(struct pane *p,
					    struct mark *start, int offset)
{
	struct cmd_info ci = {0};

	ci.key = "render-line";
	ci.focus = p;
	ci.mark = mark_dup(start, 0);
	ci.numeric = offset;
	ci.comm2 = &no_save;
	if (key_handle(&ci) == 0) {
		mark_free(ci.mark);
		return NULL;
	}
	return ci.mark;
}

DEF_CMD(get_len)
{
	if (ci->str)
		return strlen(ci->str) + 1;
	else
		return 1;
}

static int call_render_line_to_point(struct pane *p, struct mark *pm,
				     struct mark *start)
{
	struct cmd_info ci = {0};
	int len;

	ci.key = "render-line";
	ci.focus = p;
	ci.mark2 = pm;
	ci.mark = mark_dup(start, 0);
	ci.numeric = -1;
	ci.comm2 = &get_len;
	len = key_handle(&ci);
	if (len <= 0) {
		mark_free(ci.mark);
		return 0;
	}
	len -= 1;
	mark_free(ci.mark);
	return len;
}

static int get_scale(struct pane *p)
{
	char *sc = pane_attr_get(p, "scale");
	int x, y;
	int xscale, yscale, scale;

	if (!sc)
		return 1000;

	if (sscanf(sc, "x:%d,y:%d", &x, &y) != 2) {
		scale = atoi(sc);
		if (scale > 3)
			return scale;
		return 1000;
	}

	/* 'scale' is pixels per point times 1000.
	 * ":scale:x" is points across pane, so scale = p->w/x*1000
	 */
	xscale = 1000 * p->w / x;
	yscale = 1000 * p->h / y;
	scale = (xscale < yscale) ? xscale: yscale;
	if (scale < 10)
		scale = 1000;
	return scale;
}

static void find_lines(struct mark *pm, struct pane *p)
{
	struct rl_data *rl = p->data;
	struct mark *top, *bot;
	struct mark *m;
	struct mark *start, *end;
	int y = 0;
	int offset;
	int found_start = 0, found_end = 0;
	int lines_above = 0, lines_below = 0;
	int scale = get_scale(p);

	if (pm->viewnum != MARK_POINT)
		return;

	top = vmark_first(p, rl->typenum);
	bot = vmark_last(p, rl->typenum);
	m = call_render_line_prev(p, mark_at_point(p, pm, rl->typenum),
				  0, &rl->top_sol);
	if (!m)
		return;
	start = m;
	offset = call_render_line_to_point(p, pm, start);
	if (start->mdata == NULL)
		m = call_render_line(p, start);
	else
		m = vmark_next(start);

	end = m;
	if (start->mdata) {
		int x;
		x = -1; lines_above = -1; y = 0;
		render_line(p, start->mdata, &y, 0, scale, &x, &lines_above, &offset);
		lines_below = y - lines_above;
	}
	y = 1;
	/* We have start/end of the focus line, and its height */
	if (bot && !mark_ordered_or_same_pane(p, bot, start))
		/* already before 'bot', so will never "cross over" bot, so
		 * ignore 'bot'
		 */
		bot = NULL;
	if (top && !mark_ordered_or_same_pane(p, end, top))
		top = NULL;

	rl->skip_lines = 0;
	while (!((found_start && found_end) || y >= p->h - rl->header_lines)) {
		if (!found_start) {
			/* step backwards moving start */
			if (lines_above > 0) {
				lines_above -= 1;
				y += 1;
			} else {
				m = call_render_line_prev(p, mark_dup(start, 0),
							  1, &rl->top_sol);
				if (!m) {
					/* no text before 'start' */
					found_start = 1;
				} else {
					int h = 0;
					start = m;
					if (!start->mdata)
						call_render_line(p, start);
					render_line(p, start->mdata, &h, 0, scale,
						    NULL, NULL, NULL);
					if (h) {
						lines_above = h - 1;
						y += 1;
					} else
						found_start = 1;
				}
				if (bot && mark_ordered(start, bot))
					found_end = 1;
			}
		}
		if (!found_end) {
			/* step forwards */
			if (lines_below > 0) {
				lines_below -= 1;
				y += 1;
			} else {
				if (!end->mdata)
					call_render_line(p, end);
				if (!end->mdata)
					found_end = 1;
				else {
					int h = 0;
					render_line(p, end->mdata, &h, 0, scale,
						    NULL, NULL, NULL);
					end = vmark_next(end);
					ASSERT(end != NULL);
					if (h) {
						lines_below = h - 1;
						y += 1;
					} else
						found_end = 1;
				}
				if (top && mark_ordered(top, end))
					found_start = 1;
			}
		}
	}
	rl->skip_lines = lines_above;
	/* Now discard any marks outside start-end */
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
}

static void render(struct mark *pm, struct pane *p)
{
	struct rl_data *rl = p->data;
	int y;
	struct mark *m, *m2;
	int restarted = 0;
	char *hdr;
	int scale = get_scale(p);

	hdr = pane_attr_get(p, "heading");
	if (hdr && !*hdr)
		hdr = NULL;

restart:
	pane_clear(p, NULL);
	y = 0;
	if (hdr) {
		rl->header_lines = 0;
		render_line(p, hdr, &y, 1, scale, NULL, NULL, NULL);
		rl->header_lines = y;
	}
	y -= rl->skip_lines;
	m = vmark_first(p, rl->typenum);

	p->cx = p->cy = -1;
	rl->cursor_line = 0;

	while (m && y < p->h) {
		if (m->mdata == NULL) {
			/* This line has changed. */
			call_render_line(p, m);
		}
		m2 = vmark_next(m);
		if (p->cx <= 0 &&
		    mark_ordered_or_same_pane(p, m, pm) &&
		    (!m2 || mark_ordered_or_same_pane(p, pm, m2))) {
			int len = call_render_line_to_point(p, pm,
							    m);
			rl->cursor_line = y;
			render_line(p, m->mdata ?: "", &y, 1, scale, &p->cx, &p->cy, &len);
			if (p->cy < 0)
				p->cx = -1;
			if (!rl->do_wrap && p->cy >= 0 && p->cx < rl->prefix_len) {
				/* Need to shift to right */
				while (rl->shift_left > 0 && p->cx < rl->prefix_len) {
					if (rl->shift_left < 8) {
						p->cx += rl->shift_left;
						rl->shift_left = 0;
					} else {
						p->cx += 8;
						rl->shift_left -= 8;
					}
				}
				if (!restarted) {
					restarted = 1;
					goto restart;
				}
			}
			if (p->cx >= p->w && !rl->do_wrap) {
				/* Need to shift to the left */
				while (p->cx >= p->w) {
					rl->shift_left += 8;
					p->cx -= 8;
				}
				if (!restarted) {
					restarted = 1;
					goto restart;
				}
			}
		} else
			render_line(p, m->mdata?:"", &y, 1, scale, NULL, NULL, NULL);
		if (!m2)
			break;
		m = m2;
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
	return;
}

DEF_CMD(render_lines_refresh)
{
	struct pane *p = ci->home;
	struct rl_data *rl = p->data;
	struct mark *m;
	char *a;

	pane_check_size(p);

	a = pane_attr_get(p, "render-wrap");
	rl->do_wrap = (!a || strcmp(a, "yes") == 0);

	m = vmark_first(p, rl->typenum);
	if (rl->top_sol && m)
		m = call_render_line_prev(p, mark_dup(m, 0), 0,
					  &rl->top_sol);

	if (m) {
		render(ci->mark, p);
		if (rl->ignore_point || (p->cx >= 0 && p->cy < p->h))
			/* Found the cursor! */
			return 1;
	}
	find_lines(ci->mark, p);
	render(ci->mark, p);
	return 1;
}

DEF_CMD(render_lines_close)
{
	struct pane *p = ci->home;
	struct rl_data *rl = p->data;
	struct mark *m;

	while ((m = vmark_first(p, rl->typenum)) != NULL) {
		free(m->mdata);
		m->mdata = NULL;
		mark_free(m);
	}

	rl->pane = NULL;
	doc_del_view(p, rl->typenum);
	p->data = NULL;
	p->handle = NULL;
	free(rl);
	return 0;
}

DEF_CMD(render_lines_other_move)
{
	struct pane *p = ci->home;
	struct rl_data *rl = p->data;

	if (rl->ignore_point) {
		pane_damaged(p, DAMAGED_CONTENT);
		rl->ignore_point = 0;
	}
	rl->target_x = -1;

	/* Allow other handlers to complete the Replace */
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
	int rpt = RPT_NUM(ci);
	struct rl_data *rl = p->data;
	struct mark *top;
	int pagesize = rl->line_height;
	int scale = get_scale(p);

	top = vmark_first(p, rl->typenum);
	if (!top)
		return 0;
	if (strcmp(ci->key, "Move-View-Large") == 0)
		pagesize = p->h - 2 * rl->line_height;
	rpt *= pagesize;

	rl->ignore_point = 1;

	if (rpt < 0) {
		while (rpt < 0) {
			int y = 0;

			if (rl->skip_lines) {
				rl->skip_lines -= 1;
				rpt += 1;
				continue;
			}

			top = call_render_line_prev(p, mark_dup(top, 0),
						    1, &rl->top_sol);
			if (!top)
				break;
			if (top->mdata == NULL)
				call_render_line(p, top);
			if (top->mdata == NULL)
				break;
			render_line(p, top->mdata, &y, 0, scale, NULL, NULL, NULL);
			rl->skip_lines = y;
		}
	} else {
		while (top && rpt > 0) {
			int y = 0;
			struct mark *old;

			if (top->mdata == NULL)
				call_render_line(p, top);
			if (top->mdata == NULL)
				break;
			render_line(p, top->mdata, &y, 0, scale, NULL, NULL, NULL);
			if (rl->skip_lines + rpt < y) {
				rl->skip_lines += rpt;
				break;
			}
			top = vmark_next(top);
			if ((rpt+pagesize-rl->line_height)/pagesize !=
			    (rpt+pagesize-y-rl->line_height)/pagesize)
				/* Have crossed a full page, can discard old lines */
				while ((old = vmark_first(p, rl->typenum)) != NULL &&
				       old != top) {
					free(old->mdata);
					old->mdata = NULL;
					mark_free(old);
				}
			rpt -= y - rl->skip_lines;
			rl->skip_lines = 0;
		}
	}
	pane_damaged(p, DAMAGED_CONTENT);
	return 1;
}

DEF_CMD(render_lines_set_cursor)
{
	struct pane *p = ci->home;
	struct rl_data *rl = p->data;
	struct mark *m;
	int y = rl->header_lines - rl->skip_lines;
	int found = 0;
	int cihx, cihy;
	int scale = get_scale(p);

	render_lines_other_move_func(ci);

	m = vmark_first(p, rl->typenum);

	cihx = ci->x; cihy = ci->y;
	pane_map_xy(ci->focus, ci->home, &cihx, &cihy);

	if (y > cihy)
		/* x,y is in header line - try lower */
		cihy = y;
	while (y <= cihy && m && m->mdata) {
		int cx = cihx, cy = cihy, o = -1;
		render_line(p, m->mdata, &y, 0, scale, &cx, &cy, &o);
		if (o >= 0) {
			struct mark *m2 = call_render_line_offset(p, m, o);
			if (m2) {
				mark_to_mark(ci->mark, m2);
				mark_free(m2);
				found = 1;
			}
		} else if (found)
			break;
		m = vmark_next(m);
	}

	pane_focus(p);
	return 1;
}

DEF_CMD(render_lines_move_pos)
{
	struct pane *p = ci->home;
	struct rl_data *rl = p->data;
	struct mark *pm = ci->mark;
	struct mark *top, *bot;

	rl->ignore_point = 1;
	top = vmark_first(p, rl->typenum);
	bot = vmark_last(p, rl->typenum);
	if (top && bot &&
	    mark_ordered(top, pm) &&
	    mark_ordered(pm, bot))
		/* pos already displayed */
		return 1;
	find_lines(pm, ci->home);
	pane_damaged(p, DAMAGED_CONTENT);
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
	struct rl_data *rl = p->data;
	struct cmd_info ci2 = {0};
	int target_x, target_y;
	int o = -1;
	int scale = get_scale(p);

	rl->ignore_point = 0;

	/* save target as it might get changed */
	target_x = rl->target_x;
	target_y = rl->target_y;
	if (target_x < 0) {
		target_x = p->cx;
		target_y = p->cy - rl->cursor_line;
	}
	ci2.focus = ci->focus;
	ci2.key = "Move-EOL";
	ci2.numeric = RPT_NUM(ci);
	if (ci2.numeric < 0)
		ci2.numeric -= 1;
	else
		ci2.numeric += 1;
	ci2.mark = ci->mark;
	if (!key_handle(&ci2))
		return -1;
	if (RPT_NUM(ci) > 0) {
		/* at end of target line, move to start */
		ci2.numeric = -1;
		if (!key_handle(&ci2))
			return -1;
	}

	/* restore target: Move-EOL might have changed it */
	rl->target_x = target_x;
	rl->target_y = target_y;

	if (target_x >= 0 || target_y >= 0) {
		struct mark *start =
			vmark_at_point(p, rl->typenum);
		int y = 0;
		if (!start || !start->mdata) {
			pane_damaged(p, DAMAGED_CONTENT);
			return 1;
		}
		render_line(p, start->mdata, &y, 0, scale, &target_x, &target_y, &o);
		/* 'o' is the distance from start-of-line of the target */
		if (o >= 0) {
			struct mark *m2 = call_render_line_offset(
				p, start, o);
			if (m2)
				mark_to_mark(ci->mark, m2);
			mark_free(m2);
		}
	}
	return 1;
}

DEF_CMD(render_lines_notify_replace)
{
	struct rl_data *rl = ci->home->data;
	struct mark *start = ci->mark2;
	struct mark *end, *t;
	struct pane *p = ci->home;

	if (!start)
		start = ci->mark;
	if (!ci->mark)
		return 1;

	end = vmark_at_or_before(ci->home, ci->mark, rl->typenum);

	if (!end)
		/* Change before visible region */
		return 1;

	t = end;
	while (t && mark_ordered_or_same_pane(p, start, t)) {
		struct mark *t2;
		if (t->mdata) {
			free(t->mdata);
			t->mdata = NULL;
		}
		t2 = doc_prev_mark_view(t);
		if (t2 && mark_same_pane(p, t, t2, NULL))
			/* Marks must be distinct! */
			mark_free(t);
		t = t2;
	}
	if (t && t->mdata) {
		free(t->mdata);
		t->mdata = NULL;
	}
	pane_damaged(rl->pane, DAMAGED_CONTENT);

	return 1;
}


DEF_CMD(render_lines_attach);
DEF_CMD(render_lines_clone)
{
	struct pane *parent = ci->focus;
	struct pane *p = ci->home, *c;

	render_lines_attach.func(ci);
	c = pane_child(p);
	if (c)
		return pane_clone(c, parent->focus);
	return 1;
}

DEF_CMD(render_lines_redraw)
{
	struct pane *p = ci->home;
	struct rl_data *rl = p->data;
	struct mark *m;

	for (m = vmark_first(p, rl->typenum);
	     m;
	     m = vmark_next(m)) {
		free(m->mdata);
		m->mdata = NULL;
	}
	return 1;
}

static struct map *rl_map;

DEF_LOOKUP_CMD(render_lines_handle, rl_map)

static void render_lines_register_map(void)
{
	rl_map = key_alloc();

	key_add_range(rl_map, "Move-", "Move-\377", &render_lines_other_move);
	key_add(rl_map, "Move-View-Small", &render_lines_move);
	key_add(rl_map, "Move-View-Large", &render_lines_move);
	key_add(rl_map, "Move-View-Pos", &render_lines_move_pos);
	key_add(rl_map, "Move-CursorXY", &render_lines_set_cursor);
	key_add(rl_map, "Click-1", &render_lines_set_cursor);
	key_add(rl_map, "Press-1", &render_lines_set_cursor);
	key_add(rl_map, "Move-Line", &render_lines_move_line);

	key_add(rl_map, "Replace", &render_lines_other_move);
	/* Make it easy to stop ignoring point */
	key_add(rl_map, "Abort", &render_lines_other_move);

	key_add(rl_map, "Close", &render_lines_close);
	key_add(rl_map, "Clone", &render_lines_clone);
	key_add(rl_map, "Refresh", &render_lines_refresh);

	/* force full refresh */
	key_add(rl_map, "render-lines:redraw", &render_lines_redraw);

	key_add(rl_map, "Notify:Replace", &render_lines_notify_replace);
}

REDEF_CMD(render_lines_attach)
{
	struct rl_data *rl = malloc(sizeof(*rl));

	if (!rl_map)
		render_lines_register_map();

	rl->ignore_point = 0;
	rl->top_sol = 0;
	rl->skip_lines = 0;
	rl->target_x = -1;
	rl->target_y = -1;
	rl->cursor_line = 0;
	rl->do_wrap = 1;
	rl->shift_left = 0;
	rl->header_lines = 0;
	rl->typenum = doc_add_view(ci->focus, NULL);
	rl->pane = pane_register(ci->focus, 0, &render_lines_handle.c, rl, NULL);
	call3("Request:Notify:Replace", rl->pane, 0, NULL);

	return comm_call(ci->comm2, "callback:attach", rl->pane,
			 0, NULL, NULL, 0);
}

void edlib_init(struct editor *ed)
{
	key_add(ed->commands, "render-lines-attach", &render_lines_attach);
}
