/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * render-format.  Provide 'render-line' functions to render
 * a document one element per line using a format string to display
 * attributes of that element.
 *
 * This is particularly used for directories and the document list.
 */

#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include "core.h"
#include "misc.h"

struct rf_data {
	int home_field;
	int fields;
};

DEF_CMD(render_line)
{
	char *body = pane_attr_get(ci->focus, "line-format");
	struct rf_data *rf = ci->home->data;
	struct buf ret;
	struct mark *m = ci->mark;
	struct mark *pm = ci->mark2;
	char *n;
	wint_t ch;
	int home;
	int field = 0;
	int rv;

	if (!ci->mark || !ci->focus)
		return -1;

	if (pm && !mark_same_pane(ci->focus, pm, m, NULL))
		pm = NULL;
	ch = doc_following_pane(ci->focus, m);
	if (ch == WEOF)
		return 1;
	buf_init(&ret);

	if (!body)
		body = "%+name";
	n = body;
	m->rpos = field - rf->home_field;
	if (pm && pm->rpos == m->rpos)
		goto endwhile;
	if (ci->numeric != NO_NUMERIC && ci->numeric >= 0 &&
	    ret.len >= ci->numeric)
		goto endwhile;

	while (*n) {
		char buf[40], *b, *val;
		int w, adjust, l;

		if (*n != '%' || n[1] == '%') {
			buf_append_byte(&ret, *n);
			if (*n == '%')
				n += 1;
			n += 1;
			continue;
		}
		field += 1;
		m->rpos = field - rf->home_field;

		if (ci->numeric != NO_NUMERIC && ci->numeric >= 0 &&
		    ret.len >= ci->numeric)
			break;
		if (pm && pm->rpos == m->rpos)
			break;
		n += 1;
		if (*n == '+') {
			/* Home field */
			n += 1;
			home = field;
			if (rf->home_field < 0)
				rf->home_field = home;
		}
		b = buf;
		while (*n == '-' || *n == '_' || isalnum(*n)) {
			if (b < buf + sizeof(buf) - 2)
				*b++ = *n;
			n += 1;
		}
		*b = 0;
		if (strcmp(buf, "c") == 0) {
			/* Display the char under cursor */
			buf_append(&ret, ch);
			continue;
		}
		val = pane_mark_attr(ci->focus, m, 1, buf);
		if (!val)
			val = "-";
		if (*n != ':') {
			while (*val) {
				if (*val == '<')
					buf_append_byte(&ret, '<');
				buf_append_byte(&ret, *val);
				val += 1;
			}
			continue;
		}
		w = 0;
		adjust=0;
		n += 1;
		while (*n) {
			if (isdigit(*n))
				w = w * 10 + (*n - '0');
			else if (w == 0 && *n == '-')
				adjust = 1;
			else break;
			n+= 1;
		}
		l = strlen(val);
		while (adjust && w > l) {
			buf_append(&ret, ' ');
			w -= 1;
		}

		while (*val && w > 0 ) {
			if (*val == '<')
				buf_append_byte(&ret, '<');
			buf_append_byte(&ret, *val);
			w -= 1;
			val += 1;
		}
		while (w > 0) {
			buf_append(&ret, ' ');
			w -= 1;
		}
	}
endwhile:
	if (!*n) {
		rf->fields = field;
		rf->home_field = home;
		m->rpos = field + 1 - rf->home_field;
		if (pm && pm->rpos == m->rpos)
			;
		else if (ci->numeric >= 0 && ci->numeric != NO_NUMERIC)
			;
		else {
			buf_append(&ret, '\n');
			mark_next_pane(ci->focus, m);
		}
	}
	rv = comm_call(ci->comm2, "callback:render", ci->focus, 0, NULL,
		       buf_final(&ret), 0);
	free(ret.b);
	return rv;
}

DEF_CMD(render_line_prev)
{
	struct mark *m = ci->mark;

	if (RPT_NUM(ci) == 0)
		/* always at start-of-line */
		return 1;
	if (mark_prev_pane(ci->focus, m) == WEOF)
		/* Hit start-of-file */
		return -1;
	return 1;
}

DEF_CMD(format_close)
{
	struct rf_data *rl = ci->home->data;
	free(rl);
	return 1;
}

static struct pane *do_render_format_attach(struct pane *parent);
DEF_CMD(format_clone)
{
	struct pane *c;
	struct pane *p;

	p = do_render_format_attach(ci->focus);
	c = pane_child(ci->home);
	if (c)
		return comm_call_pane(c, "Clone", p, 0, NULL, NULL, 0, NULL);
	return 1;
}

DEF_CMD(format_move_line)
{
	int rpt = RPT_NUM(ci);
	struct rf_data *rf = ci->home->data;

	while (rpt > 1) {
		if (mark_next_pane(ci->focus, ci->mark) == WEOF)
			break;
		rpt -= 1;
	}
	while (rpt < -1) {
		if (mark_prev_pane(ci->focus, ci->mark) == WEOF)
			break;
		rpt += 1;
	}
	if (rpt < 0)
		ci->mark->rpos = -rf->home_field;
	if (rpt > 0)
		ci->mark->rpos = rf->fields + 1 - rf->home_field;


	return 1;
}

DEF_CMD(format_move_horiz)
{
	/* Horizonal movement - adjust ->rpos within fields, or
	 * move to next line
	 */
	struct rf_data *rf = ci->home->data;
	int rpt = RPT_NUM(ci);

	if (rf->fields < 2)
		return 1;
	while (rpt > 0 && doc_following_pane(ci->focus, ci->mark) != WEOF) {
		if (ci->mark->rpos < rf->fields - rf->home_field + 1)
			ci->mark->rpos += 1;
		else {
			if (mark_next_pane(ci->focus, ci->mark) == WEOF)
				break;
			ci->mark->rpos = -rf->home_field;
		}
		rpt -= 1;
	}
	while (rpt < 0) {
		if (ci->mark->rpos > -rf->home_field)
			ci->mark->rpos -= 1;
		else {
			if (mark_prev_pane(ci->focus, ci->mark) == WEOF)
				break;
			ci->mark->rpos = rf->fields - rf->home_field + 1;
		}
		rpt += 1;
	}
	return 1;
}

static struct map *rf_map;

static void render_format_register_map(void)
{
	rf_map = key_alloc();

	key_add(rf_map, "render-line", &render_line);
	key_add(rf_map, "render-line-prev", &render_line_prev);
	key_add(rf_map, "Close", &format_close);
	key_add(rf_map, "Clone", &format_clone);

	key_add(rf_map, "Move-EOL", &format_move_line);
	key_add(rf_map, "Move-Char", &format_move_horiz);
	key_add(rf_map, "Move-Word", &format_move_horiz);
	key_add(rf_map, "Move-WORD", &format_move_horiz);
}

DEF_LOOKUP_CMD(render_format_handle, rf_map);

static struct pane *do_render_format_attach(struct pane *parent)
{
	struct rf_data *rf = malloc(sizeof(*rf));
	struct pane *p;

	if (!rf_map)
		render_format_register_map();

	rf->home_field = -1;
	p = pane_register(parent, 0, &render_format_handle.c, rf, NULL);
	attr_set_str(&p->attrs, "render-wrap", "no", -1);
	return render_attach("lines", p);
}

DEF_CMD(render_format_attach)
{
	struct pane *p;

	p = do_render_format_attach(ci->focus);

	return comm_call(ci->comm2, "callback:attach", p, 0, NULL, NULL, 0);
}

void edlib_init(struct pane *ed)
{
	call_comm("global-set-command", ed, 0, NULL, "attach-render-format",
		  0, &render_format_attach);
}
