/*
 * Copyright Neil Brown ©2015-2020 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * A renderline pane will take a single line of marked-up text
 * and draw it.  The "line" may well be longer that the width
 * of the pane, and it might then be wrapped generatinging
 * multiple display lines.
 *
 * The render-lines pane will place multiple renderline panes and use
 * them to do the drawing - resizing and moving them as necessary to fit
 * the size of the text.
 *
 * A renderline normally is only active when the render-lines (or other)
 * parent pane is being refreshed - that pane hands over some of the
 * task to the renderline pane.
 * Specifically a "draw-markup" command provides a marked-up line of
 * text, a scale, and other details.  The resulting image in measured
 * and possibly drawn
 *
 *
 */

#include "core.h"
#include "misc.h"

struct render_list {
	struct render_list *next;
	const char	*text_orig;
	const char	*text safe, *attr safe; // both are allocated
	short		x, width;
	short		cursorpos;
	const char	*xypos;	/* location in text_orig where given x,y was found */
};

struct rline_data {
	short		prefix_len;
	const char	*xyattr;
};

#define WRAP 1
#define XYPOS 2

static int draw_some(struct pane *p safe, struct pane *focus safe,
		     struct render_list **rlp safe,
		     int *x safe,
		     const char *start safe, const char **endp safe,
		     const char *attr safe, int margin, int cursorpos, int xpos,
		     int scale)
{
	/* Measure the text from 'start' for length 'len', expecting to
	 * draw to p[x,?].
	 * Update 'x' and 'startp' past what was drawn.
	 * Everything will be drawn with the same attributes: attr.
	 * If the text would get closer to right end than 'margin',
	 * we stop drawing before then.  If this happens, WRAP is returned.
	 * If drawing would pass xpos, stop there and record pointer
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
	if (xpos >= 0 && xpos >= *x && xpos < rmargin) {
		rmargin = xpos;
		ret = XYPOS;
	}

	rl = calloc(1, sizeof(*rl));
	cr = home_call_ret(all, focus, "text-size", p, rmargin - *x, NULL, str,
			   scale, NULL, attr);
	max = cr.i;
	if (max == 0 && ret == XYPOS) {
		/* must already have XY position. */
		rl->xypos = start;
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
	if (ret == XYPOS)
		rl->xypos = start + strlen(rl->text);

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

static char *get_last_attr(const char *attrs safe, const char *attr safe)
{
	const char *com = attrs + strlen(attrs);
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
		      const char **xypos, const char **xyattr)
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
		if (xypos && rl->xypos) {
			*xypos = rl->xypos;
			if (xyattr)
				*xyattr = strsave(p, rl->attr);
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
		free((void*)rl->text);
		free((void*)rl->attr);
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
			       int *w safe, int *center, const char *line safe,
			       int scale)
{
	struct buf attr;
	int attr_found = 0;
	const char *segstart = line;
	int above = 0, below = 0;

	buf_init(&attr);
	buf_append(&attr, ',');
	while (*line) {
		char c = *line++;
		const char *st = line;
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
	if (line > segstart && line[-1] == '\n')
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

static int render_image(struct pane *p safe, struct pane *focus safe,
			const char *line safe, short y,
			int dodraw, int scale)
{
	char *fname = NULL;
	short width = p->w/2, height = p->h/2;

	while (*line == '<')
		line += 1;

	while (*line && *line != '>') {
		int len = strcspn(line, ",>");

		if (strncmp(line, "image:", 6) == 0) {
			const char *cp = line + 6;
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
		struct pane *tmp = pane_register(p, -1, &null);

		if (tmp) {
			pane_resize(tmp, (p->w - width)/2, y, width, height);
			home_call(focus, "Draw:image", tmp, 0, NULL, fname, 5);
			pane_close(tmp);
		}
	}
	free(fname);
	return y + height;
}

static void find_xypos(struct render_list *rlst,
		       struct pane *p safe, struct pane *focus safe, int posx,
		       int scale, const char **xypos safe,
		       const char **xyattr safe)
{
	while (rlst &&
	       rlst->x + rlst->width < posx)
		rlst = rlst->next;
	if (!rlst)
		return;
	if (rlst->x > posx)
		*xypos = rlst->text_orig;
	else {
		struct call_return cr = home_call_ret(
			all, focus, "text-size", p,
			posx - rlst->x, NULL, rlst->text,
			scale, NULL, rlst->attr);
		*xypos = rlst->text_orig + cr.i;
	}
	*xyattr = strsave(p, rlst->attr);
}
/* Render a line, with attributes and wrapping.
 * Report line offset and attributes where point x,y is passed, (assuming
 * non-negative) via that "xypos" callback.
 * Report cx,cy location where char at 'offsetp' was drawn, or -1.
 * Note offsetp, cxp, cyp are in-out parameters.
 * The location that comes in as *offsetp goes out as *cxp,*cyp
 * The location that comes in as *cxp,*cyp goes out as *offsetp.
 */

DEF_CMD(renderline)
{
	struct pane *p = ci->home;
	struct rline_data *rd = p->data;
	struct pane *focus = ci->focus;
	const char *line = ci->str;
	int dodraw = strcmp(ci->key, "render-line:draw") == 0;
	short posx = ci->x;
	short posy = ci->y;
	short offset = ci->num2;
	struct command *comm2 = ci->comm2;

	int x = 0;
	int y = 0;
	const char *line_start;
	const char *start;
	struct buf attr;
	unsigned char ch;
	int wrap_offset = 0; /*number of columns displayed in earlier lines */
	int in_tab = 0;
	int shift_left = atoi(pane_attr_get(focus, "shift_left") ?:"0");
	int wrap = shift_left < 0;
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
	const char *xypos = NULL;
	const char *ret_xypos = NULL;
	const char *xyattr = NULL;
	int want_xypos = 0;
	const char *cstart = NULL;
	struct xy xyscale = pane_scale(focus);
	int scale = xyscale.x;
	short cx = -1, cy = -1;

	if (!line)
		return Enoarg;
	start = line_start = line;

	if (strncmp(line, "<image:",7) == 0) {
		/* For now an <image> must be on a line by itself.
		 * Maybe this can be changed later if I decide on
		 * something that makes sense.
		 * The cursor is not on the image.
		 */
		y = render_image(p, focus, line, y, dodraw, scale);
		comm_call(comm2, "dimensions", p, p->w);
		p->cx = p->cy = -1;
		return 1;
	}

	update_line_height(p, focus, &line_height, &ascent, &twidth, &center,
			   line, scale);

	if (!wrap)
		x -= shift_left;
	else
		shift_left = 0;

	if (prefix) {
		const char *s = prefix + strlen(prefix);
		update_line_height_attr(p, focus, &line_height, &ascent, NULL,
					"bold", prefix, scale);
		draw_some(p, focus, &rlst, &x, prefix, &s, "bold",
			  0, -1, -1, scale);
		rd->prefix_len = x + shift_left;
	} else
		rd->prefix_len = 0;

	if (center == 1)
		x += (p->w - x - twidth) / 2;
	if (center > 1)
		x += center;
	if (center < 0)
		x = p->w - x - twidth + center;
	margin = x;

	comm_call(comm2, "dimensions", p,
		  0, NULL, NULL, line_height);

	buf_init(&attr);
	buf_append(&attr, ' '); attr.len = 0;

	/* If posx and posy are non-negative, set *offsetp to
	 * the length when we reach that cursor pos.
	 * if offset is non-negative, set posx and posy to cursor
	 * pos when we reach that length
	 */
	if (posx >= 0 && posy >= 0) {
		want_xypos = 1;
		free((void*)rd->xyattr);
		rd->xyattr = NULL;
	}
	if (posy >= 0 && posy < y) {
		/* cursor is not here */
		posx = posy = -1;
	}

	while (*line && y < p->h && !end_of_page) {
		int XPOS;

		if (mwidth <= 0) {
			struct call_return cr = home_call_ret(all, focus,
							      "text-size", p,
							      -1, NULL, "M",
							      scale, NULL,
							      buf_final(&attr));
			mwidth = cr.x;
			if (mwidth <= 0)
				mwidth = 1;
		}

		if (ret == XYPOS) {
			/* Found the cursor, stop looking */
			posy = -1; posx = -1;
		}
		if (y+line_height >= posy &&
		    y <= posy && x <= posx)
			XPOS = posx;
		else
			XPOS = -1;

		if (y > posy && want_xypos == 1 && xypos) {
			rd->xyattr = xyattr ? strdup(xyattr) : NULL;
			ret_xypos = xypos;
			want_xypos = 2;
		}

		if (offset >= 0 && start - line_start <= offset) {
			if (y >= 0 && (y == 0 || y + line_height <= p->h)) {
				/* Some chars result in multiple chars
				 * being drawn.  If they are on the same
				 * line, like spaces in a TAB, we leave
				 * cursor at the start.  If on different
				 * lines like "\" are e-o-l and char on
				 * next line, then leave cursor at first
				 * char on next line.
				 */
				if (cstart != start || y != cy) {
					cy = y;
					cx = x;
					cstart = start;
				}
			} else {
				cy = cx = -1;
			}
		}

		if ((ret == WRAP|| x >= p->w - mwidth) &&
		    (line[0] != '<' || line[1] == '<')) {
			/* No room for more text */
			if (wrap && *line && *line != '\n') {
				int len = flush_line(p, focus, dodraw, &rlst,
						     y+ascent, scale,
						     p->w - mwidth,
						     &xypos, &xyattr);
				wrap_offset += len;
				x -= len;
				if (x < 0)
					x = 0;
				y += line_height;
				if (want_xypos == 1) {
					if (y+line_height >= posy &&
					    y <= posy && x > posx) {
						/* cursor is in field move down */
						find_xypos(rlst, p, focus,
							   posx, scale,
							   &xypos, &xyattr);
					}
					if (xypos) {
						rd->xyattr = xyattr ?
							strdup(xyattr) : NULL;
						ret_xypos = xypos;
						want_xypos = 2;
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
			    (XPOS>x && (line - start)*mwidth > XPOS - x)) {
				ret = draw_some(p, focus, &rlst, &x, start,
						&line,
						buf_final(&attr),
						wrap ? mwidth : 0,
						offset - (start - line_start),
						XPOS, scale);
				start = line;
			}
			continue;
		}
		ret = draw_some(p, focus, &rlst, &x, start, &line,
				buf_final(&attr),
				wrap ? mwidth : 0,
				in_tab ?:offset - (start - line_start),
				XPOS, scale);
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
						XPOS, scale);
				if (ret)
					continue;
				start += 2;
				line = start;
			} else {
				const char *a = line;

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
			xypos = line-1;
			flush_line(p, focus, dodraw, &rlst, y+ascent, scale, 0,
				   &xypos, &xyattr);
			y += line_height;
			x = 0;
			wrap_offset = 0;
			start = line;
			if (xypos && want_xypos == 1) {
				rd->xyattr = xyattr ? strdup(xyattr) : NULL;
				ret_xypos = xypos;
				want_xypos = 2;
			}
		} else if (ch == '\f') {
			x = 0;
			start = line;
			wrap_offset = 0;
			end_of_page = 1;
		} else if (ch == '\t') {
			int xc = (wrap_offset + x) / mwidth;
			/* Note xc might be negative, so "xc % 8" won't work here */
			int w = 8 - (xc & 7);
			ret = draw_some(p, focus, &rlst, &x, start, &line,
					buf_final(&attr),
					wrap ? mwidth*2: 0,
					offset == (start - line_start)
					? in_tab : -1,
					XPOS, scale);
			if (w > 1) {
				line -= 1;
				in_tab = -1; // suppress extra cursors
			} else
				in_tab = 0;
			start = line;
		} else {
			char buf[4];
			const char *b;
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
					XPOS, scale);
			attr.len = l;
			start = line;
		}
	}
	if (!*line && (line > start || offset == start - line_start)) {
		/* Some more to draw */
		draw_some(p, focus, &rlst, &x, start, &line,
			  buf_final(&attr),
			  wrap ? mwidth : 0, offset - (start - line_start),
			  posx, scale);
	}

	flush_line(p, focus, dodraw, &rlst, y+ascent, scale, 0,
		   &xypos, &xyattr);

	if (want_xypos == 1 && xypos) {
		rd->xyattr = xyattr ? strdup(xyattr) : NULL;
		ret_xypos = xypos;
		want_xypos = 2;
	}

	if (offset >= 0 && line - line_start <= offset) {
		if (y >= 0 && (y == 0 || y + line_height <= p->h)) {
			if (cstart != start || cy != y) {
				cy = y;
				cx = x;
				cstart = start;
			}
		} else {
			cy = cx = -1;
		}
	}
	if (x > 0 || y == 0)
		/* No newline at the end .. but we must render as whole lines */
		y += line_height;
	free(buf_final(&attr));
	if (offset >= 0) {
		p->cx = cx;
		p->cy = cy;
	}
	pane_resize(p, p->x, p->y, margin + twidth, y);
	while (rlst) {
		struct render_list *r = rlst;
		rlst = r->next;
		free((void*)r->text);
		free((void*)r->attr);
		free(r);
	}
	if (want_xypos) {
		if (ret_xypos)
			return ret_xypos - line_start + 1;
		else
			return Efalse;
	} else
		return end_of_page ? 2 : 1;
}

DEF_CMD(renderline_get)
{
	struct rline_data *rd = ci->home->data;

	if (!ci->str)
		return 1;
	if (strcmp(ci->str, "prefix_len") == 0)
		return rd->prefix_len + 1;
	if (strcmp(ci->str, "xyattr") == 0)
		comm_call(ci->comm2, "xyattr", ci->focus, 0, NULL,
			  rd->xyattr);
	return 1;
}

DEF_CMD(renderline_close)
{
	struct rline_data *rd = ci->home->data;

	free((void*)rd->xyattr);
	rd->xyattr = NULL;
	return 1;
}

static struct map *rl_map;
DEF_LOOKUP_CMD(renderline_handle, rl_map);

DEF_CMD(renderline_attach)
{
	struct rline_data *rd;
	struct pane *p;

	if (!rl_map) {
		rl_map = key_alloc();
		key_add(rl_map, "render-line:draw", &renderline);
		key_add(rl_map, "render-line:measure", &renderline);
		key_add(rl_map, "render-line:get", &renderline_get);
		key_add(rl_map, "Close", &renderline_close);
		key_add(rl_map, "Free", &edlib_do_free);
	}

	alloc(rd, pane);
	p = pane_register(ci->focus, -1, &renderline_handle.c, rd);
	if (!p) {
		unalloc(rd, pane);
		return Efail;
	}
	return comm_call(ci->comm2, "cb", p);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &renderline_attach, 0, NULL,
		  "attach-renderline");
}
