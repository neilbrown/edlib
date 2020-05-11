/*
 * Copyright Neil Brown ©2015-2020 <neil@brown.name>
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


static char *do_format(struct pane *focus safe,
		       struct mark *m safe, struct mark *pm,
		       int len, int attrs)
{
	char *body = pane_attr_get(focus, "line-format");
	struct buf ret;
	char *n;

	if (pm && !mark_same(pm, m))
		pm = NULL;
	buf_init(&ret);

	if (!body)
		body = "%name";
	n = body;
	if (pm)
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

		if (len >= 0 && ret.len >= len)
			break;
		if (pm)
			break;
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
		if (len < 0)
			buf_append(&ret, '\n');
	}
	return buf_final(&ret);
}

DEF_CMD(format_content)
{
	if (!ci->mark || !ci->comm2)
		return Enoarg;

	while (doc_following(ci->focus, ci->mark) != WEOF) {
		const char *l, *c;
		wint_t w;

		l = do_format(ci->focus, ci->mark, NULL, -1, 0);
		if (!l)
			break;
		c = l;
		while (*c) {
			w = get_utf8(&c, NULL);
			if (w >= WERR ||
			    comm_call(ci->comm2, "consume", ci->home, w, ci->mark) <= 0)
				/* Finished */
				break;
		}
		free((void*)l);
		if (*c)
			break;
		doc_next(ci->focus, ci->mark);
	}
	return 1;
}

DEF_CMD(render_line)
{
	struct mark *m = ci->mark;
	struct mark *pm = ci->mark2;
	char *ret;
	int rv;
	int len;

	if (!ci->mark)
		return Enoarg;
	if (doc_following(ci->focus, ci->mark) == WEOF)
		return Efalse;

	if (pm && !mark_same(pm, m))
		pm = NULL;
	if (ci->num == NO_NUMERIC || ci->num < 0)
		len = -1;
	else
		len = ci->num;
	ret = do_format(ci->focus, ci->mark, pm, len, 1);
	if (!pm && len < 0)
		doc_next(ci->focus, m);
	rv = comm_call(ci->comm2, "callback:render", ci->focus, 0, NULL, ret);
	free(ret);
	return rv ?: 1;
}

DEF_CMD(render_line_prev)
{
	struct mark *m = ci->mark;

	if (!m)
		return Enoarg;
	if (RPT_NUM(ci) == 0)
		/* always at start-of-line */
		return 1;
	if (doc_prev(ci->focus, m) == WEOF)
		/* Hit start-of-file */
		return Efail;
	return 1;
}

static struct pane *do_render_format_attach(struct pane *parent, int nolines);
DEF_CMD(format_clone)
{
	struct pane *p;

	p = do_render_format_attach(ci->focus, 0);
	pane_clone_children(ci->home, p);
	return 1;
}

static struct map *rf_map;

static void render_format_register_map(void)
{
	rf_map = key_alloc();

	key_add(rf_map, "doc:render-line", &render_line);
	key_add(rf_map, "doc:render-line-prev", &render_line_prev);
	key_add(rf_map, "Clone", &format_clone);
	key_add(rf_map, "doc:content", &format_content);
}

DEF_LOOKUP_CMD(render_format_handle, rf_map);

static struct pane *do_render_format_attach(struct pane *parent, int nolines)
{
	struct pane *p;

	if (!rf_map)
		render_format_register_map();

	p = pane_register(parent, 0, &render_format_handle.c);
	if (!p)
		return NULL;
	attr_set_str(&p->attrs, "render-wrap", "no");
	if (nolines)
		return p;
	return call_ret(pane, "attach-render-lines", p);
}

DEF_CMD(render_format_attach)
{
	struct pane *p;

	p = do_render_format_attach(ci->focus, ci->num);
	if (!p)
		return Efail;
	return comm_call(ci->comm2, "callback:attach", p);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &render_format_attach, 0, NULL, "attach-render-format");
}
