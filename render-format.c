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
#include <stdio.h>
#include "core.h"
#include "misc.h"

struct rf_data {
	int home_field;
	int fields;
};

static char *do_format(struct rf_data *rf safe, struct pane *focus safe,
		       struct mark *m safe, struct mark *pm,
		       int len, int attrs)
{
	char *body = pane_attr_get(focus, "line-format");
	struct buf ret;
	char *n;
	int home = 0;
	int field = 0;

	if (pm && !mark_same_pane(focus, pm, m))
		pm = NULL;
	buf_init(&ret);

	if (!body)
		body = "%+name";
	n = body;
	m->rpos = field;
	if (pm && (pm->rpos == NO_RPOS|| pm->rpos == NEVER_RPOS))
		pm->rpos = rf->home_field;
	if (pm && pm->rpos == m->rpos)
		goto endwhile;
	if (len >= 0 && ret.len >= len)
		goto endwhile;

	while (*n) {
		char buf[40], *b, *val;
		int w, adjust, l;

		if (!attrs && *n == '<' && n[1] != '<') {
			/* an attribute, skip it */
			n += 1;
			while (*n && *n != '>')
				n += 1;
			if (*n == '>')
				n += 1;
			continue;
		}
		if (*n != '%' || n[1] == '%') {
			buf_append_byte(&ret, *n);
			if (*n == '%')
				n += 1;
			n += 1;
			continue;
		}
		if (n[1] == '+' || n[1] == '.')
			field += 1;
		m->rpos = field;

		if (len >= 0 && ret.len >= len)
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
		} else if (*n == '.')
			n += 1;
		b = buf;
		while (*n == '-' || *n == '_' || isalnum(*n)) {
			if (b < buf + sizeof(buf) - 2)
				*b++ = *n;
			n += 1;
		}
		*b = 0;
		if (!buf[0])
			val = "";
		else
			val = pane_mark_attr(focus, m, buf);
		if (!val)
			val = "-";

		if (*n != ':') {
			while (*val) {
				if (*val == '<' && attrs)
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
			if (*val == '<' && attrs)
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
		rf->fields = field+1;
		rf->home_field = home;
		m->rpos = field;
		if (pm && pm->rpos == m->rpos)
			;
		else if (len >= 0)
			;
		else
			buf_append(&ret, '\n');
	}
	return buf_final(&ret);
}

DEF_CMD(render_line)
{
	struct rf_data *rf = ci->home->data;
	struct mark *m = ci->mark;
	struct mark *pm = ci->mark2;
	char *ret;
	int rv;
	int len;

	if (!ci->mark)
		return -1;
	if (doc_following_pane(ci->focus, ci->mark) == WEOF)
		return 1;

	if (pm && !mark_same_pane(ci->focus, pm, m))
		pm = NULL;
	if (ci->num == NO_NUMERIC || ci->num < 0)
		len = -1;
	else
		len = ci->num;
	ret = do_format(rf, ci->focus, ci->mark, pm, len, 1);
	if (!pm && len < 0)
		mark_next_pane(ci->focus, m);
	rv = comm_call(ci->comm2, "callback:render", ci->focus, 0, NULL, ret);
	free(ret);
	return rv;
}

DEF_CMD(render_line_prev)
{
	struct mark *m = ci->mark;

	if (!m)
		return -1;
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
	struct pane *p;

	p = do_render_format_attach(ci->focus);
	pane_clone_children(ci->home, p);
	return 1;
}

DEF_CMD(format_get_attr)
{
	char attr[20];
	struct rf_data *rf = ci->home->data;

	if (!ci->mark ||
	    !ci->str ||
	    strcmp(ci->str, "renderline:fields") != 0)
		return 0;
	sprintf(attr, "%u:%u", rf->home_field, rf->fields);
	return comm_call(ci->comm2, "attr", ci->focus, 0, ci->mark, attr);
}

static struct map *rf_map;

static void render_format_register_map(void)
{
	rf_map = key_alloc();

	key_add(rf_map, "render-line", &render_line);
	key_add(rf_map, "render-line-prev", &render_line_prev);
	key_add(rf_map, "Close", &format_close);
	key_add(rf_map, "Clone", &format_clone);
	key_add(rf_map, "doc:get-attr", &format_get_attr);
}

DEF_LOOKUP_CMD(render_format_handle, rf_map);

static struct pane *do_render_format_attach(struct pane *parent)
{
	struct rf_data *rf = calloc(1, sizeof(*rf));
	struct pane *p;

	if (!rf_map)
		render_format_register_map();

	rf->home_field = -1;
	p = pane_register(parent, 0, &render_format_handle.c, rf, NULL);
	attr_set_str(&p->attrs, "render-wrap", "no");
	return render_attach("lines", p);
}

DEF_CMD(render_format_attach)
{
	struct pane *p;

	p = do_render_format_attach(ci->focus);
	if (!p)
		return -1;
	return comm_call(ci->comm2, "callback:attach", p);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &render_format_attach, 0, NULL, "attach-render-format");
}
