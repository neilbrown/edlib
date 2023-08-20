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
 * the size of the text.  Other panes can use renderline for similar
 * purposes.  messageline uses just one renderline.
 *
 * A renderline pane can sit in the normal stack and receive Refresh
 * messages to trigger drawing, or can sit "beside" the stack with a negative
 * 'z' value. In that can the owner need to explicitly request refresh.
 *
 * "render-line:set" will set the content of the line
 * "render-line:measure" will determine layout and size given the available
 *    width and other parameters
 * "render-line:draw" will send drawing commands.
 * "Refresh" does both the measure and the draw.
 */

#define _GNU_SOURCE /*  for asprintf */
#include <stdio.h>
#include <ctype.h>
#include <wctype.h>
#include <stdint.h>

#define PANE_DATA_TYPE struct rline_data
#include "core.h"
#include "misc.h"

/* There is one render_item entry for
 * - each string of text with all the same attributes
 * - each individual TAB
 * - each unknown control character
 * - the \n \f or \0 which ends the line
 * When word-wrap is enabled, strings of spaces get
 * different attributes, so a different render_item entry.
 *
 * attributes understood at this level are:
 *  left:nn		- left margin - in "points" (10 points per char normally)
 *  right:nn		- right margin
 *  tab:nn		- move to nn from left margin or -nn from right margin
 *  rtab		- from here to next tab or eol right-aligned
 *  center or centre	- equal space inserted here and before next
 *  or ctab		  tab-stop or margin
 *  space-above:nn	- extra space before (wrapped) line
 *  space-below:nn	- extra space after (wrapped) line
 *  height:nn		- override height.  This effectively adds space above
 *			  every individual line if the whole line is wrapped
 *  wrap		- text with this attr can be hidden when used as a wrap
 *			  point.  Not hidden if cursor in the region.
 *  wrap-margin		- remember this x offset as left margin of wrapped lines
 *  wrap-head=xx	- text is inserted at start of line when wrapped
 *  wrap-tail=xx	- text to include at end of line when wrapped.  This
 *			  determines how far before right margin the wrap is
 *			  triggered.
 *  wrap-XXXX		- attrs to apply to wrap head/tail. Anything not
 *			  recognised has "wrap-" stripped and is used for the
 *			  head and tail. Default is fg:blue,underline
 *  hide		- Text is hidden if cursor is not within range.
 *			  NOT YET IMPLEMENTED
 *
 * "nn" is measured in "points" which is 1/10 the nominal width of chars
 * in the default font size, which is called "10".  A positive value is
 * measured from the left margin or, which setting margins, from the
 * relevant page edge.  A negative value is measures from the right margin.
 *
 */

/* When an entry is split for line-wrap:
 *   'split_cnt' is count of splits (total lines - 1)
 *   ' split_list' is offsets from start where split happens
 *   'x' position of wrapped portions is wrap_marign or head_length
 *   'y' position of wrapped portions increases line_height for each
 */
struct render_item {
	struct render_item *next;
	const char	*attr safe;
	unsigned short	*split_list;
	unsigned short	start, len; // in rd->line
	unsigned short	height, width;
	signed short	x,y; /* signed so x can go negative when shifted */
	signed short	tab;
	unsigned short	wrap_x;	/* If this item wraps, wrap_x is the margin */
	uint8_t		split_cnt; /* Wrap happens this many times at byte
				    * positions in split_list */
	uint8_t		wrap;	/* This and consecutive render_items
				 * with the same wrap number form an
				 * optional wrap point.  It is only visible
				 * when not wrapped, or when cursor is in
				 * it.
				 */
	uint8_t		hide;	/* This and consecutive render_items
				 * with the same hide nmber form a
				 * hidden extent which is visible when
				 * the cursor is in it.
				 */
	bool		wrap_margin:1;
	bool		hidden:1;
	bool		eol:1;
	unsigned int	tab_cols:4; /* For \t char */
	enum tab_align {
		TAB_LEFT = 0,	// No extra space (after tab stop)
		TAB_RIGHT,	// Add extra space here so there no space at
				// next tab stop or margin
		TAB_CENTRE,	// Add extra space here, half of what TAB_RIGHT
				// would insert
	}		tab_align:2;
};
/* A "tab" value of 0 means left margin, and negative is measured from right
 * margin, so we need some other value to say "no value here"
 */
static const short TAB_UNSET = (1<<(14-2));

struct rline_data {
	unsigned short	prefix_bytes, prefix_pixels;
	short		curs_width;
	short		left_margin, right_margin;
	short		space_above, space_below;
	unsigned short	line_height, min_height;
	unsigned short	scale;
	unsigned short	width;
	unsigned short	ascent;
	char		*wrap_head, *wrap_tail, *wrap_attr;
	int		head_length, tail_length;
	char		*line safe;
	bool		word_wrap;
	bool		image;
	int		curspos;

	/* These used to check is measuring is needed, or to record
	 * results of last measurement */
	unsigned short measure_width, measure_height;
	short measure_offset, measure_shift_left;
	struct render_item *content;
};
#include "core-pane.h"

static void aappend(struct buf *b safe, char const *a safe)
{
	const char *end = a;
	while (*end >= ' ' && *end != ',')
		end++;
	buf_concat_len(b, a, end-a);
	buf_append(b, ',');
}

static void add_render(struct rline_data *rd safe,
		       struct render_item **safe*rip safe,
		       const char *start safe, const char *end safe,
		       char *attr safe,
		       short *tab safe, enum tab_align *align safe,
		       bool *wrap_margin safe,
		       short wrap, short hide)
{
	struct render_item *ri;
	struct render_item **riend = *rip;

	alloc(ri, pane);
	ri->attr = strdup(attr);
	ri->start = start - rd->line;
	ri->len = end - start;
	ri->tab_align = *align;
	ri->tab = *tab;
	ri->wrap = wrap;
	ri->hide = hide;
	ri->wrap_margin = *wrap_margin;
	ri->eol = !!strchr("\n\f\0", *start);
	*tab = TAB_UNSET;
	*align = TAB_LEFT;
	*wrap_margin = False;
	*riend = ri;
	riend = &ri->next;
	*rip = riend;
}

static void parse_line(struct rline_data *rd safe)
{
	/* Parse out markup in line into a renderlist with
	 * global content directly in rd.
	 */
	struct buf attr, wrapattr;
	struct render_item *ri = NULL, **riend = &ri;
	const char *line = rd->line;
	bool wrap_margin = False;
	short tab = TAB_UNSET;
	enum tab_align align = TAB_LEFT;
	int hide = 0, hide_num = 0, hide_depth = 0;
	int wrap = 0, wrap_num = 0, wrap_depth = 0;
	unsigned char c;

	rd->left_margin = rd->right_margin = 0;
	rd->space_above = rd->space_below = 0;
	rd->min_height = 0;
	aupdate(&rd->wrap_head, NULL);
	aupdate(&rd->wrap_tail, NULL);
	aupdate(&rd->wrap_attr, NULL);

	if (!line) {
		rd->image = False;
		return;
	}
	rd->image = strstarts(line, SOH "image:");
	if (rd->image)
		return;
	buf_init(&attr);
	buf_init(&wrapattr);

	ri = rd->content;
	rd->content = NULL;
	rd->measure_width = 0; // force re-measure
	while (ri) {
		struct render_item *r = ri;
		ri = r->next;
		free(r->split_list);
		unalloc_str_safe(r->attr, pane);
		unalloc(r, pane);
	}

	do {
		const char *st = line;
		c = *line++;

		while (c >= ' ' &&
		       (!rd->word_wrap || c != ' '))
			c = *line++;

		if (line - 1 > st || tab != TAB_UNSET) {
			/* All text from st to line-1 has "attr' */
			add_render(rd, &riend, st, line-1, buf_final(&attr),
				   &tab, &align, &wrap_margin, wrap, hide);
			st = line - 1;
		}
		switch (c) {
		case soh: {
			int old_len;
			const char *a, *v;
			st = line;
			/* Move 'line' over the attrs */
			while (*line && line[-1] != stx)
				line += 1;

			/* A set of attrs begins and ends with ',' so that
			 * ",," separates sets of attrs
			 * An empty set will be precisely 1 ','.  We strip
			 * "attr," as long as we can, then strip one more ',',
			 * which should leave either a trailing comma, or an
			 * empty string.
			 */
			buf_append(&attr, ',');
			old_len = attr.len;
			foreach_attr(a, v, st, line) {
				if (amatch(a, "centre") || amatch(a, "center") ||
				    amatch(a, "ctab")) {
					if (v)
						tab = anum(v);
					align = TAB_CENTRE;
				} else if (amatch(a, "tab") && v) {
					tab = anum(v);
					align = TAB_LEFT;
				} else if (amatch(a, "rtab")) {
					align = TAB_RIGHT;
				} else if (amatch(a, "left") && v) {
					rd->left_margin = anum(v);
				} else if (amatch(a, "right") && v) {
					rd->right_margin = anum(v);
				} else if (amatch(a, "space-above") && v) {
					rd->space_above = anum(v);
				} else if (amatch(a, "space-below") && v) {
					rd->space_below = anum(v);
				} else if (amatch(a, "height") && v) {
					rd->min_height = anum(v);
				} else if (amatch(a, "wrap")) {
					wrap = ++wrap_num;
					wrap_depth = old_len;
				} else if (amatch(a, "wrap-margin")) {
					wrap_margin = True;
				} else if (amatch(a, "wrap-head")) {
					aupdate(&rd->wrap_head, v);
				} else if (amatch(a, "wrap-tail")) {
					aupdate(&rd->wrap_tail, v);
				} else if (aprefix(a, "wrap-")) {
					aappend(&wrapattr, a+5);
				} else if (amatch(a, "word-wrap")) {
					if (!v || *v == '1')
						rd->word_wrap = 1;
					else if (*v == '0')
						rd->word_wrap = 0;
				} else if (amatch(a, "hide")) {
					hide = ++hide_num;
					hide_depth = old_len;
				} else
					aappend(&attr, a);
			}
			break;
			}
		case etx:
			/* strip last set of attrs */
			while (attr.len >= 2 &&
			attr.b[attr.len-1] == ',' &&
			attr.b[attr.len-2] != ',') {
				/* strip this attr */
				attr.len -= 2;
				while (attr.len && attr.b[attr.len-1] != ',')
					attr.len -= 1;
			}
			/* strip one more ',' */
			if (attr.len > 0)
				attr.len -= 1;
			if (attr.len <= wrap_depth)
				wrap = 0;
			if (attr.len <= hide_depth)
				hide = 0;
			break;
		case ack:
			/* Just ignore this */
			break;
		case ' ':
			/* This and following spaces are wrappable */
			st = line;
			while (*line == ' ')
				line += 1;
			wrap = ++wrap_num;
			add_render(rd, &riend, st - 1, line, buf_final(&attr),
				   &tab, &align, &wrap_margin, wrap, hide);
			wrap = 0;
			break;
		case '\0':
		case '\n':
		case '\f':
		case '\t':
		default:
			/* Each tab gets an entry of its own, as does any
			 * stray control character.
			 * \f \n and even \0 do.  These are needed for
			 * easy cursor placement.
			 */
			add_render(rd, &riend, st, line, buf_final(&attr),
				   &tab, &align, &wrap_margin, wrap, hide);
			break;
		}
	} while (c);

	rd->content = ri;
	free(attr.b);
	if (buf_final(&wrapattr)[0])
		rd->wrap_attr = buf_final(&wrapattr);
	else {
		free(wrapattr.b);
		rd->wrap_attr = strdup(",fg:blue,underline,");
	}
}

static inline struct call_return do_measure(struct pane *p safe,
					    struct render_item *ri safe,
					    int splitpos, int len,
					    int maxwidth)
{
	struct rline_data *rd = &p->data;
	struct call_return cr;
	char tb[] = "        ";
	char *str = rd->line + ri->start + splitpos;
	char tmp;

	if (ri->len && rd->line[ri->start] == '\t') {
		str = tb;
		if (len < 0)
			len = ri->tab_cols - splitpos;
	} else
		if (len < 0)
			len = ri->len - splitpos;
	tmp = str[len];
	str[len] = 0;

	cr = call_ret(all, "Draw:text-size", p,
		      maxwidth, NULL, str,
		      rd->scale, NULL, ri->attr);

	str[len] = tmp;
	if (cr.ret == 1 && maxwidth >= 0 &&
	    cr.i >= len)
		/* All fits in maxwidth */
		cr.ret = 2;
	/* Report position in rd->line */
	if (str == tb) {
		cr.s = rd->line + ri->start;
		if (splitpos + cr.i >= ri->tab_cols)
			cr.s += 1;
	} else
		cr.s = str + cr.i;
	return cr;
}

static inline struct call_return measure_str(struct pane *p safe,
					     char *str safe,
					     const char *attr)
{
	struct rline_data *rd = &p->data;

	return call_ret(all, "Draw:text-size", p,
			-1, NULL, str,
			rd->scale, NULL, attr);
}

static inline void do_draw(struct pane *p safe,
			   struct pane *focus safe,
			   struct render_item *ri safe, int split,
			   int offset,
			   int x, int y)
{
	struct rline_data *rd = &p->data;
	char tmp;
	char *str;
	int len;
	char tb[] = "         ";

	str = rd->line + ri->start;
	len = ri->len;

	y += rd->ascent;
	if (ri->len && strchr("\f\n\0", str[0])) {
		/* end marker - len extends past end of string,
		 * but mustn't write there.  Only need to draw if
		 * cursor is here.
		 */
		if (offset == 0)
			home_call(focus, "Draw:text", p, offset, NULL, "",
				  rd->scale, NULL, ri->attr, x, y);
		return;
	}
	if (ri->len && str[0] == '\t') {
		len = ri->tab_cols;
		if (split)
			offset = -1;
	}
	if (ri->split_list) {
		if (split < ri->split_cnt)
			len = ri->split_list[split];
		if (split)
			len -= ri->split_list[split-1];
	}

	if (ri->len && str[0] == '\t')
		/* Tab need a list of spaces */
		str = tb;
	else
		if (split > 0 && split <= ri->split_cnt && ri->split_list) {
			str += ri->split_list[split-1];
			offset -= ri->split_list[split-1];
		}

	tmp = str[len];
	str[len] = 0;

	if (offset >= len)
		offset = -1;
	home_call(focus, "Draw:text", p, offset, NULL, str,
			   rd->scale, NULL, ri->attr, x, y);
	str[len] = tmp;
}

static inline void draw_wrap(struct pane *p safe,
			     struct pane *focus safe,
			     char *str safe,
			     int x, int y)
{
	struct rline_data *rd = &p->data;

	home_call(focus, "Draw:text", p,
		  -1, NULL, str,
		  rd->scale, NULL, rd->wrap_attr,
		  x, y + rd->ascent);
}

static bool add_split(struct render_item *ri safe, int split)
{
	int i = ri->split_cnt;
	if (i > 250)
		return False;
	ri->split_cnt += 1;
	ri->split_list = realloc(ri->split_list,
				 sizeof(ri->split_list[0]) * ri->split_cnt);
	ri->split_list[i] = split;
	return True;
}

static int calc_pos(int num, int margin, int width)
{
	if (num >= 0)
		return num * width / 10;
	if (-num * width / 10 > margin)
		return 0;
	return margin + num * width / 10;
}

static int measure_line(struct pane *p safe, struct pane *focus safe, int offset)
{
	/* First measure each render_item entry setting
	 * height, ascent, width.
	 * Then use that with tab information to set 'x' position for
	 * each unit.
	 * Finally identify line-break locations if needed and set 'y'
	 * positions
	 *
	 * Return 1 if there is an EOL ('\n')
	 * 2 if there is an end-of-page ('\f')
	 * 3 if both.
	 * 0 if neither
	 */
	struct rline_data *rd = &p->data;
	struct render_item *ri, *wraprl;
	int shift_left = pane_attr_get_int(focus, "shift_left", 0);
	bool wrap = shift_left < 0;
	int wrap_margin;
	int right_margin;
	int left_margin;
	struct xy xyscale = pane_scale(focus);
	int curs_height;
	int xdiff, ydiff;
	struct call_return cr;
	int x, y;
	int ret = 0;
	bool seen_rtab = False;

	if (!rd->content)
		return ret;
	if (xyscale.x == rd->scale && p->w == rd->measure_width &&
	    shift_left == rd->measure_shift_left &&
	    offset == rd->measure_offset) {
		/* No change */
		for (ri = rd->content ; ri ; ri = ri->next) {
			if (ri->eol && rd->line[ri->start] == '\n')
				ret |= 1;
			if (ri->eol && rd->line[ri->start] == '\f')
				ret |= 2;
		}
		pane_resize(p, p->x, p->y, p->w, rd->measure_height);
		return ret;
	}
	rd->scale = xyscale.x;
	rd->measure_width = p->w;
	rd->measure_offset = offset;
	rd->measure_shift_left = shift_left;

	cr = measure_str(p, "M", "");
	rd->curs_width = cr.x;
	curs_height = cr.y;
	rd->line_height = cr.y;
	rd->ascent = cr.i2;
	if (rd->min_height > 10)
		rd->line_height = rd->line_height * rd->min_height / 10;

	if (rd->wrap_head) {
		cr = measure_str(p, rd->wrap_head, rd->wrap_attr);
		rd->head_length = cr.x;
	}
	cr = measure_str(p, rd->wrap_tail ?: "\\", rd->wrap_attr);
	rd->tail_length = cr.x;

	left_margin = calc_pos(rd->left_margin, p->w, rd->curs_width);
	/* 0 means right edge for right_margin, and left edge for all others */
	right_margin = p->w - calc_pos(-rd->right_margin, p->w, rd->curs_width);

	for (ri = rd->content; ri; ri = ri->next) {
		if (ri->len == 0 ||
		    (unsigned char)rd->line[ri->start] >= ' ') {
			cr = do_measure(p, ri, 0, -1, -1);
		} else {
			char tmp[4];
			if (ri->eol) {
				/* Ensure attributes of newline add to line
				 * height. The width will be ignored. */
				strcpy(tmp, "M");
				if (rd->line[ri->start] == '\n')
					ret |= 1;
				if (rd->line[ri->start] == '\f')
					ret |= 2;
			} else if (rd->line[ri->start] == '\t') {
				strcpy(tmp, " ");
			} else {
				strcpy(tmp, "^x");
				tmp[1] = '@' + (rd->line[ri->start] & 31);
			}
			cr = measure_str(p, tmp, ri->attr);
		}

		if (cr.y > rd->line_height)
			rd->line_height = cr.y;
		ri->height = cr.y;
		if (cr.i2 > rd->ascent)
			rd->ascent = cr.i2;
		ri->width = ri->eol ? 0 : cr.x;
		ri->hidden = False;

		if (ri->start <= offset && offset <= ri->start + ri->len) {
			cr = measure_str(p, "M", ri->attr);
			rd->curs_width = cr.x;
		}

		ri->split_cnt = 0;
		free(ri->split_list);
		ri->split_list = NULL;
	}
	/* Set 'x' position honouring tab stops, and set length
	 * of "\t" characters.  Also handle \n and \f.
	 */
	x = left_margin - (shift_left > 0 ? shift_left : 0);
	y = rd->space_above * curs_height / 10;
	rd->width = 0;
	for (ri = rd->content; ri; ri = ri->next) {
		int w, margin;
		struct render_item *ri2;
		ri->y = y;
		if (ri->tab != TAB_UNSET)
			x =  left_margin + calc_pos(ri->tab,
						    right_margin - left_margin,
						    rd->curs_width);
		if (ri->eol) {
			/* EOL */
			if (x > rd->width)
				rd->width = x;
			ri->x = x;
			x = 0; /* Don't include shift. probably not margin */
			if (rd->line[ri->start])
				y += rd->line_height;
			continue;
		}
		if (ri->tab_align == TAB_LEFT) {
			ri->x = x;
			if (ri->len && rd->line[ri->start] == '\t') {
				int col = x / ri->width;
				int cols= 8 - (col % 8);
				ri->tab_cols = cols;
				ri->width *= cols;
			}
			x += ri->width;
			continue;
		}
		if (ri->tab_align == TAB_RIGHT)
			seen_rtab = True;
		w = ri->width;
		for (ri2 = ri->next;
		     ri2 && ri2->tab_align == TAB_LEFT && ri2->tab == TAB_UNSET;
		     ri2 = ri2->next)
			w += ri2->width;
		while (ri2 && ri2->tab == TAB_UNSET)
			ri2 = ri2->next;
		margin = right_margin - left_margin;
		if (ri2)
			margin =  left_margin + calc_pos(ri2->tab,
							 right_margin - left_margin,
							 rd->curs_width);
		if (ri->tab_align == TAB_RIGHT)
			x = x + margin - x - w;
		else
			x = x + (margin - x - w) / 2;
		ri->x = x;
		while (ri->next && ri->next->next && ri->next->tab_align == TAB_LEFT) {
			x += ri->width;
			ri = ri->next;
			ri->x = x;
			ri->y = y;
		}
	}

	/* Now we check to see if the line needs to be wrapped and
	 * if so, adjust some y values and reduce x.  If we need to
	 * split an individual entry we create an array of split points.
	 */
	xdiff = 0; ydiff = 0; y = 0;
	wraprl = NULL;
	wrap_margin = left_margin + rd->head_length;
	for (ri = rd->content ; wrap && ri ; ri = ri->next) {
		int splitpos;
		if (ri->wrap && (wraprl == NULL || ri->wrap != wraprl->wrap))
			wraprl = ri;
		if (ri->wrap_margin)
			wrap_margin = ri->x + xdiff;
		ri->wrap_x = wrap_margin;
		ri->x += xdiff;
		ri->y += ydiff;
		y = ri->y;
		if (ri->eol) {
			xdiff = 0;
			continue;
		}
		if (ri->x + ri->width <= right_margin - rd->tail_length)
			continue;
		if ((ri->next == NULL || ri->next->eol) &&
		    ri->x + ri->width <= right_margin &&
		    seen_rtab)
			/* Don't need any tail space for last item.
			 * This allows rtab to fully right-justify,
			 * but leaves no-where for the cursor.  So
			 * only do it if rtab is present.
			 */
			continue;
		/* This doesn't fit here */
		if (wraprl) {
			/* Move wraprl to next line and hide it unless it contains cursor */
			int xd;
			struct render_item *wraprl2, *ri2;

			/* Find last ritem in wrap region.*/
			for (wraprl2 = wraprl ;
			     wraprl2->next && wraprl2->next->wrap == wraprl->wrap ;
			     wraprl2 = wraprl2->next)
				;
			wrap_margin = wraprl2->wrap_x;
			if (wraprl2->next) {
				xd = wraprl2->next->x - wrap_margin;
				if (wraprl2->next->start > ri->start)
					xd += xdiff;
			} else {
				xd = wraprl2->x - wrap_margin;
				if (wraprl2->start > ri->start)
					xd += xdiff;
			}
			if (offset >= 0 &&
			    offset >= wraprl->start &&
			    offset <= wraprl2->start + wraprl2->len) {
				/* Cursor is here, so keep it visible.
				 * If we are still in the wrap region, pretend
				 * it didn't exist, else move first item
				 * after it to next line
				 */
				if (ri->wrap == wraprl->wrap)
					goto normal_wrap;
			} else {
				/* Hide the wrap region */
				while (wraprl != wraprl2) {
					wraprl->hidden = True;
					wraprl = wraprl->next;
				}
				if (wraprl)
					wraprl->hidden = True;
				while (ri->next && ri->next->hidden)
					ri = ri->next;
			}
			for (ri2 = wraprl2->next ; ri2 && ri2 != ri->next; ri2 = ri2->next) {
				ri2->y += rd->line_height;
				ri2->x -= xd;
				if (ri2->wrap_margin)
					wrap_margin = ri2->x;
				ri2->wrap_x = wrap_margin;
			}
			xdiff -= xd;
			ydiff += rd->line_height;
			wraprl = NULL;
			if (ri->hidden ||
			    ri->x + ri->width <= right_margin - rd->tail_length)
				continue;
		}
	normal_wrap:
		/* Might need to split this ri into two or more pieces */
		x = ri->x;
		splitpos = 0;
		while (1) {
			cr = do_measure(p, ri, splitpos, -1,
					right_margin - rd->tail_length - x);
			if (cr.ret == 2)
				/* Remainder fits now */
				break;
			if (cr.i == 0 && splitpos == 0) {
				/* None of this fits here, move to next line */
				xdiff -= ri->x - wrap_margin;
				ri->x = wrap_margin;
				x = ri->x;
				ydiff += rd->line_height;
				ri->y += rd->line_height;
				wraprl = NULL;
			}
			if (cr.i == 0)
				/* Nothing fits and we already split - give up */
				break;
			/* re-measure the first part */
			cr = do_measure(p, ri, splitpos,
					cr.i,
					right_margin - rd->tail_length - x);
			ydiff += rd->line_height;
			xdiff -= cr.x;
			if (splitpos == 0) {
				xdiff -= ri->x - wrap_margin;
				x = wrap_margin;
			}
			splitpos += cr.i;
			if (!add_split(ri, splitpos))
				break;
		}
	}
	rd->measure_height =
		(rd->space_above + rd->space_below) * curs_height / 10 +
		ydiff + rd->line_height;
	pane_resize(p, p->x, p->y, p->w, rd->measure_height);
	attr_set_int(&p->attrs, "line-height", rd->line_height);
	return ret;
}

static void draw_line(struct pane *p safe, struct pane *focus safe, int offset)
{
	struct rline_data *rd = &p->data;
	struct render_item *ri;
	char *wrap_tail = rd->wrap_tail ?: "\\";
	char *wrap_head = rd->wrap_head ?: "";

	home_call(focus, "Draw:clear", p);

	if (!rd->content)
		return;
	for (ri = rd->content ; ri; ri = ri->next) {
		int split = 0;
		short y = ri->y;
		int cpos;

		if (ri->hidden)
			continue;
		if (offset < 0 || offset >= ri->start + ri->len)
			cpos = -1;
		else if (offset < ri->start)
			cpos = 0;
		else
			cpos = offset - ri->start;

		do_draw(p, focus, ri, 0, cpos, ri->x, y);

		while (split < ri->split_cnt ||
		       (ri->next && !ri->next->eol && ri->next->y > y)) {
			/* line wrap here */
			/* don't show head/tail for wrap-regions */
			if (*wrap_tail /*&& !ri->wrap*/)
				draw_wrap(p, focus, wrap_tail,
					  p->w - rd->tail_length, y);
			y += rd->line_height;
			if (*wrap_head /*&& !ri->wrap*/)
				draw_wrap(p, focus, wrap_head,
					  0, y);
			if (ri->split_list && split < ri->split_cnt) {
				split += 1;
				do_draw(p, focus, ri, split, cpos,
					ri->wrap_x,
					y);
			} else
				break;
		}
		if (offset < ri->start + ri->len)
			offset = -1;
	}
}

static int find_xy(struct pane *p safe, struct pane *focus safe,
		   short x, short y, const char **xyattr)
{
	/* Find the location in ->line that is best match for x,y.
	 * If x,y is on the char at that location, when store attributes
	 * for the char in xyattr
	 * We always return a location, even if no xyattr.
	 * We use the last render_item that is not definitely after x,y
	 * We do not consider the eol render_item
	 */
	struct call_return cr;
	struct rline_data *rd = &p->data;
	struct render_item *r, *ri = NULL;
	int splitpos = 0;
	int start = 0;

	for (r = rd->content; r ; r = r->next) {
		int split;
		if (r->y <= y && r->x <= x) {
			ri = r;
			start = r->start;
		}
		for (split = 0; split < r->split_cnt; split++) {
			if (r->y + (split + 1) * rd->line_height <= y &&
			    r->wrap_x <= x && r->split_list) {
				ri = r;
				splitpos = r->split_list[split];
				start = r->start + splitpos;
			}
		}
	}
	if (!ri)
		return 0;
	if (ri->eol)
		/* newline or similar.  Can only be at start */
		return start;
	cr = do_measure(p, ri, splitpos, -1, x - ri->x);
	if ((splitpos ? ri->wrap_x : ri->x ) + cr.x > x &&
	    ri->y + rd->line_height * splitpos > y &&
	    xyattr)
		*xyattr = ri->attr;
	if (cr.s)
		return cr.s - rd->line;
	return start + cr.i;
}

static struct xy find_curs(struct pane *p safe, int offset, const char **cursattr)
{
	struct call_return cr;
	struct xy xy = {0,0};
	int split;
	int st;
	struct rline_data *rd = &p->data;
	struct render_item *r, *ri = NULL;

	for (r = rd->content; r; r = r->next) {
		if (offset < r->start)
			break;
		ri = r;
	}
	if (!ri) {
		/* This should be impossible as the eol goes past
		 * the largest offset.
		 */
		return xy;
	}
	if (offset < ri->start)
		/* in the attrs?? */
		offset = 0;
	else
		offset -= ri->start;
	/* offset now from ri->start */
	if (ri->len && rd->line[ri->start] == '\t' && offset)
		offset = ri->tab_cols;
	if (cursattr)
		*cursattr = ri->attr;
	st = 0;
	for (split = 0; split < ri->split_cnt && ri->split_list; split ++) {
		if (offset < ri->split_list[split])
			break;
		st = ri->split_list[split];
	}
	if (ri->eol)
		cr.x = offset ? ri->width : 0;
	else
		cr = do_measure(p, ri, st, offset - st, -1);

	if (split)
		xy.x = ri->wrap_x + cr.x;
	else
		xy.x = ri->x + cr.x;
	xy.y = ri->y + split * rd->line_height;
	if (ri->next == NULL && offset > ri->len) {
		/* After the newline ? Go to next line */
		xy.x = 0;
		xy.y += rd->line_height;
	}
	return xy;
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
			int dodraw,
			int offset, int want_xypos, short x, short y)
{
	char *fname = NULL;
	const char *orig_line = line;
	short width, height;
	short rows = -1, cols = -1;
	int map_offset = 0;
	int ioffset;
	struct xy xyscale = pane_scale(focus);
	int scale = xyscale.x;
	char *ssize = attr_find(p->attrs, "cached-size");
	struct xy size= {-1, -1};

	if (dodraw)
		home_call(focus, "Draw:clear", p);

	width = p->parent->w/2;
	height = p->parent->h/2;

	while (*line == soh)
		line += 1;

	while (*line && *line != stx && *line != etx) {
		int len = strcspn(line, "," STX ETX);
		if (strstarts(line, "image:")) {
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
		} else if (strstarts(line, "width:")) {
			width = atoi(line + 6);
			width = width * scale / 1000;
		} else if (strstarts(line, "height:")) {
			height = atoi(line + 7);
			height = height * scale / 1000;
		} else if (strstarts(line, "noupscale") &&
			   fname && size.x > 0) {
			if (size.x < p->parent->w)
				width = size.x;
			if (size.y < p->parent->h)
				height = size.y;
		} else if ((offset >= 0 || want_xypos) &&
			   strstarts(line, "map:")) {
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

DEF_CMD(renderline_draw)
{
	struct rline_data *rd = &ci->home->data;
	struct xy xy;
	int offset = -1;

	if (ci->num >= 0)
		offset = rd->prefix_bytes + ci->num;

	if (rd->image)
		render_image(ci->home, ci->focus, rd->line, True,
			     offset, False, 0, 0);
	else
		draw_line(ci->home, ci->focus, offset);

	if (ci->num >= 0) {
		xy = find_curs(ci->home, rd->prefix_bytes + ci->num, NULL);
		ci->home->cx = xy.x;
		ci->home->cy = xy.y;
	}
	return 1;
}

DEF_CMD(renderline_refresh)
{
	struct rline_data *rd = &ci->home->data;
	int offset = -1;

	if (rd->curspos >= 0)
		offset = rd->prefix_bytes + rd->curspos;
	if (rd->image)
		render_image(ci->home, ci->focus, rd->line, True,
			     offset, False, 0, 0);
	else {
		measure_line(ci->home, ci->focus, offset);
		draw_line(ci->home, ci->focus, offset);
	}
	return 1;
}

DEF_CMD(renderline_measure)
{
	struct rline_data *rd = &ci->home->data;
	int ret;

	if (rd->image)
		return render_image(ci->home, ci->focus, rd->line,
				    False, ci->num, False, 0, 0);

	ret = measure_line(ci->home, ci->focus,
			   ci->num < 0 ? -1 : rd->prefix_bytes + ci->num);
	rd->prefix_pixels = 0;
	if (rd->prefix_bytes) {
		struct xy xy = find_curs(ci->home, rd->prefix_bytes, NULL);
		rd->prefix_pixels = xy.x;
	}
	if (ci->num >= 0) {
		/* Find cursor and report x,y pos and attributes */
		const char *cursattr = NULL;
		struct xy xy;
		xy = find_curs(ci->home, rd->prefix_bytes + ci->num, &cursattr);
		comm_call(ci->comm2, "cb", ci->focus, ret, NULL,
			  cursattr);
		ci->home->cx = xy.x;
		ci->home->cy = xy.y;
	}
	return ret | 4;
}

DEF_CMD(renderline_findxy)
{
	struct rline_data *rd = &ci->home->data;
	const char *xyattr = NULL;
	int pos;

	if (rd->image)
		return render_image(ci->home, ci->focus, rd->line,
				    False, -1, True,
				    ci->x, ci->y);

	measure_line(ci->home, ci->focus,
		     ci->num < 0 ? -1 : rd->prefix_bytes + ci->num);
	pos = find_xy(ci->home, ci->focus, ci->x, ci->y, &xyattr);
	if (pos >= rd->prefix_bytes)
		pos -= rd->prefix_bytes;
	else {
		pos = 0;
		xyattr = NULL;
	}
	comm_call(ci->comm2, "cb", ci->focus, pos, NULL, xyattr);
	return pos+1;
}

DEF_CMD(renderline_get)
{
	struct rline_data *rd = &ci->home->data;
	char buf[20];
	const char *val = buf;

	if (!ci->str)
		return Enoarg;
	if (strcmp(ci->str, "prefix_len") == 0)
		snprintf(buf, sizeof(buf), "%d", rd->prefix_pixels);
	else if (strcmp(ci->str, "curs_width") == 0)
		snprintf(buf, sizeof(buf), "%d", rd->curs_width);
	else if (strcmp(ci->str, "width") == 0)
		snprintf(buf, sizeof(buf), "%d", rd->width);
	else
		return Einval;

	comm_call(ci->comm2, "attr", ci->focus, 0, NULL, val);
	return 1;
}

static char *cvt(char *str safe)
{
	/* Convert:
	 *    << to < ack  (ack is a no-op)
	 *    < stuff > to soh stuff stx
	 *    </> to ack ack etx
	 */
	char *c, *c1;
	for (c = str; *c; c += 1) {
		if (c[0] == soh || c[0] == ack)
			break;
		if (c[0] == '<' && c[1] == '<') {
			c[1] = ack;
			c++;
			continue;
		}
		if (c[0] != '<')
			continue;
		if (c[1] == '/') {
			while (*c && *c != '>')
				*c++ = ack;
			if (!*c)
				break;
			*c = etx;
			continue;
		}
		c[0] = soh;
		c += 1;
		c1 = c;
		while (*c && *c != '>') {
			if (*c == '\\' &&
			    (c[1] == '\\' || c[1] == '>'))
				c++;
			*c1++ = *c++;
		}
		while (c1 < c)
			*c1++ = ack;
		if (!*c)
			break;
		*c = stx;
	}
	return str;
}

DEF_CMD(renderline_set)
{
	struct rline_data *rd = &ci->home->data;
	const char *old = rd->line;
	char *prefix = pane_attr_get(ci->focus, "prefix");
	bool word_wrap = pane_attr_get_int(ci->focus, "word-wrap", 0) != 0;

	if (!ci->str)
		return -Enoarg;
	if (prefix)
		prefix = cvt(strconcat(ci->home, "<bold>", prefix, "</>"));

	if (prefix)
		rd->line = strconcat(NULL, prefix, ci->str);
	else
		rd->line = strdup(ci->str);
	rd->prefix_bytes = strlen(prefix?:"");
	cvt(rd->line + rd->prefix_bytes);

	rd->curspos = ci->num;
	if (strcmp(rd->line, old) != 0 ||
	    (old && word_wrap != rd->word_wrap)) {
		pane_damaged(ci->home, DAMAGED_REFRESH);
		pane_damaged(ci->home->parent, DAMAGED_REFRESH);
		rd->word_wrap = word_wrap;
		parse_line(rd);
	}
	free((void*)old);
	ci->home->damaged &= ~DAMAGED_VIEW;
	return 1;
}

DEF_CMD(renderline_close)
{
	struct rline_data *rd = &ci->home->data;
	struct render_item *ri = rd->content;

	free((void*)rd->line);
	while (ri) {
		struct render_item *r = ri;
		ri = r->next;
		free(r->split_list);
		unalloc_str_safe(r->attr, pane);
		unalloc(r, pane);
	}
	aupdate(&rd->wrap_head, NULL);
	aupdate(&rd->wrap_tail, NULL);
	aupdate(&rd->wrap_attr, NULL);
	return 1;
}

static struct map *rl_map;
DEF_LOOKUP_CMD(renderline_handle, rl_map);

DEF_CMD(renderline_attach)
{
	struct pane *p;
	struct rline_data *rd;

	if (!rl_map) {
		rl_map = key_alloc();
		key_add(rl_map, "render-line:draw", &renderline_draw);
		key_add(rl_map, "Refresh", &renderline_refresh);
		key_add(rl_map, "render-line:measure", &renderline_measure);
		key_add(rl_map, "render-line:findxy", &renderline_findxy);
		key_add(rl_map, "get-attr", &renderline_get);
		key_add(rl_map, "render-line:set", &renderline_set);
		key_add(rl_map, "Close", &renderline_close);
		key_add(rl_map, "Free", &edlib_do_free);
	}

	p = pane_register(ci->focus, ci->num, &renderline_handle.c);
	if (!p)
		return Efail;
	rd = &p->data;
	rd->line = strdup(ETX); // Imposible string

	return comm_call(ci->comm2, "cb", p);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &renderline_attach, 0, NULL,
		  "attach-renderline");
}
