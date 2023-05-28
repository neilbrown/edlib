/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
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

#define _GNU_SOURCE /*  for asprintf */
#include <stdio.h>
#include <ctype.h>
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
	short		curs_width;
	int		scale;
	const char	*line;
};

enum {
	OK = 0,
	WRAP,
	XYPOS,
};

static int draw_some(struct pane *p safe, struct pane *focus safe,
		     struct render_list **rlp safe,
		     int *x safe,
		     const char *start safe, const char **endp safe,
		     const char *attr safe, int margin, int cursorpos, int xpos,
		     int scale)
{
	/* Measure the text from 'start' to '*endp', expecting to
	 * draw to p[x,?].
	 * Update 'x' and 'endp' past what was drawn.
	 * Everything will be drawn with the same attributes: attr.
	 * If the text would get closer to right end than 'margin',
	 * we stop drawing before then.  If this happens, WRAP is returned.
	 * If drawing would pass xpos: stop there, record pointer
	 * into 'endp', and return XYPOS.
	 * If cursorpos is between 0 and len inclusive, a cursor is drawn there.
	 * Don't actually draw anything, just append a new entry to the
	 * render_list rlp.
	 */
	int len = *endp - start;
	char *str;
	struct call_return cr;
	int max;
	int ret = WRAP;
	int rmargin = p->w - margin;
	struct render_list *rl;

	if (cursorpos > len)
		cursorpos = -1;
	if (len == 0 && cursorpos < 0)
		/* Nothing to do */
		return OK;
	if ((*rlp == NULL ||
	     ((*rlp)->next == NULL && (*rlp)->text_orig == NULL)) &&
	    strstr(attr, ",wrap,") && cursorpos < 0)
		/* The text in a <wrap> marker that causes a wrap is
		 * suppressed unless the cursor is in it.
		 * This will only ever be at start of line.  <wrap> text
		 * elsewhere is not active.
		 */
		return OK;
	str = strndup(start, len);
	if (*str == '\t')
		/* TABs are only sent one at a time, and are rendered as space */
		*str = ' ';
	if (xpos >= 0 && xpos >= *x && xpos < rmargin) {
		/* reduce marking to given position, and record that
		 * as xypos when we hit it.
		 */
		rmargin = xpos;
		ret = XYPOS;
	}

	rl = calloc(1, sizeof(*rl));
	cr = home_call_ret(all, focus, "Draw:text-size", p,
			   rmargin - *x, NULL, str,
			   scale, NULL, attr);
	max = cr.i;
	if (max == 0 && ret == XYPOS) {
		/* Must have already reported XY position, don't do it again */
		rl->xypos = start;
		ret = WRAP;
		rmargin = p->w - margin;
		cr = home_call_ret(all, focus, "Draw:text-size", p,
				   rmargin - *x, NULL, str,
				   scale, NULL, attr);
		max = cr.i;
	}
	if (max < len) {
		/* It didn't all fit, so trim the string and
		 * try again.  It must fit this time.
		 * I don't know what we expect to be different the second time..
		 */
		str[max] = 0;
		cr = home_call_ret(all, focus, "Draw:text-size", p,
				   rmargin - *x, NULL, str,
				   scale, NULL, attr);
	}

	rl->text_orig = start;
	rl->text = str;
	rl->attr = strdup(attr);
	rl->width = cr.x;
	rl->x = *x;
	*x += rl->width;
	if (ret == XYPOS)
		rl->xypos = start + strlen(str);

	if (cursorpos >= 0 && cursorpos <= len && cursorpos <= max)
		rl->cursorpos = cursorpos;
	else
		rl->cursorpos = -1;
	while (*rlp)
		rlp = &(*rlp)->next;
	*rlp = rl;

	if (max >= len)
		return OK;
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
		i = strcspn(com, ",");
		return strndup(com, i);
	}
	return NULL;
}

static int flush_line(struct pane *p safe, struct pane *focus safe, int dodraw,
		      struct render_list **rlp safe,
		      int y, int scale, int wrap_pos, int *wrap_margin safe,
		      int *wrap_prefix_sizep,
		      const char **xypos, const char **xyattr)
{
	/* Flush a render_list returning x-space used.
	 * If wrap_pos is > 0, stop rendering before last entry with the
	 * "wrap" attribute; draw the wrap-tail at wrap_pos, and insert
	 * wrap_head into the render_list before the next line.
	 * If any render_list entry reports xypos, store that in xypos with
	 * matching attributes in xyattr.
	 */
	struct render_list *last_wrap = NULL, *end_wrap = NULL, *last_rl = NULL;
	int in_wrap = 0;
	int wrap_len = 0; /* length of text in final <wrap> section */
	int wrap_prefix_size = 0; /* wrap margin plus prefix */
	struct render_list *rl, *tofree;
	int x = 0;
	char *head;

	if (!*rlp)
		return 0;
	for (rl = *rlp; wrap_pos && rl; rl = rl->next) {
		if (strstr(rl->attr, ",wrap,") && rl != *rlp) {
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
		/* A wrap was found, so finish there */
		last_rl = last_wrap;

	for (rl = *rlp; rl && rl != last_wrap; rl = rl->next) {
		int cp = rl->cursorpos;

		if (!*wrap_margin && strstr(rl->attr, ",wrap-margin,"))
			*wrap_margin = rl->x;

		if (wrap_pos &&
		    cp >= (int)strlen(rl->text) + wrap_len)
			/* Don't want to place cursor at end of line before
			 * the wrap, only on the next line after the
			 * wrap.
			 */
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
	/* Draw the wrap text if it contains the cursor */
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
	/* Draw the wrap-tail */
	if (wrap_pos && last_rl && dodraw) {
		char *e = get_last_attr(last_rl->attr, "wrap-tail");
		home_call(focus, "Draw:text", p, -1, NULL, e ?: "\\",
			  scale, NULL, "underline,fg:blue",
			  wrap_pos, y);
		free(e);
	}

	tofree = *rlp;
	*rlp = end_wrap;

	/* Queue the wrap-head for the next flush */
	if (wrap_pos && last_rl) {
		head = get_last_attr(last_rl->attr, "wrap-head");
		if (head) {
			struct call_return cr =
				home_call_ret(all, focus, "Draw:text-size", p,
					      p->w, NULL, head,
					      scale, NULL, last_rl->attr);
			rl = calloc(1, sizeof(*rl));
			rl->text = head;
			rl->attr = strdup(last_rl->attr); // FIXME underline,fg:blue ???
			rl->width = cr.x;
			rl->x = *wrap_margin;
			rl->cursorpos = -1;
			rl->next = *rlp;
			*rlp = rl;
			/* 'x' is how much to shift-left remaining rl entries,
			 * Don't want to shift them over the wrap-head
			 */
			x -= cr.x;
			wrap_prefix_size += cr.x;
		}
		x -= *wrap_margin;
		wrap_prefix_size += *wrap_margin;
	}
	if (wrap_prefix_sizep)
		*wrap_prefix_sizep = wrap_prefix_size;

	for (rl = tofree; rl && rl != end_wrap; rl = tofree) {
		tofree = rl->next;
		free((void*)rl->text);
		free((void*)rl->attr);
		free(rl);
	}

	/* Shift remaining rl to the left */
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
	struct call_return cr = home_call_ret(all, focus, "Draw:text-size", p,
					      -1, NULL, str,
					      scale, NULL, attr);
	if (cr.y > *h)
		*h = cr.y;
	if (cr.i2 > *a)
		*a = cr.i2;
	if (w)
		*w += cr.x;
}

static void strip_ctrl(char *s safe)
{
	while (*s) {
		if (*s < ' ' || ((unsigned)*s >= 128 && (unsigned)*s < 128+' '))
			*s = 'M';
		s += 1;
	}
}

static void update_line_height(struct pane *p safe, struct pane *focus safe,
			       int *h safe, int *a safe,
			       int *w safe, int *center, const char *line safe,
			       int scale)
{
	/* Extract general geometry information from the line.
	 * Maximum height and ascent are extracted together with total
	 * with and h-alignment info:
	 * 0 - left, 1 = centered, >=2 = space-on-left, <=-2 = space from right
	 */
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
			strip_ctrl(l);
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
			const char *aend;

			/* attrs must not contain ",," */
			aend = strstr(st, ",,");
			if (aend)
				aend += 1;
			if (!aend || line < aend)
				aend = line;

			buf_concat_len(&attr, st, aend-st);
			/* Replace trailing '>' with ',', and append ','
			 * so ",," marks where to strip back to when we
			 * find </>.
			 */
			attr.b[attr.len-1] = ',';
			buf_append(&attr, ',');
			b = buf_final(&attr);
			if (center && strstr(b, ",center,"))
				*center = 1;
			if (center && (c2=strstr(b, ",left:")) != NULL)
				*center = 2 + atoi(c2+6) * scale / 1000;
			if (center && (c2=strstr(b, ",right:")) != NULL)
				*center = -2 - atoi(c2+7) * scale / 1000;
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
		strip_ctrl(l);
		update_line_height_attr(p, focus, h, a, w,
					buf_final(&attr), l, scale);
		free(l);
	}
	*h += above + below;
	*a += above;
	free(buf_final(&attr));
}

static void parse_map(const char *map safe, short *rowsp safe, short *colsp safe)
{
	/* The map must be a sequence of rows, each of which is
	 * a sequence of chars starting CAPS and continuing lower.
	 * Each row must be the same length.
	 */
	short cols = -1;
	short rows = 0;
	short this_cols = 0;

	for (; *map && isalpha(*map); map += 1) {
		if (isupper(*map)) {
			if (rows > 1)
				if (this_cols != cols)
					/* Rows aren't all the same */
					return;
			if (rows)
				cols = this_cols;

			this_cols = 1;
			rows += 1;
		} else if (rows == 0) {
			/* First row malformed */
			return;
		} else {
			this_cols += 1;
		}
	}
	if (this_cols != cols)
		/* Last row is wrong length */
		return;
	*rowsp = rows;
	*colsp = cols;
}

static int render_image(struct pane *p safe, struct pane *focus safe,
			const char *line safe,
			int dodraw, int scale,
			int offset, int want_xypos, short x, short y)
{
	char *fname = NULL;
	const char *orig_line = line;
	short width, height;
	short rows = -1, cols = -1;
	int map_offset = 0;
	int ioffset;
	char *ssize = attr_find(p->attrs, "cached-size");
	struct xy size= {-1, -1};

	width = p->parent->w/2;
	height = p->parent->h/2;

	while (*line == '<')
		line += 1;

	while (*line && *line != '>') {
		int len = strcspn(line, ",>");

		if (strncmp(line, "image:", 6) == 0) {
			fname = strndup(line+6, len-6);
			if (!ssize ||
			    sscanf(ssize, "%hdx%hd", &size.x, &size.y) != 2) {
				struct call_return cr =
					home_call_ret(all, focus,
						      "Draw:image-size",
						      p, 0, NULL, fname);
				if (cr.x > 0 && cr.y > 0) {
					size.x = cr.x;
					size.y = cr.y;
					asprintf(&ssize, "%hdx%hd",
						 cr.x, cr.y);
					attr_set_str(&p->attrs,
						     "cached-size", ssize);
				}
			}
		} else if (strncmp(line, "width:", 6) == 0) {
			width = atoi(line + 6);
			width = width * scale / 1000;
		} else if (strncmp(line, "height:", 7) == 0) {
			height = atoi(line + 7);
			height = height * scale / 1000;
		} else if (strncmp(line, "noupscale", 9) == 0 &&
			   fname && size.x > 0) {
			if (size.x < p->parent->w)
				width = size.x;
			if (size.y < p->parent->h)
				height = size.y;
		} else if ((offset >= 0 || want_xypos) &&
			   strncmp(line, "map:", 4) == 0) {
			/*
			 * A map is map:LxxxLxxxLxxxLxxx or similar
			 * Where each "Lxxx" recognised by a CAP followed
			 * by lower is a row, and each char is a column.
			 * So we count the CAPs to get row count, and
			 * count the chars to get col count.
			 * If want_xypos then map x,y ino that matrix
			 * and return pos in original line of cell.
			 * If offset is in the map, then set ->cx,->cy to
			 * the appropriate location.
			 */
			map_offset = line+4 - orig_line;
			parse_map(line+4, &rows, &cols);
		}
		line += len;
		line += strspn(line, ",");
	}
	pane_resize(p, (p->parent->w - width)/2, p->y, width, height);

	attr_set_int(&p->attrs, "line-height", p->h);

	/* Adjust size to be the scaled size - it must fit in
	 * p->w, p->h
	 */
	if (size.x * p->h > size.y * p->w) {
		/* Image is wider than space */
		size.y = size.y * p->w / size.x;
		size.x = p->w;
		ioffset = 0;
	} else {
		/* Image is taller than space */
		size.x = size.x * p->h / size.y;
		size.y = p->h;
		ioffset = (p->w - size.x) / 2;
	}

	p->cx = p->cy = -1;

	if (offset >= 0 && map_offset > 0 && rows > 0 &&
	    offset >= map_offset && offset < map_offset + (rows*cols)) {
		/* Place cursor based on where 'offset' is in the map */
		short r = (offset - map_offset) / cols;
		short c = offset - map_offset - r * cols;
		p->cx = size.x / cols * c + ioffset;
		p->cy = size.y / rows * r;
	}

	if (fname && dodraw)
		home_call(focus, "Draw:image", p, 5, NULL, fname,
			  0, NULL, NULL, cols, rows);

	free(fname);

	if (want_xypos && map_offset > 0 && rows > 0) {
		/* report where x,y is as a position in the map */
		short r = y * rows / size.y;
		short c = (x > ioffset ? x - ioffset : 0) * cols / size.x;
		if (c >= cols) c = cols - 1;
		/* +1 below because result must never be zero */
		return map_offset + r * cols + c + 1;
	}
	return 1;
}

static void set_xypos(struct render_list *rlst,
		      struct pane *p safe, struct pane *focus safe, int posx,
		      int scale)
{
	/* Find the text postition in the rlst which corresponds to
	 * the screen position posx, and report attribtes there too.
	 */
	for (; rlst && rlst->x <= posx; rlst = rlst->next) {
		if (rlst->x + rlst->width < posx)
			continue;

		if (rlst->x == posx)
			rlst->xypos = rlst->text_orig;
		else {
			struct call_return cr =
				home_call_ret(all, focus, "Draw:text-size", p,
					      posx - rlst->x, NULL, rlst->text,
					      scale, NULL, rlst->attr);
			rlst->xypos = rlst->text_orig + cr.i;
		}
	}
}

/* Render a line, with attributes and wrapping.
 * The marked-up text to be processed has already been provided with
 *   render-line:set.  It is in rd->line;
 * ->num is <0, or an index into ->str where the cursor is,
 *   and the x,y co-ords will be stored in p->cx,p->cy
 * If key is "render-line:draw", then send drawing commands, otherwise
 * just perform measurements.
 * If key is "render-line:findxy", then only measure until the position
 *   in x,y is reached, then return an index into ->str of where we reached.
 *   Store the attributes active at the time so they can be fetched later.
 */
DEF_CMD(renderline)
{
	struct pane *p = ci->home;
	struct rline_data *rd = p->data;
	struct pane *focus = ci->focus;
	const char *line = rd->line;
	int dodraw = strcmp(ci->key, "render-line:draw") == 0;
	short posx;
	short offset = ci->num;
	int x = 0;
	int y = 0;
	const char *line_start;
	const char *start;
	struct buf attr;
	unsigned char ch;
	int wrap_offset = 0; /*number of columns displayed in earlier lines,
			      * use for calculating TAB size. */
	int wrap_margin = 0; /* left margin for wrap - carried forward from
			      * line to line. */
	int in_tab = 0;
	int shift_left = atoi(pane_attr_get(focus, "shift_left") ?:"0");
	int wrap = shift_left < 0;
	char *prefix = pane_attr_get(focus, "prefix");
	int line_height = 0;
	int ascent = -1;
	int mwidth = -1;
	int ret = OK;
	int twidth = 0;
	int center = 0;
	int margin;
	int end_of_page = 0;
	struct render_list *rlst = NULL;
	const char *xypos = NULL;
	const char *ret_xypos = NULL;
	const char *xyattr = NULL;
	/* want_xypos becomes 2 when the pos is found */
	int want_xypos = strcmp(ci->key, "render-line:findxy") == 0;
	struct xy xyscale = pane_scale(focus);
	int scale = xyscale.x;
	short cx = -1, cy = -1;

	if (!line)
		return Enoarg;
	/* line_start doesn't change
	 * start is the start of the current segment(since attr)
	 *    is update after we call draw_some()
	 * line is where we are now up to.
	 */
	start = line_start = line;

	rd->scale = scale;

	if (dodraw)
		home_call(focus, "Draw:clear", p);

	if (strncmp(line, "<image:",7) == 0)
		/* For now an <image> must be on a line by itself.
		 * Maybe this can be changed later if I decide on
		 * something that makes sense.
		 */
		return render_image(p, focus, line, dodraw, scale,
				    offset, want_xypos, ci->x, ci->y);

	update_line_height(p, focus, &line_height, &ascent, &twidth, &center,
			   line, scale);

	if (line_height <= 0)
		return Einval;

	if (!wrap)
		x -= shift_left;
	else
		shift_left = 0;

	if (prefix) {
		const char *s = prefix + strlen(prefix);
		update_line_height_attr(p, focus, &line_height, &ascent, NULL,
					"bold", prefix, scale);
		draw_some(p, focus, &rlst, &x, prefix, &s, ",bold,",
			  0, -1, -1, scale);
		rd->prefix_len = x + shift_left;
	} else
		rd->prefix_len = 0;

	if (center == 1)
		x += (p->w - x - twidth) / 2;
	if (center >= 2)
		x += center - 2;
	if (center <= -2)
		x = p->w - x - twidth + (center + 2);
	/* tabs are measured against this margin */
	margin = x;

	/* The attr string starts and ends with ',' and
	 * attrs are separated by commas.
	 * Groups of attrs to be popped by the next </>
	 * are separated by ",,"
	 */
	buf_init(&attr);
	buf_append(&attr, ',');

	rd->curs_width = 0;

	/* If findxy was requested, ci->x and ci->y tells us
	 * what to look for, and we return index into line where this
	 * co-ordinate was reached.
	 * want_xypos will be set to 2 when we pass the co-ordinate
	 * At that time ret_xypos is set, to be used to provide return value.
	 * This might happen when y exceeds ypos, or we hit end-of-page.
	 */
	if (want_xypos) {
		free((void*)rd->xyattr);
		rd->xyattr = NULL;
	}

	while (*line && y < p->h && !end_of_page) {
		if (mwidth <= 0) {
			/* mwidth is recalculated whenever attrs change */
			struct call_return cr = home_call_ret(all, focus,
							      "Draw:text-size", p,
							      -1, NULL, "M",
							      scale, NULL,
							      buf_final(&attr));
			mwidth = cr.x;
			if (mwidth <= 0)
				mwidth = 1;
			if (!rd->curs_width)
				rd->curs_width = mwidth;
		}

		if (want_xypos == 1 &&
		    y > ci->y - line_height &&
		    y <= ci->y)
			posx = ci->x;
		else
			posx = -1;

		if (want_xypos == 1 && xypos) {
			rd->xyattr = xyattr ? strdup(xyattr) : NULL;
			ret_xypos = xypos;
			want_xypos = 2;
		}

		if (offset >= 0 && start - line_start <= offset) {
			if (y >= 0 && (y == 0 || y + line_height <= p->h)) {
				/* Don't update cursor pos while in a TAB
				 * as we want to leave cursor at the start.
				 */
				if (!in_tab) {
					cy = y;
					cx = x;
				}
			} else {
				cy = cx = -1;
			}
		}

		if ((ret == WRAP || x >= p->w - mwidth) &&
		    (line[0] != '<' || line[1] == '<')) {
			/* No room for more text */
			if (wrap && *line && *line != '\n') {
				int wrap_prefix_size;
				int len = flush_line(p, focus, dodraw, &rlst,
						     y+ascent, scale,
						     p->w - mwidth, &wrap_margin,
						     &wrap_prefix_size,
						     &xypos, &xyattr);
				if (len + wrap_prefix_size <= cx && cy == y) {
					cx -= len;
					cy += line_height;
				}
				wrap_offset += len;
				x -= len;
				if (x < 0)
					x = 0;
				y += line_height;
				if (want_xypos == 1 &&
				    y >= ci->y - line_height &&
				    y <= ci->y)
					/* cursor is in the tail of rlst that
					 * was relocated - reassess xypos
					 */
					set_xypos(rlst, p, focus, ci->x, scale);
			} else {
				/* truncate: skip over normal text, but
				 * stop at newline.
				 */
				line += strcspn(line, "\n");
				start = line;
			}
		}

		ret = OK;
		ch = *line;
		if (line == line_start + offset)
			rd->curs_width = mwidth;
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
			    (line-start) * mwidth >= p->w - x ||
			    (posx > x && (line - start)*mwidth > posx - x)) {
				ret = draw_some(p, focus, &rlst, &x, start,
						&line,
						buf_final(&attr),
						wrap ? mwidth : 0,
						offset - (start - line_start),
						posx, scale);
				start = line;
			}
			continue;
		}
		ret = draw_some(p, focus, &rlst, &x, start, &line,
				buf_final(&attr),
				wrap ? mwidth : 0,
				in_tab ?:offset - (start - line_start),
				posx, scale);
		start = line;
		if (ret != OK || !ch)
			continue;
		if (ch == '<') {
			line += 1;
			if (*line == '<') {
				ret = draw_some(p, focus, &rlst, &x, start, &line,
						buf_final(&attr),
						wrap ? mwidth : 0,
						in_tab ?:offset - (start - line_start),
						posx, scale);
				if (ret != OK)
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
					const char *aend;

					/* attrs must not contain ",," */
					aend = strstr(a, ",,");
					if (aend)
						aend += 1;
					if (!aend || line < aend)
						aend = line;

					buf_concat_len(&attr, a, aend-a);
					/* Replace trailing '>' with ',', and
					 * append ',' so ",," marks where to
					 * strip back to when we find </>.
					 */
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
				   &wrap_margin, NULL, &xypos, &xyattr);
			y += line_height;
			x = 0;
			wrap_offset = 0;
			start = line;
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
					posx, scale);
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
			buf_concat(&attr, ",underline,fg:red,");
			ret = draw_some(p, focus, &rlst, &x, buf, &b,
					buf_final(&attr),
					wrap ? mwidth*2: 0,
					offset - (start - line_start),
					posx, scale);
			attr.len = l;
			start = line;
		}
	}
	if (!*line && (line > start || offset == start - line_start)) {
		/* Some more to draw */
		if (want_xypos == 1 &&
		    y > ci->y - line_height &&
		    y <= ci->y)
			posx = ci->x;
		else
			posx = -1;

		draw_some(p, focus, &rlst, &x, start, &line,
			  buf_final(&attr),
			  wrap ? mwidth : 0, offset - (start - line_start),
			  posx, scale);
	}

	flush_line(p, focus, dodraw, &rlst, y+ascent, scale, 0,
		   &wrap_margin, NULL,  &xypos, &xyattr);

	if (want_xypos == 1) {
		rd->xyattr = xyattr ? strdup(xyattr) : NULL;
		ret_xypos = xypos ?: line;
		want_xypos = 2;
	}

	if (offset >= 0 && line - line_start <= offset) {
		if (y >= 0 && (y == 0 || y + line_height <= p->h)) {
			cy = y;
			cx = x;
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
	if (!dodraw)
		/* Mustn't resize after clearing the pane, or we'll
		 * be out-of-sync with display manager.
		 */
		pane_resize(p, p->x, p->y, p->w, y);
	attr_set_int(&p->attrs, "line-height", line_height);
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
			return 1;
	} else
		return end_of_page ? 2 : 1;
}

DEF_CMD(renderline_get)
{
	struct rline_data *rd = ci->home->data;
	char buf[20];
	const char *val = buf;

	if (!ci->str)
		return Enoarg;
	if (strcmp(ci->str, "prefix_len") == 0)
		snprintf(buf, sizeof(buf), "%d", rd->prefix_len);
	else if (strcmp(ci->str, "curs_width") == 0)
		snprintf(buf, sizeof(buf), "%d", rd->curs_width);
	else if (strcmp(ci->str, "xyattr") == 0)
		val = rd->xyattr;
	else
		return Einval;

	comm_call(ci->comm2, "attr", ci->focus, 0, NULL, val);
	return 1;
}

DEF_CMD(renderline_set)
{
	struct rline_data *rd = ci->home->data;
	const char *old = rd->line;
	struct xy xyscale = pane_scale(ci->focus);

	if (ci->str)
		rd->line = strdup(ci->str);
	else
		rd->line = NULL;
	if (strcmp(rd->line ?:"", old ?:"") != 0 ||
	    (old && xyscale.x != rd->scale)) {
		pane_damaged(ci->home, DAMAGED_REFRESH);
		pane_damaged(ci->home->parent, DAMAGED_REFRESH);
	}
	free((void*)old);
	ci->home->damaged &= ~DAMAGED_VIEW;
	return 1;
}

DEF_CMD(renderline_close)
{
	struct rline_data *rd = ci->home->data;

	free((void*)rd->xyattr);
	free((void*)rd->line);
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
		key_add(rl_map, "render-line:findxy", &renderline);
		key_add(rl_map, "get-attr", &renderline_get);
		key_add(rl_map, "render-line:set", &renderline_set);
		key_add(rl_map, "Close", &renderline_close);
		key_add(rl_map, "Free", &edlib_do_free);
	}

	alloc(rd, pane);
	p = pane_register(ci->focus, -10, &renderline_handle.c, rd);
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
