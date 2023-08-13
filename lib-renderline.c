// always remeasure?
// :xx is points: tab left_margin
// what exactly is left margin for wrapping
// wrap content with cursor should itself wrap if needed, appeared unwrapped if possible
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
#include <wctype.h>
#include <stdint.h>

#define PANE_DATA_TYPE struct rline_data
#include "core.h"
#include "misc.h"

/* There is one render_item entry
 * - each string of text with all the same attributes
 * - each individual TAB
 * - each unknown control character
 * - the \n \f or \0 which ends the line
 * When word-wrap is enabled, strings of linear white-space get
 * different attributes, so a different render_item entry.
 *
 * attributes understood at this level are:
 *  center or centre	- equal space on either end of flushed line
 *  left:nn		- left margin - in "points"
 *  right:nn		- right margin
 *  tab:nn		- move to nn from left margin or -nn from right margin
 *  rtab:nn		- from here to next tab or eol right-aligned at nn
 *  ctab:nn		- from here to next tab or eol centered at nn
 *  space-above:nn	- extra space before (wrapped) line
 *  space-below:nn	- extra space after (wrapped) line
 *  height:nn		- override height.  This effectively adds space above
 *			  every individual line if the whole line is wrapped
 *  wrap		- text with this attr can be hidden when used as a wrap
 *			  point.
 *  wrap-margin		- remember this x offset as left margin of wrapped lines
 *  wrap-head=xx	- text is inserted at start of line when wrapped
 *  wrap-tail=xx	- text to include at end of line when wrapped.  This
 *			  determines how far before right margin that wrapp is
 *			  triggered.
 *  wrap-XXXX		- attrs to apply to wrap head/tail. Anything not recognised
 *			  has "wrap-" stripped and is used for the head and tail.
 *  hide		- Text is hidden if cursor is not within range.
 *
 * "nn" is measured in "points" which is 1/10 the nominal width of chars
 * in the default font size, which is called "10".  A positive value is
 * measured from the left margin or, which setting margins, from the
 * left page edge.  A negative value is measures from the right margin
 * or right page edge.
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
	unsigned short	wrap_x;
	uint8_t		split_cnt; /* wrap happens this many times at byte
				    * positions in split_list */
	uint8_t		wrap; /* this and consecutive render_items
			       * with the same wrap number form an
			       * optional wrap point.  It is only visible
			       * when not wrapped, or when cursor is in
			       * it.
			       */
	uint8_t		hide; /* This and consecutive render_items
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

	struct render_item *content;
};
#include "core-pane.h"

enum {
	OK = 0,
	WRAP,
	XYPOS,
};

/* sequentially set _attr to the an attr name, and _val to
 * either the val (following ":") or NULL.
 * _attr is valid up to : or , or < space and _val is valid up to , or <space
 * _c is the start which will be updates, and _end is the end which
 * must point to , or nul or a control char
 */
#define foreach_attr(_attr, _val, _c, _end)			\
	for (_attr = _c, _val = find_val(&_c, _end);		\
	     _attr;						\
	     _attr = _c, _val = find_val(&_c, _end))
static const char *find_val(const char **cp safe, const char *end safe)
{
	const char *c = *cp;
	const char *ret;

	if (!c)
		return NULL;
	while (c < end && *c != ':' && *c != ',')
		c++;
	if (c == end) {
		*cp = NULL;
		return NULL;
	}
	if (*c == ',') {
		while (*c == ',' && c < end)
			c++;
		if (c == end) {
			*cp = NULL;
			return NULL;
		}
		*cp = c;
		return NULL;
	}
	c += 1;
	ret = c;
	while (c < end && *c != ',')
		c++;
	while (c < end && *c == ',')
		c++;
	if (c == end)
		c = NULL;
	*cp = c;
	return ret;
}

static bool amatch(const char *a safe, const char *m safe)
{
	while (*a && *a == *m) {
		a += 1;
		m += 1;
	}
	if (*m)
		/* Didn't match all of m */
		return False;
	if (*a != ':' && *a != ',' && *a >= ' ')
		/* Didn't match all of a */
		return False;
	return True;
}

static bool aprefix(const char *a safe, const char *m safe)
{
	while (*a && *a == *m) {
		a += 1;
		m += 1;
	}
	if (*m)
		/* Didn't match all of m */
		return False;
	return True;
}

static long anum(const char *v safe)
{
	char *end = NULL;
	long ret = strtol(v, &end, 10);
	if (end == v || !end ||
	    (*end != ',' && *end >= ' '))
		/* Not a valid number - use zero */
		return 0;
	return ret;
}

static void aupdate(char **cp safe, const char *v)
{
	/* duplicate value at v and store in *cp, freeing what is there
	 * first
	 */
	const char *end = v;

	while (end && *end != ',' && *end >= ' ')
		end += 1;

	free(*cp);
	if (v)
		*cp = strndup(v, end-v);
	else
		*cp = NULL;
}

static void aappend(struct buf *b safe, char const *a safe)
{
	const char *end = a;
	while (*end >= ' ' && *end != ',')
		end++;
	buf_concat_len(b, a, end-a);
	buf_append(b, ',');
}

static void add_render(struct rline_data *rd safe, struct render_item **safe*rlp safe,
		       const char *start safe, const char *end safe,
		       char *attr safe,
		       short tab, enum tab_align align,
		       bool wrap_margin,
		       short wrap, short hide)
{
	struct render_item *ri;
	struct render_item **riend = *rlp;

	alloc(ri, pane);
	ri->attr = strdup(attr);
	ri->start = start - rd->line;
	ri->len = end - start;
	ri->tab_align = align;
	ri->tab = tab;
	ri->wrap = wrap;
	ri->hide = hide;
	ri->wrap_margin = wrap_margin;
	ri->eol = !!strchr("\n\f\0", *start);
	*riend = ri;
	riend = &ri->next;
	*rlp = riend;
}

static inline bool is_ctrl(unsigned int c)
{
	return c < ' ' ||
		(c >= 128 && c < 128 + ' ');
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
	int tab = TAB_UNSET, align = TAB_LEFT;
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

		if (line - 1 > st) {
			/* All text from st to line-1 has "attr' */
			add_render(rd, &riend, st, line-1, buf_final(&attr),
				   tab, align, wrap_margin, wrap, hide);
			align = TAB_LEFT;
			tab = TAB_UNSET;
			wrap_margin = False;
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
				   tab, align, wrap_margin, wrap, hide);
			tab = TAB_UNSET;
			align = TAB_LEFT;
			wrap_margin = False;
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
				   tab, align, wrap_margin, wrap, hide);
			tab = TAB_UNSET;
			align = TAB_LEFT;
			wrap_margin = False;
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
					    char *str safe, int len,
					    int offset, int scale,
					    const char *attr)
{
	struct call_return cr;
	char tmp;

	if (len >= 0) {
		tmp = str[len];
		str[len] = 0;
	}
	cr = call_ret(all, "Draw:text-size", p,
		      offset, NULL, str,
		      scale, NULL, attr);
	if (len >= 0)
		str[len] = tmp;
	return cr;
}

static inline void do_draw(struct pane *p safe,
			   struct pane *focus safe,
			   char *str safe, int len, int tab_cols,
			   int offset,
			   const char *attr, int x, int y)
{
	struct rline_data *rd = &p->data;
	char tmp;
	char tb[] = "         ";

	y += rd->ascent;
	if (strchr("\f\n\0", str[0])) {
		/* end marker - len extends past end of string,
		 * but mustn't write there.  Only need to draw if
		 * cursor is here.
		 */
		if (offset == 0)
			home_call(focus, "Draw:text", p, offset, NULL, "",
				  rd->scale, NULL, attr, x, y);
		return;
	}
	if (str[0] == '\t') {
		/* Tab needs special handling */
		str = tb;
		len = tab_cols;
	}
	if (len >= 0) {
		tmp = str[len];
		str[len] = 0;
	}
	home_call(focus, "Draw:text", p, offset, NULL, str,
			   rd->scale, NULL, attr, x, y);
	if (len >= 0)
		str[len] = tmp;
}

static void add_split(struct render_item *ri safe, int split)
{
	int i = ri->split_cnt;
	ri->split_cnt += 1;
	ri->split_list = realloc(ri->split_list,
				 sizeof(ri->split_list[0]) * ri->split_cnt);
	ri->split_list[i] = split;
}

static int calc_tab(int num, int margin, int scale)
{
	if (num > 0)
		return num * scale / 1000;
	if (-num > margin)
		return 0;
	return margin + num * scale / 1000;
}

static bool measure_line(struct pane *p safe, struct pane *focus safe, int offset)
{
	/* First measure each render_item entry setting
	 * height, ascent, width.
	 * Then use that with tab information to set 'x' position for
	 * each unit.
	 * Finally identify line-break locations if needed and set 'y'
	 * positions
	 */
	struct rline_data *rd = &p->data;
	struct render_item *ri, *wraprl;
	int shift_left = pane_attr_get_int(focus, "shift_left", 0);
	bool wrap = shift_left < 0;
	int wrap_margin;
	int right_margin = p->w - (rd->right_margin * rd->scale / 1000);
	int xdiff, ydiff;
	struct call_return cr;
	int x, y;
	bool eop = False;

	if (!rd->content)
		return eop;
	cr = do_measure(p, "M", -1, -1, rd->scale,"");
	rd->curs_width = cr.x;
	rd->line_height = cr.y;
	rd->ascent = cr.i2;
	if (rd->min_height * rd->scale / 1000 > rd->line_height)
		rd->line_height = rd->min_height * rd->scale / 1000;

	if (rd->wrap_head) {
		cr = do_measure(p, rd->wrap_head, -1, -1, rd->scale,
				rd->wrap_attr);
		rd->head_length = cr.x;
	}
	cr = do_measure(p, rd->wrap_tail ?: "\\", -1, -1, rd->scale,
			rd->wrap_attr);
	rd->tail_length = cr.x;

	for (ri = rd->content; ri; ri = ri->next) {
		char tmp[4] = "";
		char *txt = tmp;
		int len = -1;
		if (!is_ctrl(rd->line[ri->start])) {
			txt = rd->line + ri->start;
			len = ri->len;
		} else if (ri->eol) {
			/* Ensure attributes of newline add to line height.
			 * The width will be ignored. */
			strcpy(tmp, "M");
			if (rd->line[ri->start] == '\f')
				eop = True;
		} else if (rd->line[ri->start] == '\t') {
			strcpy(tmp, " ");
		} else {
			strcpy(tmp, "^x");
			tmp[1] = '@' + (rd->line[ri->start] & 31);
		}
		cr = do_measure(p, txt, len, -1, rd->scale, ri->attr);
		if (cr.y > rd->line_height)
			rd->line_height = cr.y;
		ri->height = cr.y;
		if (cr.i2 > rd->ascent)
			rd->ascent = cr.i2;
		ri->width = ri->eol ? 0 : cr.x;
		ri->hidden = False;

		if (ri->start <= offset && offset <= ri->start + ri->len) {
			cr = do_measure(p, "M", -1, -1, rd->scale, ri->attr);
			rd->curs_width = cr.x;
		}

		ri->split_cnt = 0;
		free(ri->split_list);
		ri->split_list = NULL;
	}
	/* Set 'x' position honouring tab stops, and set length
	 * of "\t" characters.  Also handle \n and \f.
	 */
	x = (rd->left_margin * rd->scale / 1000) - (shift_left > 0 ? shift_left : 0);
	y = rd->space_above * rd->scale / 1000;
	rd->width = 0;
	for (ri = rd->content; ri; ri = ri->next) {
		int w, margin;
		struct render_item *ri2;
		ri->y = y;
		if (ri->tab != TAB_UNSET)
			x =  (rd->left_margin * rd->scale / 1000) + calc_tab(ri->tab, right_margin, rd->scale);
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
			if (rd->line[ri->start] == '\t') {
				int col = x / ri->width;
				int cols= 8 - (col % 8);
				ri->tab_cols = cols;
				ri->width *= cols;
			}
			x += ri->width;
			continue;
		}
		w = ri->width;
		for (ri2 = ri->next;
		     ri2 && ri2->tab_align == TAB_LEFT && ri2->tab == TAB_UNSET;
		     ri2 = ri2->next)
			w += ri2->width;
		while (ri2 && ri2->tab == TAB_UNSET)
			ri2 = ri2->next;
		margin = right_margin;
		if (ri2)
			margin =  (rd->left_margin * rd->scale / 1000) + calc_tab(ri2->tab, right_margin, rd->scale);
		if (ri->tab_align == TAB_RIGHT) {
			margin -= rd->tail_length;// FIXME don't want this HACK
			x = x + margin - x - w;
		} else
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
	wrap_margin = rd->head_length;
	for (ri = rd->content ; wrap && ri ; ri = ri->next) {
		int splitpos;
		char *str;
		int len;
		char tb[] = "        ";
		if (ri->wrap && (wraprl == NULL || ri->wrap != wraprl->wrap))
			wraprl = ri;
		if (ri->wrap_margin)
			wrap_margin = ri->x;
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
		/* This doesn't fit here */
		if (wraprl) {
			/* Move wraprl to next line and hide it unless it contains cursor */
			int xd = wraprl->x - wrap_margin;
			struct render_item *wraprl2, *ri2;

			/* Find last ritem in wrap region.*/
			for (wraprl2 = wraprl ;
			     wraprl2->next && wraprl2->next->wrap == wraprl->wrap ;
			     wraprl2 = wraprl2->next)
				;
			if (wraprl2->next)
				xd = wraprl2->next->x - wrap_margin;
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
			}
			xdiff -= xd;
			ydiff += rd->line_height;
			wraprl = NULL;
			continue;
		}
	normal_wrap:
		if (ri->x >= right_margin - rd->tail_length) {
			/* This ri moves completely to next line */
			xdiff -= ri->x - wrap_margin;
			ri->x = wrap_margin;
			ydiff += rd->line_height;
			ri->y += rd->line_height;
			wraprl = NULL;
			continue;
		}
		/* Need to split this ri into two or more pieces */
		x = ri->x;
		splitpos = 0;
		str = rd->line + ri->start;
		len = ri->len;
		if (*str == '\t') {
			str = tb;
			len = ri->tab_cols;
		}
		while (1) {
			cr = do_measure(p, str + splitpos,
					len - splitpos,
					right_margin - rd->tail_length - x,
					rd->scale, ri->attr);
			if (cr.i >= len - splitpos)
				/* Remainder fits now */
				break;
			/* re-measure the first part */
			cr = do_measure(p, str + splitpos,
					cr.i,
					right_margin - rd->tail_length - x,
					rd->scale, ri->attr);

			ydiff += rd->line_height;
			xdiff -= cr.x; // fixme where does wrap_margin fit in there
			if (splitpos == 0)
				xdiff -= ri->x;
			splitpos += cr.i;
			x = wrap_margin;
			add_split(ri, splitpos);
		}
	}
	/* We add rd->line_height for the EOL, whether a NL is present of not */
	ydiff += rd->line_height;
	pane_resize(p, p->x, p->y, p->w,
		    (rd->space_above + rd->space_below) * rd->scale / 1000 +
		    ydiff);
	attr_set_int(&p->attrs, "line-height", rd->line_height);
	return eop;
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

		do_draw(p, focus,
			rd->line + ri->start, ri->split_list ? ri->split_list[0]: ri->len,
			ri->split_list ? ri->split_list[0] : ri->tab_cols,
			cpos, ri->attr,
			ri->x, y);
		if (!ri->split_cnt && ri->next &&
		    !ri->next->eol && ri->next->y != ri->y) {
			/* we are about to wrap - draw the markers */
			if (*wrap_tail)
				do_draw(p, focus, wrap_tail, -1, 0, -1,
					rd->wrap_attr,
					p->w - rd->tail_length,
					y);
			if (*wrap_head)
				do_draw(p, focus, wrap_head, -1, 0, -1,
					rd->wrap_attr,
					0, y + rd->line_height);
		}

		while (split < ri->split_cnt ||
		       (ri->next && ri->next->next && ri->next->y > y)) {
			/* line wrap here */
			/* don't show head/tail for wrap-regions */
			if (*wrap_tail /*&& !ri->wrap*/)
				do_draw(p, focus, wrap_tail, -1, 0, -1,
					rd->wrap_attr,
					p->w - rd->tail_length,
					y);
			y += rd->line_height;
			if (*wrap_head /*&& !ri->wrap*/)
				do_draw(p, focus, wrap_head, -1, 0, -1,
					rd->wrap_attr,
					0, y);
			if (ri->split_list && split < ri->split_cnt) {
				int end = ri->len;
				char *str = rd->line + ri->start + ri->split_list[split];
				if (rd->line[ri->start] == '\t') {
					end = ri->tab_cols;
					str = "\t";
				}
				if (split+1 < ri->split_cnt)
					end = ri->split_list[split+1];
				do_draw(p, focus,
					str,
					end - ri->split_list[split],
					end - ri->split_list[split],
					cpos - ri->split_list[split],
					ri->attr, rd->left_margin + rd->head_length,
					y);
				split += 1;
			}
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

	if (!rd->content)
		return 0;
	for (r = rd->content; r ; r = r->next) {
		int split;
		if (r->y <= y && r->x <= x)
			ri = r;
		for (split = 0; split < r->split_cnt; split++) {
			if (r->y + (split + 1) * rd->line_height &&
			    r->x <= r->wrap_x)
				ri = r;
		}
	}
	if (!ri)
		return 0;
	if (ri->eol)
		/* newline or similar.  Can only be at start */
		return ri->start;
	if (ri->x + ri->width > x &&
	    ri->y + ri->height > y &&
	    xyattr)
		*xyattr = ri->attr;
	if (rd->line[ri->start] == '\t')
		cr.i = 0;
	else
		cr = do_measure(p, rd->line + ri->start, ri->len,
				x - ri->x, rd->scale, ri->attr);
	return ri->start + cr.i;
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
	if (rd->line[ri->start] == '\t' && offset)
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
	else {
		char *str = rd->line + ri->start + st;
		char tb[] = "        ";
		if (rd->line[ri->start] == '\t') {
			str = tb;
			if (offset)
				offset = ri->tab_cols;
		}
		cr = do_measure(p, str, offset - st,
				-1, rd->scale, ri->attr);
	}
	if (split)
		xy.x = cr.x; /* FIXME margin?? */
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
			     rd->scale, offset, False, 0, 0);
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
			     rd->scale, offset, False, 0, 0);
	else {
		measure_line(ci->home, ci->focus, offset);
		draw_line(ci->home, ci->focus, offset);
	}
	return 1;
}

DEF_CMD(renderline_measure)
{
	struct rline_data *rd = &ci->home->data;
	bool end_of_page;
	if (rd->image)
		return render_image(ci->home, ci->focus, rd->line,
				    False, rd->scale, ci->num, False, 0, 0);

	end_of_page = measure_line(ci->home, ci->focus,
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
		comm_call(ci->comm2, "cb", ci->focus, end_of_page, NULL,
			  cursattr);
		ci->home->cx = xy.x;
		ci->home->cy = xy.y;
	}
	return end_of_page ? 2 : 1;
}

DEF_CMD(renderline_findxy)
{
	struct rline_data *rd = &ci->home->data;
	const char *xyattr = NULL;
	int pos;

	if (rd->image)
		return render_image(ci->home, ci->focus, rd->line,
				    False, rd->scale, -1, True,
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
	char *c;
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
			c[0] = ack;
			c[1] = ack;
			c[2] = etx;
			c += 2;
			continue;
		}
		c[0] = soh;
		while (c[0] && c[1] != '>')
			c++;
		c[1] = stx;
	}
	return str;
}

DEF_CMD(renderline_set)
{
	struct rline_data *rd = &ci->home->data;
	const char *old = rd->line;
	struct xy xyscale = pane_scale(ci->focus);
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
	    (old && xyscale.x != rd->scale) ||
	    (old && word_wrap != rd->word_wrap)) {
		pane_damaged(ci->home, DAMAGED_REFRESH);
		pane_damaged(ci->home->parent, DAMAGED_REFRESH);
		rd->scale = xyscale.x;
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

	free((void*)rd->line);
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
	rd->line = strdup("");

	return comm_call(ci->comm2, "cb", p);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &renderline_attach, 0, NULL,
		  "attach-renderline");
}
