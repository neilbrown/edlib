/*
 * Copyright Neil Brown <neil@brown.name> 2015
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
	struct command	type;
	int		typenum;
	struct pane	*pane;
};

struct rl_mark {
	struct mark	m;
	char		*line;
};

/* render a line, with attributes and wrapping.  Report line offset where
 * cursor point cx,cy is passed. -1 if never seen.
 */
static void render_line(struct pane *p, char *line, int *yp, int dodraw,
		       int *cxp, int *cyp, int *offsetp)
{
	int x = 0;
	int y = *yp;
	mbstate_t ps = {0};
	char *line_start = line;
	int l = strlen(line);
	struct buf attr;
	wchar_t ch;
	int cy = -1, cx = -1, offset = -1;
	struct rl_data *rl = p->data;
	int wrap = rl->do_wrap;
	char *prefix = pane_attr_get(p, "prefix");

	if (prefix) {
		char *s = prefix;
		while (*s) {
			if (y >= rl->header_lines)
				pane_text(p, *s, "bold", x, y);
			x += 1;
			s += 1;
		}
	}
	rl->prefix_len = x;

	buf_init(&attr);
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
		int err = mbrtowc(&ch, line, l, &ps);
		int draw_cursor = 0;
		int l;

		if (err < 0) {
			ch = *line;
			err = 1;
		}
		if (y == cy && x <= cx)
			/* haven't passed the cursor yet */
			*offsetp = line - line_start;
		if (offset >= 0 && line - line_start <= offset) {
			*cyp = y;
			*cxp = x;
		}
		if (line - line_start == offset)
			draw_cursor = 1;

		if (err == 0)
			break;
		line += err;
		l -= err;

		if (ch == '<') {
			if (*line == '<') {
				line += 1;
				l -= 1;
			} else {
				char *a = line;
				while (*line && line[-1] != '>') {
					line += 1;
					l =- 1;
				}
				if (a[0] != '/') {
					buf_concat_len(&attr, a, line-a);
					/* mark location with ",," */
					attr.b[attr.len-1] = ',';
					buf_append(&attr, ',');
				} else {
					/* strip back to ",," */
					if (attr.len > 0)
						attr.len -= 2;
					while (attr.len > 0 &&
					       (attr.b[attr.len-1] != ',' ||
						attr.b[attr.len-2] != ','))
						attr.len -= 1;
				}
				continue;
			}
		}
		if (draw_cursor) {
			l = attr.len;
			buf_concat(&attr, ",inverse");
			if (dodraw)
				pane_text(p, ' ', buf_final(&attr), x, y);
		}
		if (ch == '\n') {
			x = 0;
			y += 1;
		} else {
			int w = 1;
			if (ch == '\t')
				w = 9 - x % 8;
			else if (ch < ' ')
				w = 2;
			else
				w = 1;
			if (x + w >= p->w && wrap) {
				/* line wrap */
				if (dodraw && x >= rl->prefix_len && y >= rl->header_lines)
					pane_text(p, '\\', "underline,fg:blue",
						  p->w-1, y);
				y += 1;
				x = rl->prefix_len;
			}
			if (!dodraw || x < rl->prefix_len || y < rl->header_lines)
				;
			else if (ch == '\t')
				;
			else if (ch < ' ') {
				/* Should not happen, bug just in case ... */
				pane_text(p, '^', "underline,fg:red", x, y);
				pane_text(p, ch + '@', "underline,fg:blue", x+1, y);
			} else {
				pane_text(p, ch, buf_final(&attr), x, y);
			}
			x += w;
		}
		if (draw_cursor)
			attr.len = l;
	}
	if ((y == cy && x <= cx) || y < cy)
		/* haven't passed the cursor yet */
		*offsetp = line - line_start;
	if (offset >= 0 && line - line_start <= offset) {
		*cyp = y;
		*cxp = x;
	}
	if (x > 0)
		/* No newline at the end .. but we must render as whole lines */
		y += 1;
	*yp = y;
	free(buf_final(&attr));
}

static struct mark *call_render_line_prev(struct pane *p, struct point **ptp,
					  struct mark *m, int n, int *found)
{
	struct cmd_info ci = {0};
	int ret;

	ci.key = "render-line-prev";
	ci.pointp = ptp;
	ci.mark = m;
	ci.focus = p;
	ci.numeric = n;
	ret = key_handle(&ci);
	if (ret == 0) {
		mark_free(m);
		return NULL;
	}
	/* if n>0 we can fail because start-of-file was found before
	 * and newline.  In that case ret == -2, and we return NULL.
	 */
	if (found)
		*found = (ret != -1);
	if (ret < 0) {
		/* current line is start-of-file */
		mark_free(m);
		return NULL;
	}

	m = vmark_matching((*ptp)->doc, ci.mark);
	if (m)
		mark_free(ci.mark);
	else
		m = ci.mark;
	return m;
}

static struct mark *call_render_line(struct pane *p, struct point **ptp,
				     struct rl_mark *start)
{
	struct cmd_info ci = {0};
	struct mark *m, *m2;

	ci.key = "render-line";
	ci.focus = p;
	ci.pointp = ptp;
	ci.mark = mark_dup(&start->m, 0);
	ci.numeric = NO_NUMERIC;
	/* Allow for filling the rest of the pane, given that
	 * some has been used.
	 * 'used' can be negative if the mark is before the start
	 * of the pane
	 */
	if (key_handle(&ci) == 0) {
		mark_free(ci.mark);
		return NULL;
	}

	if (start->line)
		free(start->line);
	start->line = ci.str;

	m = vmark_matching((*ptp)->doc, ci.mark);
	if (m)
		mark_free(ci.mark);
	else
		m = ci.mark;
	/* Any mark between start and m must be discarded,
	 */
	while ((m2 = vmark_next(&start->m)) != NULL &&
	       mark_ordered(m2, m)) {
			struct rl_mark *rm2 = container_of(m2, struct rl_mark, m);
			free(rm2->line);
			mark_free(m2);
	}

	return m;
}

static struct mark *call_render_line_offset(struct pane *p, struct point **ptp,
					    struct rl_mark *start, int offset)
{
	struct cmd_info ci = {0};

	ci.key = "render-line";
	ci.focus = p;
	ci.pointp = ptp;
	ci.mark = mark_dup(&start->m, 0);
	ci.numeric = offset;
	if (key_handle(&ci) == 0) {
		mark_free(ci.mark);
		return NULL;
	}
	free(ci.str);
	return ci.mark;
}

static int call_render_line_to_point(struct pane *p, struct point **ptp,
				     struct rl_mark *start)
{
	struct cmd_info ci = {0};
	int len;

	ci.key = "render-line";
	ci.focus = p;
	ci.pointp = ptp;
	ci.mark = mark_dup(&start->m, 0);
	ci.numeric = -1;
	if (key_handle(&ci) == 0) {
		mark_free(ci.mark);
		return 0;
	}
	mark_free(ci.mark);
	if (ci.str) {
		len = strlen(ci.str);
		free(ci.str);
	} else
		len = 0;
	return len;
}

static void find_lines(struct point **ptp, struct pane *p)
{
	struct rl_data *rl = p->data;
	struct doc *d;
	struct rl_mark *top, *bot;
	struct mark *m;
	struct rl_mark *start, *end;
	int y = 0;
	int offset;
	int found_start = 0, found_end = 0;
	int lines_above = 0, lines_below = 0;

	d = (*ptp)->doc;
	top = container_of(vmark_first(d, rl->typenum), struct rl_mark, m);
	bot = container_of(vmark_last(d, rl->typenum), struct rl_mark, m);
	m = call_render_line_prev(p, ptp, mark_at_point(*ptp, rl->typenum),
				  0, &rl->top_sol);
	if (!m)
		return;
	start = container_of(m, struct rl_mark, m);
	offset = call_render_line_to_point(p, ptp, start);
	if (start->line == NULL)
		m = call_render_line(p, ptp, start);
	else
		m = vmark_next(&start->m);

	end = container_of(m, struct rl_mark, m);
	if (start->line) {
		int x;
		x = -1; lines_above = -1; y = 0;
		render_line(p, start->line, &y, 0, &x, &lines_above, &offset);
		lines_below = y - lines_above;
	}
	y = 1;
	/* We have start/end of the focus line, and its height */
	if (bot && !mark_ordered_or_same(d, &bot->m, &start->m))
		/* already before 'bot', so will never "cross over" bot, so
		 * ignore 'bot'
		 */
		bot = NULL;
	if (top && !mark_ordered_or_same(d, &end->m, &top->m))
		top = NULL;

	rl->skip_lines = 0;
	while (!((found_start && found_end) || y >= p->h - rl->header_lines)) {
		if (!found_start) {
			/* step backwards moving start */
			if (lines_above > 0) {
				lines_above -= 1;
				y += 1;
			} else {
				m = call_render_line_prev(p, ptp,
							  mark_dup(&start->m, 0),
							  1, &rl->top_sol);
				if (!m) {
					/* no text before 'start' */
					found_start = 1;
				} else {
					int h = 0;
					start = container_of(m, struct rl_mark, m);
					if (!start->line)
						call_render_line(p, ptp, start);
					render_line(p, start->line, &h, 0,
						    NULL, NULL, NULL);
					if (h) {
						lines_above = h - 1;
						y += 1;
					} else
						found_start = 1;
				}
				if (bot && mark_ordered(&start->m, &bot->m))
					found_end = 1;
			}
		}
		if (!found_end) {
			/* step forwards */
			if (lines_below > 0) {
				lines_below -= 1;
				y += 1;
			} else {
				if (!end->line)
					call_render_line(p, ptp, end);
				if (!end->line)
					found_end = 1;
				else {
					int h = 0;
					render_line(p, end->line, &h, 0,
						    NULL, NULL, NULL);
					end = container_of(vmark_next(&end->m),
							   struct rl_mark, m);
					ASSERT(end != NULL);
					if (h) {
						lines_below = h - 1;
						y += 1;
					} else
						found_end = 1;
				}
				if (top && mark_ordered(&top->m, &end->m))
					found_start = 1;
			}
		}
	}
	rl->skip_lines = lines_above;
	/* Now discard any marks outside start-end */
	while ((m = vmark_prev(&start->m)) != NULL) {
		struct rl_mark *rm = container_of(m, struct rl_mark, m);
		free(rm->line);
		mark_free(m);
	}
	while ((m = vmark_next(&end->m)) != NULL) {
		struct rl_mark *rm = container_of(m, struct rl_mark, m);
		free(rm->line);
		mark_free(m);
	}
	free(end->line);
	end->line = NULL;
}

static void render(struct point **ptp, struct pane *p)
{
	struct rl_data *rl = p->data;
	struct doc *d;
	int y;
	struct rl_mark *m, *m2;
	int restarted = 0;
	char *hdr;

	d = (*ptp)->doc;

	hdr = pane_attr_get(p, "heading");
	if (hdr && !*hdr)
		hdr = NULL;

restart:
	pane_clear(p, NULL);
	y = 0;
	if (hdr) {
		rl->header_lines = 0;
		render_line(p, hdr, &y, 1, NULL, NULL, NULL);
		rl->header_lines = y;
	}
	y -= rl->skip_lines;
	m = container_of(vmark_first(d, rl->typenum), struct rl_mark, m);

	p->cx = p->cy = -1;
	rl->cursor_line = 0;

	while (m && y < p->h) {
		if (m->line == NULL) {
			/* This line has changed. */
			call_render_line(p, ptp, m);
		}
		m2 = container_of(vmark_next(&m->m), struct rl_mark, m);
		if (p->cx <= 0 &&
		    mark_ordered_or_same(d, &m->m, &(*ptp)->m) &&
		    (!m2 || mark_ordered_or_same(d, &(*ptp)->m, &m2->m))) {
			int len = call_render_line_to_point(p, ptp,
							    m);
			rl->cursor_line = y;
			render_line(p, m->line ?: "", &y, 1, &p->cx, &p->cy, &len);
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
			render_line(p, m->line?:"", &y, 1, NULL, NULL, NULL);
		if (!m2)
			break;
		m = m2;
	}
	/* Any marks after 'm' must be discarded */
	if (m) {
		free(m->line);
		m->line = NULL;
		while ((m2 = container_of(vmark_next(&m->m),
					  struct rl_mark, m)) != NULL) {
			free(m2->line);
			mark_free(&m2->m);
		}
	}
	return;
}

DEF_CMD(render_lines_refresh)
{
	struct pane *p = ci->home;
	struct rl_data *rl = p->data;
	struct doc *d;
	struct mark *m;
	char *a;

	pane_check_size(p);

	a = pane_attr_get(p, "render-wrap");
	rl->do_wrap = (!a || strcmp(a, "yes") == 0);

	d = (*ci->pointp)->doc;

	m = vmark_first(d, rl->typenum);
	if (rl->top_sol && m)
		m = call_render_line_prev(p, ci->pointp, mark_dup(m, 0), 0,
					  &rl->top_sol);

	if (m) {
		render(ci->pointp, p);
		if (rl->ignore_point || (p->cx >= 0 && p->cy < p->h))
			/* Found the cursor! */
			return 1;
	}
	find_lines(ci->pointp, p);
	render(ci->pointp, p);
	return 1;
}

DEF_CMD(render_lines_close)
{
	struct pane *p = ci->home;
	struct rl_data *rl = p->data;
	struct doc *d;
	struct mark *m;

	d = (*ci->pointp)->doc;

	while ((m = vmark_first(d, rl->typenum)) != NULL) {
		struct rl_mark *rm = container_of(m, struct rl_mark, m);
		free(rm->line);
		mark_free(m);
	}

	rl->pane = NULL;
	doc_del_view(p, &rl->type);
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
	struct point **ptp = ci->pointp;
	struct doc *d = (*ptp)->doc;
	struct mark *top;
	int pagesize = 1;

	top = vmark_first(d, rl->typenum);
	if (!top)
		return 0;
	if (strcmp(ci->key, "Move-View-Large") == 0)
		pagesize = p->h - 2;
	rpt *= pagesize;

	rl->ignore_point = 1;

	if (rpt < 0) {
		while (rpt < 0) {
			struct rl_mark *rm;
			int y = 0;

			if (rl->skip_lines) {
				rl->skip_lines -= 1;
				rpt += 1;
				continue;
			}

			top = call_render_line_prev(p, ptp, mark_dup(top, 0),
						    1, &rl->top_sol);
			if (!top)
				break;
			rm = container_of(top, struct rl_mark, m);
			if (rm->line == NULL)
				call_render_line(p, ptp, rm);
			if (rm->line == NULL)
				break;
			render_line(p, rm->line, &y, 0, NULL, NULL, NULL);
			rl->skip_lines = y;
		}
	} else {
		while (top && rpt > 0) {
			int y = 0;
			struct mark *old;
			struct rl_mark *rm = container_of(top, struct rl_mark, m);

			if (rm->line == NULL)
				call_render_line(p, ptp, rm);
			if (rm->line == NULL)
				break;
			render_line(p, rm->line, &y, 0, NULL, NULL, NULL);
			if (rl->skip_lines + rpt < y) {
				rl->skip_lines += rpt;
				break;
			}
			top = vmark_next(top);
			if ((rpt+pagesize-1)/pagesize !=
			    (rpt+pagesize-y-1)/pagesize)
				/* Have cross a full page, can discard old lines */
				while ((old = vmark_first(d, rl->typenum)) != NULL &&
				       old != top) {
					rm = container_of(old, struct rl_mark, m);
					free(rm->line);
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
	struct point **ptp = ci->pointp;
	struct doc *d = (*ptp)->doc;
	struct rl_data *rl = p->data;
	struct rl_mark *m;
	int y = rl->header_lines - rl->skip_lines;
	int found = 0;

	render_lines_other_move_func(ci);

	m = container_of(vmark_first(d, rl->typenum), struct rl_mark, m);

	while (y <= ci->hy && m && m->line) {
		int cx = ci->hx, cy = ci->hy, o = -1;
		render_line(p, m->line, &y, 0, &cx, &cy, &o);
		if (o >= 0) {
			struct mark *m2 = call_render_line_offset(p, ptp, m, o);
			if (m2) {
				point_to_mark(*ptp, m2);
				mark_free(m2);
				found = 1;
			}
		} else if (found)
			break;
		m = container_of(vmark_next(&m->m), struct rl_mark, m);
	}

	pane_focus(p);
	return 1;
}

DEF_CMD(render_lines_move_pos)
{
	struct pane *p = ci->home;
	struct rl_data *rl = p->data;
	struct point *pt = *ci->pointp;
	struct doc *d = pt->doc;
	struct mark *top, *bot;

	rl->ignore_point = 1;
	top = vmark_first(d, rl->typenum);
	bot = vmark_last(d, rl->typenum);
	if (top && bot &&
	    mark_ordered(top, &pt->m) &&
	    mark_ordered(&pt->m, bot))
		/* pos already displayed */
		return 1;
	find_lines(ci->pointp, ci->home);
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
	struct point **ptp = ci->pointp;
	struct cmd_info ci2 = {0};
	int target_x, target_y;
	int o = -1;

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
	ci2.mark = &(*ptp)->m;
	ci2.pointp = ci->pointp;
	if (!key_handle_focus(&ci2))
		return -1;
	if (RPT_NUM(ci) > 0) {
		/* at end of target line, move to start */
		ci2.numeric = -1;
		if (!key_handle_focus(&ci2))
			return -1;
	}

	/* restore target: Move-EOL might have changed it */
	rl->target_x = target_x;
	rl->target_y = target_y;

	if (target_x >= 0 || target_y >= 0) {
		struct rl_mark *start =
			container_of(vmark_at_point(*ci->pointp, rl->typenum),
				     struct rl_mark, m);
		int y = 0;
		if (!start || !start->line) {
			pane_damaged(p, DAMAGED_CONTENT);
			return 1;
		}
		render_line(p, start->line, &y, 0, &target_x, &target_y, &o);
		/* 'o' is the distance from start-of-line of the target */
		if (o >= 0) {
			struct mark *m2 = call_render_line_offset(
				p, ci->pointp, start, o);
			if (m2)
				point_to_mark(*ci->pointp, m2);
			mark_free(m2);
		}
	}
	return 1;
}

DEF_CMD(render_lines_notify)
{
	struct rl_data *rl = container_of(ci->comm, struct rl_data, type);

	if (strcmp(ci->key, "Replace") == 0) {
		if (ci->mark) {
			struct rl_mark *rm = container_of(ci->mark,
							  struct rl_mark, m);
			struct mark *vm;
			struct cmd_info ci2 = {0};
			struct pane *p = rl->pane;

			if (rm->line) {
				free(rm->line);
				rm->line = NULL;
			}
			/* If an adjacent mark is for the same location,
			 * delete it - marks must remain distinct
			 */
			while ((vm = vmark_prev(&rm->m)) != NULL &&
			       mark_same_pane(p, &rm->m, vm, &ci2)) {
				struct rl_mark *rlm = container_of(vm, struct rl_mark, m);
				free(rlm->line);
				mark_free(vm);
			}
			while ((vm = vmark_next(&rm->m)) != NULL &&
			       mark_same_pane(p, &rm->m, vm, &ci2)) {
				struct rl_mark *rlm = container_of(vm, struct rl_mark, m);
				free(rlm->line);
				mark_free(vm);
			}
			pane_damaged(rl->pane, DAMAGED_CONTENT);
		}
		return 1;
	}
	if (strcmp(ci->key, "Release") == 0) {
		if (rl->pane)
			pane_close(rl->pane);
		return 1;
	}
	return 0;
}


DEF_CMD(render_lines_attach);
DEF_CMD(render_lines_clone)
{
	struct pane *parent = ci->focus;
	struct pane *p = ci->home, *c;

	ci->pointp = pane_point(parent);
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
	struct doc *d = (*ci->pointp)->doc;
	struct mark *m;

	for (m = vmark_first(d, rl->typenum);
	     m;
	     m = vmark_next(m)) {
		struct rl_mark *rm = container_of(m, struct rl_mark, m);
		free(rm->line);
		rm->line = NULL;
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

	key_add(rl_map, "Close", &render_lines_close);
	key_add(rl_map, "Clone", &render_lines_clone);
	key_add(rl_map, "Refresh", &render_lines_refresh);

	/* force full refresh */
	key_add(rl_map, "render-lines:redraw", &render_lines_redraw);
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
	rl->type = render_lines_notify;
	rl->typenum = doc_add_view(ci->focus, &rl->type, sizeof(struct rl_mark));
	rl->pane = pane_register(ci->focus, 0, &render_lines_handle.c, rl, NULL);

	ci->focus = rl->pane;
	return 1;
}

void edlib_init(struct editor *ed)
{
	key_add(ed->commands, "render-lines-attach", &render_lines_attach);
}
