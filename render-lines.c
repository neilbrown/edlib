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
	struct call_return cr = {};
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
			char *c2;
			char *b = buf_final(&attr);

			buf_concat_len(&attr, st, line-st);
			attr.b[attr.len-1] = ',';
			buf_append(&attr, ',');
			if (center && strstr(b, ",center,"))
				*center = 1;
			if (center && (c2=strstr(b, ",left:")) != NULL)
				*center = atoi(c2+6) * scale / 1000;
			if (center && (c2=strstr(b, ",right:")) != NULL)
				*center = - atoi(c2+7) * scale / 1000;
			if ((c2=strstr(b, ",space-above:")) != NULL)
				above = atoi(c2+13) * scale / 1000;
			if ((c2=strstr(b, ",space-below:")) != NULL)
				below = atoi(c2+13) * scale / 1000;
			if ((c2=strstr(b, ",tab:")) != NULL)
				*w = atoi(c2+5) * scale / 1000;
			attr_found = 1;
			update_line_height_attr(p, h, a, w, b, "", scale);
		} else {
			/* strip back to ",," */
			if (attr.len >= 2)
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

static void render_image(struct pane *p, char *line, int *yp,
			 int dodraw, int scale)
{
	char *fname = NULL;
	int width = p->w/2, height = p->h/2;

	while (*line == '<')
		line += 1;

	while (*line && *line != '>') {
		int len = strcspn(line, ",>");

		if (strncmp(line, "image:", 6) == 0) {
			char *cp = line + 6;
			fname = strndup(cp, len-6);
		} else if (strncmp(line, "width:", 6) == 0) {
			width = atoi(line + 6);
			width = width * scale / 1000;
		} else if (strncmp(line, "height:", 7) == 0) {
			height = atoi(line + 7);
			height = height * scale / 1000;
		}
		line += len;
		line += strspn(line, ",");
	}
	if (fname && dodraw) {
		struct pane *tmp = pane_register(p, 0, NULL, NULL, NULL);

		pane_resize(tmp, (p->w - width)/2, *yp, width, height);
		call5("image-display", tmp, 0, NULL, fname, 5);
		pane_close(tmp);
	}
	*yp += height;
	free(fname);
}

/* render a line, with attributes and wrapping.  Report line offset where
 * cursor point cx,cy is passed. -1 if never seen.
 */
static void render_line(struct pane *p, struct pane *focus,
			char *line, int *yp, int dodraw, int scale,
			int *cxp, int *cyp, int *offsetp, int *end_of_page)
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
	char *prefix = pane_attr_get(focus, "prefix");
	int line_height = -1;
	int ascent = -1;
	int mwidth = -1;
	int ret = 0;
	int twidth = 0;
	int center = 0;
	int margin;

	if (strncmp(line, "<image:",7) == 0) {
		/* For now an <image> must be on a line by itself.
		 * Maybe this can be changed later if I decide on
		 * something that makes sense.
		 * The cursor is not on the image.
		 */
		render_image(p, line, yp, dodraw, scale);
		if (cxp)
			*cxp = -1;
		if (cyp)
			*cyp = -1;
		if (offsetp)
			*offsetp = -1;
		return;
	}

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
	margin = x;

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

	while (*line && y < p->h && (!end_of_page || !*end_of_page)) {
		int CX;
		int CP;

		if (mwidth < 0) {
			struct call_return cr;
			cr.c = text_size_callback;
			call_comm7("text-size", p, -1, NULL, "M", 0,
				   buf_final(&attr), &cr.c);
			mwidth = cr.x;
		}

		if (ret == WRAP && wrap) {
			char buf[2], *b;
			strcpy(buf, "\\");
			b = buf+1;
			x = p->w - mwidth;
			draw_some(p, &x, dodraw?y+ascent:-1, buf, &b, "underline,fg:blue",
				  0, -1, -1, scale);

			x = 0;
			y += line_height;
		}
		if (ret == WRAP && !wrap) {
			while (*line && *line != '\n' && *line != '<')
				line += 1;
			start = line;
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
						buf_final(&attr),
						wrap ? mwidth : 0,
						CP, CX, scale);
				start = line;
			}
			continue;
		}
		ret = draw_some(p, &x, dodraw?y+ascent:-1, start, &line,
				buf_final(&attr),
				wrap ? mwidth : 0,
				CP, CX, scale);
		if (!wrap && ret == WRAP && line == start)
			ret = 0;
		start = line;
		if (ret)
			continue;
		if (ch == '<') {
			line += 1;
			if (*line == '<') {
				if (offset >= 0 && offset == start - line_start)
					offset += 1;
				start = line;
				line += 1;
			} else {
				char *a = line;

				while (*line && line[-1] != '>')
					line += 1;

				if (a[0] != '/') {
					int ln = attr.len;
					char *tb;

					buf_concat_len(&attr, a, line-a);
					/* mark location with ",," */
					attr.b[attr.len-1] = ',';
					buf_append(&attr, ',');
					tb = strstr(buf_final(&attr)+ln,
						    "tab:");
					if (tb)
						x = margin + atoi(tb+4) * scale / 1000;
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
		} else if (ch == '\f') {
			x = 0;
			start = line;
			if (end_of_page)
				*end_of_page = 1;
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
					wrap ? mwidth*2: 0, CP, CX, scale);
			attr.len = l;
			start = line;
		}
		continue;
	}
	if (!*line && (line > start || offset == start - line_start)) {
		/* Some more to draw */
		draw_some(p, &x, dodraw?y+ascent:-1, start, &line,
			  buf_final(&attr),
			  wrap ? mwidth : 0, offset - (start - line_start), cx, scale);
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
	int ret;
	struct mark *m2;

	ret = call3("render-line-prev", p, n, m);
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

	m2 = vmark_matching(p, m);
	if (m2)
		mark_free(m);
	else
		m2 = m;
	return m2;
}

DEF_CMD(save_str)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);
	cr->s = ci->str ? strdup(ci->str) : NULL;
	return 1;
}

static struct mark *call_render_line(struct pane *p, struct mark *start)
{
	struct cmd_info ci = {};
	struct call_return cr;
	struct mark *m, *m2;

	m = mark_dup(start, 0);
	cr.c = save_str;
	cr.s = NULL;
	/* Allow for filling the rest of the pane, given that
	 * some has been used.
	 * 'used' can be negative if the mark is before the start
	 * of the pane
	 */
	if (call_comm("render-line", p, NO_NUMERIC,
		      m, NULL, 0, &cr.c) == 0) {
		mark_free(ci.mark);
		return NULL;
	}

	if (start->mdata)
		free(start->mdata);
	start->mdata = cr.s;

	m2 = vmark_matching(p, m);
	if (m2)
		mark_free(m);
	else
		m2 = m;
	/* Any mark between start and m2 must be discarded,
	 */
	while ((m = vmark_next(start)) != NULL &&
	       mark_ordered(m, m2)) {
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

static struct mark *call_render_line_offset(struct pane *p,
					    struct mark *start, int offset)
{
	struct mark *m;

	m = mark_dup(start, 0);
	if (call_comm("render-line", pane_final_child(p), offset, m,
		      NULL, 0, &no_save) == 0) {
		mark_free(m);
		return NULL;
	}
	return m;
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
	struct cmd_info ci = {};
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

static void find_lines(struct mark *pm, struct pane *p, struct pane *focus)
{
	struct rl_data *rl = p->data;
	struct mark *top, *bot;
	struct mark *m;
	struct mark *start, *end;
	int x;
	int y = 0;
	int offset;
	int found_start = 0, found_end = 0;
	int lines_above = 0, lines_below = 0;
	int scale = get_scale(focus);

	top = vmark_first(focus, rl->typenum);
	bot = vmark_last(focus, rl->typenum);
	m = call_render_line_prev(focus, mark_at_point(focus, pm, rl->typenum),
				  0, &rl->top_sol);
	if (!m)
		return;
	start = m;
	offset = call_render_line_to_point(focus, pm, start);
	if (start->mdata == NULL)
		m = call_render_line(focus, start);
	else
		m = vmark_next(start);

	end = m;
	x = -1; lines_above = -1; y = 0;
	render_line(p, focus, start->mdata ?: "", &y, 0, scale,
		    &x, &lines_above, &offset, &found_end);
	lines_above = lines_below = 0;

	/* We have start/end of the focus line, and its height.
	 * Rendering just that "line" uses a height of 'y', of which
	 * 'lines_above' is above the cursor, and 'lines_below' is below.
	 */
	if (bot && !mark_ordered_or_same_pane(focus, bot, start))
		/* already before 'bot', so will never "cross over" bot, so
		 * ignore 'bot'
		 */
		bot = NULL;
	if (top && !mark_ordered_or_same_pane(focus, end, top))
		top = NULL;

	while ((!found_start || !found_end) && y < p->h - rl->header_lines) {
		if (!found_start && lines_above == 0) {
			/* step backwards moving start */
			m = call_render_line_prev(focus, mark_dup(start, 0),
						  1, &rl->top_sol);
			if (!m) {
				/* no text before 'start' */
				found_start = 1;
			} else {
				int h = 0;
				start = m;
				if (!start->mdata)
					call_render_line(focus, start);
				if (start->mdata)
					render_line(p, focus, start->mdata, &h, 0, scale,
						    NULL, NULL, NULL, &found_end);
				if (h)
					lines_above = h;
				else
					found_start = 1;
			}
			if (bot && mark_ordered(start, bot))
				found_end = 1;
		}
		if (!found_end && lines_below == 0) {
			/* step forwards */
			struct mark *next;
			if (!end->mdata)
				call_render_line(focus, end);
			next = vmark_next(end);
			if (!end->mdata || !next)
				found_end = 1;
			else {
				int h = 0;
				render_line(p, focus, end->mdata, &h, 0, scale,
					    NULL, NULL, NULL, &found_end);
				end = next;
				if (h)
					lines_below = h;
				else
					found_end = 1;
			}
			if (top && mark_ordered(top, end))
				found_start = 1;
		}
		if (lines_above > 0 && lines_below > 0) {
			int consume = (lines_above > lines_below ? lines_below : lines_above) * 2;
			if (consume > (p->h - rl->header_lines) - y)
				consume = (p->h - rl->header_lines) - y;
			if (lines_above > lines_below) {
				lines_above -= consume - (consume/2);
				lines_below -= consume/2;
			} else {
				lines_below -= consume - (consume/2);
				lines_above -= consume/2;
			}
			y += consume;
			/* We have just consumed all of one of lines_{above,below}
			 * so they are no longer both > 0 */
		}
		if (found_end && lines_above) {
			int consume = p->h - rl->header_lines - y;
			if (consume > lines_above)
				consume = lines_above;
			lines_above -= consume;
			y += consume;
		}
		if (found_start && lines_below) {
			int consume = p->h - rl->header_lines - y;
			if (consume > lines_below)
				consume = lines_below;
			lines_below -= consume;
			y += consume;
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
}

static void render(struct mark *pm, struct pane *p, struct pane *focus)
{
	struct rl_data *rl = p->data;
	int y;
	struct mark *m, *m2;
	int restarted = 0;
	char *hdr;
	int scale = get_scale(focus);
	char *s;
	int hide_cursor = 0;
	int found_end;

	hdr = pane_attr_get(focus, "heading");
	if (hdr && !*hdr)
		hdr = NULL;
	s = pane_attr_get(focus, "hide-cursor");
	if (s && strcmp(s, "yes") == 0)
		hide_cursor = 1;
	s = pane_attr_get(focus, "background");

restart:
	found_end = 0;
	m = vmark_first(focus, rl->typenum);
	if (!s)
		pane_clear(p, NULL);
	else if (strncmp(s, "color:", 6) == 0) {
		char *a = strdup(s);
		strcpy(a, "bg:");
		strcpy(a+3, a+6);
		pane_clear(p, a);
		free(a);
	} else if (strncmp(s, "image:", 6) == 0) {
		if (call5("image-display", focus, 1, NULL, s+6, 0) <= 0)
			pane_clear(p, NULL);
	} else if (strncmp(s, "call:", 5) == 0) {
		if (call_home(focus, s+5, p, 0, m, NULL) <= 0)
			pane_clear(p, NULL);
	} else
		pane_clear(p, NULL);

	y = 0;
	if (hdr) {
		rl->header_lines = 0;
		render_line(p, focus, hdr, &y, 1, scale, NULL, NULL, NULL, NULL);
		rl->header_lines = y;
	}
	y -= rl->skip_lines;

	p->cx = p->cy = -1;
	rl->cursor_line = 0;

	while (m && y < p->h && !found_end) {
		if (m->mdata == NULL) {
			/* This line has changed. */
			call_render_line(focus, m);
		}
		m2 = vmark_next(m);
		if (!hide_cursor && p->cx <= 0 &&
		    mark_ordered_or_same_pane(focus, m, pm) &&
		    (!m2 || mark_ordered_or_same_pane(focus, pm, m2))) {
			int len = call_render_line_to_point(focus, pm,
							    m);
			rl->cursor_line = y;
			render_line(p, focus, m->mdata ?: "", &y, 1, scale,
				    &p->cx, &p->cy, &len, NULL);
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
			render_line(p, focus, m->mdata?:"", &y, 1, scale, NULL, NULL, NULL,
				    &found_end);
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
	struct pane *focus = pane_final_child(p);
	struct rl_data *rl = p->data;
	struct mark *m;
	char *a;

	pane_check_size(p);

	a = pane_attr_get(focus, "render-wrap");
	rl->do_wrap = (!a || strcmp(a, "yes") == 0);

	m = vmark_first(focus, rl->typenum);
	if (rl->top_sol && m)
		m = call_render_line_prev(focus, mark_dup(m, 0), 0,
					  &rl->top_sol);

	if (m) {
		render(ci->mark, p, focus);
		if (rl->ignore_point || (p->cx >= 0 && p->cy < p->h))
			/* Found the cursor! */
			return 1;
	}
	find_lines(ci->mark, p, focus);
	render(ci->mark, p, focus);
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

	pane_damaged(p, DAMAGED_CURSOR);

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
	struct pane *focus = pane_final_child(p);
	int rpt = RPT_NUM(ci);
	struct rl_data *rl = p->data;
	struct mark *top;
	int pagesize = rl->line_height;
	int scale = get_scale(focus);

	top = vmark_first(focus, rl->typenum);
	if (!top)
		return 0;
	if (strcmp(ci->key, "Move-View-Large") == 0)
		pagesize = p->h - 2 * rl->line_height;
	rpt *= pagesize;

	rl->ignore_point = 1;

	if (rpt < 0) {
		while (rpt < 0) {
			int y = 0;
			struct mark *m;
			struct mark *prevtop = top;

			if (rl->skip_lines) {
				rl->skip_lines -= 1;
				rpt += 1;
				continue;
			}

			m = mark_dup(top, 0);
			top = call_render_line_prev(focus, m,
						    1, &rl->top_sol);
			if (!top && doc_prior_pane(focus, prevtop) != WEOF) {
				/* Double check - maybe a soft top-of-file */
				m = mark_dup(prevtop, 0);
				mark_prev_pane(focus, m);
				top = call_render_line_prev(focus, m,
							    1, &rl->top_sol);
			}
			if (!top)
				break;
			m = top;
			while (m->seq < prevtop->seq &&
			       !mark_same_pane(focus, m, prevtop, NULL)) {
				if (m->mdata == NULL)
					call_render_line(focus, m);
				if (m->mdata == NULL) {
					rpt = 0;
					break;
				}
				render_line(p, focus, m->mdata, &y, 0, scale, NULL, NULL, NULL, NULL);
				m = vmark_next(m);
			}
			rl->skip_lines = y;
		}
	} else {
		while (top && rpt > 0) {
			int y = 0;
			int page_end = 0;

			if (top->mdata == NULL)
				call_render_line(focus, top);
			if (top->mdata == NULL)
				break;
			render_line(p, focus, top->mdata, &y, 0, scale, NULL, NULL, NULL, &page_end);
			if (page_end)
				y = rpt % pagesize;
			if (rl->skip_lines + rpt < y) {
				rl->skip_lines += rpt;
				break;
			}
			top = vmark_next(top);
			if (top->mdata == NULL)
				call_render_line(focus, top);
			rpt -= y - rl->skip_lines;
			rl->skip_lines = 0;
		}
		if (top && top->mdata) {
			/* We didn't fall off the end, so it is OK to remove
			 * everything before 'top'
			 */
			struct mark *old;
			while ((old = vmark_first(focus, rl->typenum)) != NULL &&
			       old != top) {
				free(old->mdata);
				old->mdata = NULL;
				mark_free(old);
			}
		}
	}
	pane_damaged(ci->home, DAMAGED_CONTENT);
	return 1;
}

DEF_CMD(render_lines_set_cursor)
{
	struct pane *p = ci->home;
	struct pane *focus = pane_final_child(p);
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
		render_line(p, focus, m->mdata, &y, 0, scale, &cx, &cy, &o, NULL);
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
	struct pane *focus = ci->focus;
	struct rl_data *rl = p->data;
	struct mark *pm = ci->mark;
	struct mark *top, *bot;

	rl->ignore_point = 1;
	top = vmark_first(focus, rl->typenum);
	bot = vmark_last(focus, rl->typenum);
	if (top && bot &&
	    mark_ordered(top, pm) &&
	    mark_ordered(pm, bot) && !mark_same_pane(focus, pm, bot, NULL))
		/* pos already displayed */
		return 1;
	find_lines(pm, p, focus);
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
	struct pane *focus = ci->focus;
	struct rl_data *rl = p->data;
	int target_x, target_y;
	int o = -1;
	int scale = get_scale(focus);
	int num;

	rl->ignore_point = 0;

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
	if (!call5("Move-EOL", ci->focus, num, ci->mark, NULL, 0))
		return -1;
	if (RPT_NUM(ci) > 0) {
		/* at end of target line, move to start */
		if (!call5("Move-EOL", ci->focus, -1, ci->mark, NULL, 0))
			return -1;
	}

	/* restore target: Move-EOL might have changed it */
	rl->target_x = target_x;
	rl->target_y = target_y;

	if (target_x >= 0 || target_y >= 0) {
		struct mark *start =
			vmark_at_point(focus, rl->typenum);
		int y = 0;
		if (!start || !start->mdata) {
			pane_damaged(p, DAMAGED_CONTENT);
			return 1;
		}
		render_line(p, focus, start->mdata, &y, 0, scale, &target_x, &target_y, &o, NULL);
		/* 'o' is the distance from start-of-line of the target */
		if (o >= 0) {
			struct mark *m2 = call_render_line_offset(
				p, start, o);
			if (m2)
				mark_to_mark(ci->mark, m2);
			mark_free(m2);
		}
	}
	pane_damaged(p, DAMAGED_CURSOR);
	return 1;
}

DEF_CMD(render_lines_notify_replace)
{
	struct rl_data *rl = ci->home->data;
	struct mark *start = ci->mark2;
	struct mark *end, *t;
	struct pane *p = pane_final_child(ci->home);

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
		return comm_call_pane(c, "Clone", parent->focus,
				      0, NULL, NULL, 0, NULL);
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
	struct rl_data *rl = calloc(1, sizeof(*rl));

	if (!rl_map)
		render_lines_register_map();

	rl->target_x = -1;
	rl->target_y = -1;
	rl->do_wrap = 1;
	rl->typenum = doc_add_view(ci->focus);
	rl->pane = pane_register(ci->focus, 0, &render_lines_handle.c, rl, NULL);
	call3("Request:Notify:Replace", rl->pane, 0, NULL);

	return comm_call(ci->comm2, "callback:attach", rl->pane,
			 0, NULL, NULL, 0);
}

void edlib_init(struct pane *ed)
{
	call_comm("global-set-command", ed, 0, NULL, "attach-render-lines",
		  0, &render_lines_attach);
}
