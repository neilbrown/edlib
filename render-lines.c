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

#include <unistd.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include <stdio.h> // sscanf

#define	MARK_DATA_PTR char
#include "core.h"
#include "misc.h"

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
};

struct render_list {
	struct render_list *next;
	char	*text_orig;
	char	*text safe, *attr safe; // both are allocated
	short	x, width;
	short	cursorpos;
	char	*curs;
};

#define WRAP 1
#define CURS 2

static int draw_some(struct pane *p safe, struct pane *focus safe,
		     struct render_list **rlp safe,
		     int *x safe,
		     char *start safe, char **endp safe,
		     char *attr safe, int margin, int cursorpos, int cursx,
		     int scale)
{
	/* Measure the text from 'start' for length 'len', expecting to
	 * draw to p[x,?].
	 * Update 'x' and 'startp' past what was drawn.
	 * Everything will be drawn with the same attributes: attr.
	 * If the text would get closer to right end than 'margin',
	 * we stop drawing before then.  If this happens, WRAP is returned.
	 * If drawing would pass cursx, stop there and record pointer
	 * into 'start'.
	 * If cursorpos is between 0 and len inclusive, a cursor is drawn there.
	 */
	int len = *endp - start;
	char *str;
	struct call_return cr = {};
	int max;
	int ret = WRAP;
	int rmargin = p->w - margin;
	struct render_list *rl;

	if (len == 0 && cursorpos < 0)
		return 0;
	if ((*rlp == NULL ||
	     ((*rlp)->next == NULL && (*rlp)->text_orig == NULL)) &&
	    strstr(attr, "wrap,") && (cursorpos < 0|| cursorpos > len))
		/* No wrap text at start of line, unless it
		 * contains cursor.
		 */
		return 0;
	str = strndup(start, len);
	if (*str == '\t')
		*str = ' ';
	if (cursx >= 0 && cursx >= *x && cursx < rmargin) {
		rmargin = cursx;
		ret = CURS;
	}

	rl = calloc(1, sizeof(*rl));
	cr = home_call_ret(all, focus, "text-size", p, rmargin - *x, NULL, str,
			   scale, NULL, attr);
	max = cr.i;
	if (max == 0 && ret == CURS) {
		/* must already have CURS position. */
		rl->curs = start;
		ret = WRAP;
		rmargin = p->w - margin;
		cr = home_call_ret(all, focus, "text-size", p,
				   rmargin - *x, NULL, str,
				   scale, NULL, attr);
		max = cr.i;
	}
	if (max < len) {
		str[max] = 0;
		cr = home_call_ret(all, focus, "text-size", p,
				   rmargin - *x, NULL, str,
				   scale, NULL, attr);
	}

	rl->text_orig = start;
	rl->text = str; str = NULL;
	rl->attr = strdup(attr);
	rl->width = cr.x;
	rl->x = *x;
	if (ret == CURS)
		rl->curs = start + strlen(rl->text);

	if (cursorpos >= 0 && cursorpos <= len && cursorpos <= max)
		rl->cursorpos = cursorpos;
	else
		rl->cursorpos = -1;
	while (*rlp)
		rlp = &(*rlp)->next;
	*rlp = rl;

	free(str);
	*x += cr.x;
	if (max >= len)
		return 0;
	/* Didn't draw everything. */
	*endp = start + max;
	return ret;
}

static char *get_last_attr(char *attrs safe, char *attr safe)
{
	char *com = attrs + strlen(attrs);
	int len = strlen(attr);
	for (; com >= attrs ; com--) {
		int i = 1;
		if (*com != ',' && com > attrs)
			continue;
		if (com == attrs)
			i = 0;
		if (strncmp(com+i, attr, len) != 0)
			continue;
		if (com[i+len] != ':')
			continue;
		com += i+len+1;
		for (i=0; com[i] && com[i] != ','; i++)
			;
		return strndup(com, i);
	}
	return NULL;
}

static int flush_line(struct pane *p safe, struct pane *focus safe, int dodraw,
		      struct render_list **rlp safe,
		      int y, int scale, int wrap_pos,
		      char **curspos, char **cursattr)
{
	struct render_list *last_wrap = NULL, *end_wrap = NULL, *last_rl = NULL;
	int in_wrap = 0;
	int wrap_len = 0;
	struct render_list *rl, *tofree;
	int x = 0;
	char *head;

	if (!*rlp)
		return 0;
	for (rl = *rlp; wrap_pos && rl; rl = rl->next) {
		if (strstr(rl->attr, "wrap,") && rl != *rlp) {
			if (!in_wrap) {
				last_wrap = rl;
				in_wrap = 1;
				wrap_len = 0;
			}
			wrap_len += strlen(rl->text);
			end_wrap = rl->next;
		} else {
			if (in_wrap)
				end_wrap = rl;
			in_wrap = 0;
		}
		last_rl = rl;
	}
	if (last_wrap)
		last_rl = last_wrap;
	for (rl = *rlp; rl && rl != last_wrap; rl = rl->next) {
		int cp = rl->cursorpos;
		if (wrap_pos &&
		    cp >= (int)strlen(rl->text) + wrap_len)
			cp = -1;
		x = rl->x;
		if (dodraw)
			home_call(focus, "Draw:text", p, cp, NULL, rl->text,
				  scale, NULL, rl->attr,
				  x, y);
		x += rl->width;
		if (curspos && rl->curs) {
			*curspos = rl->curs;
			if (cursattr)
				*cursattr = strdup(rl->attr);
		}
	}
	for (; rl && rl != end_wrap; rl = rl->next) {
		int cp = rl->cursorpos;
		if (cp >= (int)strlen(rl->text))
			cp = -1;

		if (cp >= 0 && dodraw)
			home_call(focus, "Draw:text", p, cp, NULL, rl->text,
				  scale, NULL, rl->attr,
				  rl->x, y);
		x = rl->x + rl->width;
	}
	if (wrap_pos && last_rl && dodraw) {
		char *e = get_last_attr(last_rl->attr, "wrap-tail");
		home_call(focus, "Draw:text", p, -1, NULL, e ?: "\\",
			  scale, NULL, "underline,fg:blue",
			  wrap_pos, y);
		free(e);
	}

	tofree = *rlp;
	*rlp = end_wrap;

	if (wrap_pos && last_rl &&
	    (head = get_last_attr(last_rl->attr, "wrap-head"))) {
		struct call_return cr =
			home_call_ret(all, focus, "text-size", p,
				      p->w, NULL, head,
				      scale, NULL, last_rl->attr);
		rl = calloc(1, sizeof(*rl));
		rl->text = head;
		rl->attr = strdup(last_rl->attr); // FIXME underline,fg:blue ???
		rl->width = cr.x;
		rl->x = 0;
		rl->cursorpos = -1;
		rl->next = *rlp;
		*rlp = rl;
		x -= cr.x;
	}

	for (rl = tofree; rl && rl != end_wrap; rl = tofree) {
		tofree = rl->next;
		free(rl->text);
		free(rl->attr);
		free(rl);
	}

	for (rl = end_wrap; rl; rl = rl->next)
		rl->x -= x;
	return x;
}

static void update_line_height_attr(struct pane *p safe,
				    struct pane *focus safe,
				    int *h safe,
				    int *a safe,int *w, char *attr safe,
				    char *str safe, int scale)
{
	struct call_return cr = home_call_ret(all, focus, "text-size", p,
					      -1, NULL, str,
					      scale, NULL, attr);
	if (cr.y > *h)
		*h = cr.y;
	if (cr.i2 > *a)
		*a = cr.i2;
	if (w)
		*w += cr.x;
}

static void update_line_height(struct pane *p safe, struct pane *focus safe,
			       int *h safe, int *a safe,
			       int *w safe, int *center, char *line safe,
			       int scale)
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
		if (c == '<' && *line == '<') {
			line += 1;
			continue;
		}
		if (c != '<')
			continue;

		if (line - 1 > segstart) {
			char *l = strndup(segstart, line - 1 - segstart);
			update_line_height_attr(p, focus, h, a, w,
						buf_final(&attr), l, scale);
			free(l);
		}
		while (*line && line[-1] != '>')
			line += 1;
		segstart = line;
		if (st[0] != '/') {
			char *c2;
			char *b;

			buf_concat_len(&attr, st, line-st);
			attr.b[attr.len-1] = ',';
			buf_append(&attr, ',');
			b = buf_final(&attr);
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
			update_line_height_attr(p, focus, h, a, w, b, "",
						scale);
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
		update_line_height_attr(p, focus, h, a, w,
					buf_final(&attr), l, scale);
		free(l);
	}
	*h += above + below;
	*a += above;
	free(buf_final(&attr));
}

DEF_CMD(null)
{
	return 0;
}

static void render_image(struct pane *p safe, struct pane *focus safe,
			 char *line safe, short *yp safe,
			 int dodraw, int scale)
{
	char *fname = NULL;
	short width = p->w/2, height = p->h/2;

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
		struct pane *tmp = pane_register(p, -1, &null, NULL);

		pane_resize(tmp, (p->w - width)/2, *yp, width, height);
		home_call(focus, "Draw:image", tmp, 0, NULL, fname, 5);
		pane_close(tmp);
	}
	*yp += height;
	free(fname);
}

static void find_cursor(struct render_list *rlst,
			struct pane *p safe, struct pane *focus safe, int cx,
			int scale, char **curspos safe, char **cursattr safe)
{
	while (rlst &&
	       rlst->x + rlst->width < cx)
		rlst = rlst->next;
	if (!rlst)
		return;
	if (rlst->x > cx)
		*curspos = rlst->text_orig;
	else {
		struct call_return cr = home_call_ret(
			all, focus, "text-size", p,
			cx - rlst->x, NULL, rlst->text,
			scale, NULL, rlst->attr);
		*curspos = rlst->text_orig + cr.i;
	}
	*cursattr = strdup(rlst->attr);
}
/* Render a line, with attributes and wrapping.
 * Report line offset where cursor point cx,cy is passed. -1 if never seen.
 * Report cx,cy location where char at 'offsetp' was drawn, or -1.
 * Note offsetp, cxp, cyp are in-out parameters.
 * The location that comes in as *offsetp goes out as *cxp,*cyp
 * The location that comes in as *cxp,*cyp goes out as *offsetp.
 */
static void render_line(struct pane *p safe, struct pane *focus safe,
			char *line safe, short *yp safe, int dodraw, int scale,
			short *cxp, short *cyp, short *cwp, short *offsetp,
			char **offset_attrs,
			int *end_of_pagep, short *cols)
{
	int x = 0;
	int y = *yp;
	char *line_start = line;
	char *start = line;
	struct buf attr;
	unsigned char ch;
	int cy = -1, cx = -1, offset = -1;
	int wrap_offset = 0; /*number of columns displayed in earlier lines */
	int in_tab = 0;
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
	int end_of_page = 0;
	struct render_list *rlst = NULL;
	char *curspos = NULL;
	char *cursattr = NULL;
	char *cstart = NULL;

	if (strncmp(line, "<image:",7) == 0) {
		/* For now an <image> must be on a line by itself.
		 * Maybe this can be changed later if I decide on
		 * something that makes sense.
		 * The cursor is not on the image.
		 */
		render_image(p, focus, line, yp, dodraw, scale);
		if (cxp)
			*cxp = -1;
		if (cyp)
			*cyp = -1;
		if (cwp)
			*cwp = 0;
		if (offsetp)
			*offsetp = -1;
		if (cols && *cols < p->w)
			*cols = p->w;
		return;
	}

	update_line_height(p, focus, &line_height, &ascent, &twidth, &center,
			   line, scale);

	if (!wrap)
		x -= rl->shift_left;

	if (prefix) {
		char *s = prefix + strlen(prefix);
		update_line_height_attr(p, focus, &line_height, &ascent, NULL,
					"bold", prefix, scale);
		draw_some(p, focus, &rlst, &x, prefix, &s, "bold",
			  0, -1, -1, scale);
	}
	rl->prefix_len = x + rl->shift_left;
	if (center == 1)
		x += (p->w - x - twidth) / 2;
	if (center > 1)
		x += center;
	if (center < 0)
		x = p->w - x - twidth + center;
	margin = x;
	if (cols && *cols < x + twidth)
		*cols = x + twidth;

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
		*cxp = -1;
		*cyp = -1;
		if (cwp)
			*cwp = 0;
	}
	if (cy >= 0 && cy < y) {
		/* cursor is not here */
		*offsetp = -1;
		cx = cy = -1;
	}

	while (*line && y < p->h && !end_of_page) {
		int CX;

		if (mwidth <= 0) {
			struct call_return cr = home_call_ret(all, focus,
							      "text-size", p,
							      -1, NULL, "M",
							      0, NULL,
							      buf_final(&attr));
			mwidth = cr.x;
			if (mwidth <= 0)
				mwidth = 1;
			/* Assume mwidth is the cursor width */
			if (cwp)
				*cwp = mwidth;
		}

		if (ret == CURS) {
			/* Found the cursor, stop looking */
			cy = -1; cx = -1;
		}
		if (y+line_height >= cy &&
		    y <= cy && x <= cx)
			CX = cx;
		else
			CX = -1;

		if (y > cy && offsetp && curspos) {
			*offsetp = curspos - line_start;
			offsetp = NULL;
			if (offset_attrs)
				*offset_attrs = cursattr;
			else
				free(cursattr);
			cursattr = NULL;
		}

		if (offset >= 0 && start - line_start <= offset) {
			if (y >= 0 && (y == 0 || y + line_height <= p->h)) {
				if (cstart != start) {
					*cyp = y;
					*cxp = x;
					cstart = start;
				}
			} else {
				*cyp = *cxp = -1;
			}
		}

		if ((ret == WRAP|| x >= p->w - mwidth) &&
		    (line[0] != '<' || line[1] == '<')) {
			/* No room for more text */
			if (wrap && *line && *line != '\n') {
				int len = flush_line(p, focus, dodraw, &rlst,
						     y+ascent, scale,
						     p->w - mwidth,
						     &curspos, &cursattr);
				wrap_offset += len;
				x -= len;
				if (x < 0)
					x = 0;
				y += line_height;
				if (offsetp) {
					if (y+line_height >= cy &&
					    y <= cy && x > cx) {
						/* cursor is in field move down */
						find_cursor(rlst, p, focus,
							    cx, scale,
							    &curspos, &cursattr);
					}
					if (curspos) {
						*offsetp = curspos - line_start;
						offsetp = NULL;
						if (offset_attrs)
							*offset_attrs = cursattr;
						else
							free(cursattr);
						cursattr = NULL;
					}
				}
			} else {
				/* Skip over normal text, but make sure
				 * to handle newline and attributes
				 * correctly.
				 */
				//while (*line && *line != '\n' && *line != '<')
				//	line += 1;
				line += strcspn(line, "\n");
				start = line;
			}
		}

		ret = 0;
		ch = *line;
		if (ch >= ' ' && ch != '<') {
			line += 1;
			/* Only flush out if string is getting a bit long.
			 * i.e.  if we have reached the offset we are
			 * measuring to, or if we could have reached the
			 * right margin.
			 */
			if ((*line & 0xc0) == 0x80)
				/* In the middle of a UTF-8 */
				continue;
			if (offset == (line - line_start) ||
			    (line-start) * mwidth > p->w - x ||
			    (CX>x && (line - start)*mwidth > CX - x)) {
				ret = draw_some(p, focus, &rlst, &x, start,
						&line,
						buf_final(&attr),
						wrap ? mwidth : 0,
						offset - (start - line_start),
						CX, scale);
				start = line;
			}
			continue;
		}
		ret = draw_some(p, focus, &rlst, &x, start, &line,
				buf_final(&attr),
				wrap ? mwidth : 0,
				in_tab ?:offset - (start - line_start),
				CX, scale);
		start = line;
		if (ret || !ch)
			continue;
		if (ch == '<') {
			line += 1;
			if (*line == '<') {
				ret = draw_some(p, focus, &rlst, &x, start, &line,
						buf_final(&attr),
						wrap ? mwidth : 0,
						in_tab ?:offset - (start - line_start),
						CX, scale);
				if (ret)
					continue;
				start += 2;
				line = start;
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
						x = margin +
						atoi(tb+4) * scale / 1000;
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

		line += 1;
		if (ch == '\n') {
			curspos = line-1;
			flush_line(p, focus, dodraw, &rlst, y+ascent, scale, 0,
				   &curspos, &cursattr);
			y += line_height;
			x = 0;
			wrap_offset = 0;
			start = line;
			if (curspos && offsetp) {
				*offsetp = curspos - line_start;
				offsetp = NULL;
				if (offset_attrs)
					*offset_attrs = cursattr;
				else
					free(cursattr);
				cursattr = NULL;
			}
		} else if (ch == '\f') {
			x = 0;
			start = line;
			wrap_offset = 0;
			end_of_page = 1;
		} else if (ch == '\t') {
			int xc = (wrap_offset + x) / mwidth;
			int w = 8 - xc % 8;
			ret = draw_some(p, focus, &rlst, &x, start, &line,
					buf_final(&attr),
					wrap ? mwidth*2: 0,
					offset == (start - line_start)
					? in_tab : -1,
					CX, scale);
			if (w > 1) {
				line -= 1;
				in_tab = -1; // suppress extra cursors
			} else
				in_tab = 0;
			start = line;
		} else {
			char buf[4], *b;
			int l = attr.len;
			buf[0] = '^';
			buf[1] = ch + '@';
			buf[2] = 0;
			b = buf+2;
			buf_concat(&attr, ",underline,fg:red");
			ret = draw_some(p, focus, &rlst, &x, buf, &b,
					buf_final(&attr),
					wrap ? mwidth*2: 0,
					offset - (start - line_start),
					CX, scale);
			attr.len = l;
			start = line;
		}
	}
	if (!*line && (line > start || offset == start - line_start)) {
		/* Some more to draw */
		draw_some(p, focus, &rlst, &x, start, &line,
			  buf_final(&attr),
			  wrap ? mwidth : 0, offset - (start - line_start),
			  cx, scale);
	}

	flush_line(p, focus, dodraw, &rlst, y+ascent, scale, 0,
		   &curspos, &cursattr);

	if (offsetp && curspos) {
		*offsetp = curspos - line_start;
		curspos = NULL;
		if (offset_attrs)
			*offset_attrs = cursattr;
		else
			free(cursattr);
		cursattr = NULL;
	}

	if (offset >= 0 && line - line_start <= offset
	    &&/* Must be true when offset >= 0 */ cyp && cxp) {
		if (y >= 0 && (y == 0 || y + line_height <= p->h)) {
			if (cstart != start) {
				*cyp = y;
				*cxp = x;
				cstart = start;
			}
		} else {
			*cyp = *cxp = -1;
		}
	}
	if (x > 0 || y == *yp)
		/* No newline at the end .. but we must render as whole lines */
		y += line_height;
	*yp = y;
	free(buf_final(&attr));
	if (end_of_pagep && end_of_page)
		*end_of_pagep = end_of_page;
	while (rlst) {
		struct render_list *r = rlst;
		rlst = r->next;
		free(r->text);
		free(r->attr);
		free(r);
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
	short x;
	short y = 0;
	short y_above = 0, y_below = 0;
	short offset;
	int found_start = 0, found_end = 0;
	short lines_above = 0, lines_below = 0;
	struct xy scale = pane_scale(focus);

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

	x = -1; lines_above = -1; y = 0;
	render_line(p, focus, start->mdata ?: "", &y, 0, scale.x,
		    &x, &lines_above, NULL, &offset, NULL, &found_end, NULL);
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
				if (start->mdata)
					render_line(p, focus, start->mdata, &h,
						    0, scale.x,
						    NULL, NULL, NULL, NULL, NULL,
						    &found_end, NULL);
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
				short h = 0;
				render_line(p, focus, end->mdata, &h, 0,
					    scale.x,
					    NULL, NULL, NULL, NULL, NULL,
					    &found_end, NULL);
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
		  struct pane *focus safe, short *cols safe)
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
	int found_end;
	short mwidth = -1;

	hdr = pane_attr_get(focus, "heading");
	if (hdr && !*hdr)
		hdr = NULL;
	s = pane_attr_get(focus, "hide-cursor");
	if (s && strcmp(s, "yes") == 0)
		hide_cursor = 1;
	s = pane_attr_get(focus, "background");

restart:
	found_end = 0;
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
		render_line(p, focus, hdr, &y, 1, scale.x,
			    NULL, NULL, NULL, NULL, NULL, NULL, cols);
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
		if (!hide_cursor && p->cx <= 0 && pm &&
		    mark_ordered_or_same(m, pm) &&
		    (!(m2 && doc_following_pane(focus, m2) != WEOF) ||
		     mark_ordered_not_same(pm, m2))) {
			short cw = -1;
			short len = call_render_line_to_point(focus, pm,
							      m);
			rl->cursor_line = y;
			render_line(p, focus, m->mdata ?: "", &y, 1, scale.x,
				    &p->cx, &p->cy, &cw, &len,
				    NULL,  NULL, cols);
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
				if (p->cx + cw + 8 * mwidth < p->w) {
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
			if (p->cx + cw >= p->w && !rl->do_wrap &&
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
				while (p->cx + cw >= p->w) {
					rl->shift_left += 8 * mwidth;
					p->cx -= 8 * mwidth;
				}
				shifted = 2;
				goto restart;
			}
			cursor_drawn = 1;
		} else
			render_line(p, focus, m->mdata?:"", &y, 1, scale.x,
				    NULL, NULL, NULL,
				    NULL, NULL, &found_end, cols);
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
		rl->lines = render(pm, p, focus, &rl->cols);
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
	rl->lines = render(m, p, focus, &rl->cols);
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
		rl->lines = render(pm, p, focus, &rl->cols);
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
	struct xy scale = pane_scale(focus);

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
				render_line(p, focus, m->mdata, &y, 0, scale.x,
					    NULL, NULL, NULL,
					    NULL, NULL, NULL, NULL);
				m = vmark_next(m);
			}
			rl->skip_lines = y;
		}
	} else {
		while (top && rpt > 0) {
			short y = 0;
			int page_end = 0;

			if (top->mdata == NULL)
				call_render_line(focus, top);
			if (top->mdata == NULL)
				break;
			render_line(p, focus, top->mdata, &y, 0, scale.x,
				    NULL, NULL, NULL, NULL, NULL,
				    &page_end, NULL);
			if (page_end)
				y = rpt % pagesize;
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

static char *get_active_tag(char *a)
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
	struct xy scale = pane_scale(p);

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
		short cx = cihx, cy = cihy, o = -1;
		char *oattrs = NULL;
		call_render_line(focus, m);
		render_line(p, focus, m->mdata, &y, 0, scale.x, &cx, &cy, NULL,
			    &o, &oattrs, NULL, NULL);
		if (o >= 0) {
			struct mark *m2 = call_render_line_offset(focus, m, o);
			if (m2) {
				char *tag = get_active_tag(oattrs);
				if (tag)
					call("Mouse-Activate", focus, 0, m2, tag,
					     0, ci->mark, oattrs);
				free(tag);
				if (!newpoint)
					newpoint = mark_dup(m2);
				else
					mark_to_mark(newpoint, m2);
				mark_free(m2);
				found = 1;
			}
			free(oattrs);
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
	short o = -1;
	struct xy scale = pane_scale(focus);
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
		render_line(p, focus, start->mdata?:"", &y, 0, scale.x,
			    &target_x, &target_y, NULL, &o, NULL, NULL, NULL);
		/* 'o' is the distance from start-of-line to the target */
		if (o >= 0) {
			struct mark *m2 = call_render_line_offset(
				focus, start, o);
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
	key_add(rl_map, "Notify:clip", &render_lines_clip);

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
	p = ci->focus;
	if (strcmp(ci->key, "attach-render-text") == 0)
		p = call_ret(pane, "attach-markup", p);
	p = pane_register(p, 0, &render_lines_handle.c, rl);
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
