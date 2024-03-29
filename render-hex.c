/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * hexedit renderer
 *
 * 16 bytes are rendered as hex, and then chars
 * Well... currently we do chars, not bytes, because I cannot control
 * char encoding yet.
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include <stdio.h>

#define PANE_DATA_TYPE struct he_data
#include "core.h"
#include "misc.h"

struct he_data {
	struct pane	*pane;
	bool bytes;
};
#include "core-pane.h"

static struct map *he_map;
static struct pane *do_render_hex_attach(struct pane *parent safe);

DEF_LOOKUP_CMD(render_hex_handle, he_map);

DEF_CMD_CLOSED(render_hex_close)
{
	struct pane *p = ci->home;
	struct he_data *he = p->data;

	he->pane = NULL;
	return 1;
}

DEF_CMD(render_hex_clone)
{
	struct pane *parent = ci->focus;

	do_render_hex_attach(parent);
	pane_clone_children(ci->home, parent->focus);
	return 1;
}

DEF_CMD(render_hex_notify_replace)
{

	/* If change happens only after the view port, we don't
	 * need damage.
	 * If before, we might need to update addresses.
	 * However we cannot currently access the view port, so
	 * always signal damage. FIXME.
	 */
	call("view:changed", pane_focus(ci->home));
	return 1;
}

DEF_CMD(render_hex_eol)
{
	wint_t ch = 1;
	int rpt = RPT_NUM(ci);
	bool one_more = ci->num2 > 0;
	int pos;

	if (!ci->mark)
		return Enoarg;
	call("CountLines", ci->focus, 0, ci->mark);

	pos = attr_find_int(*mark_attr(ci->mark), "char") - 1;
	while (rpt > 0 && ch != WEOF) {
		while ((pos & 15) != 15 &&
		       (ch = doc_next(ci->focus, ci->mark)) != WEOF)
			pos += 1;
		rpt -= 1;
		if (rpt || one_more) {
			ch = doc_next(ci->focus, ci->mark);
			pos += 1;
		}
	}
	while (rpt < 0 && ch != WEOF) {
		while ((pos & 15) != 0 &&
		       (ch = doc_prev(ci->focus, ci->mark)) != WEOF)
			pos -= 1;
		rpt += 1;
		if (rpt || one_more) {
			ch = doc_prev(ci->focus, ci->mark);
			pos -= 1;
		}
	}
	return 1;
}

DEF_CMD(render_line)
{
	struct buf ret;
	struct mark *m = NULL;
	struct mark *pm = ci->mark2;
	int pos;
	int pm_offset = -1;
	int i;
	char buf[30];
	int rv;

	if (!ci->mark)
		return Enoarg;

	call("CountLines", ci->focus, 0, ci->mark);
	pos = attr_find_int(*mark_attr(ci->mark), "char") - 1;

	buf_init(&ret);
	if (doc_following(ci->focus, ci->mark) == WEOF)
		return Efail;
	snprintf(buf, sizeof(buf), "<bold>%08x:</> ", pos);
	buf_concat(&ret, buf);
	m = mark_dup_view(ci->mark);
	for (i = 0; i < 16; i++) {
		wint_t ch;
		struct mark *m2 = ci->mark;

		if (pm && mark_same(m2, pm) && pm_offset < 0)
			pm_offset = ret.len;
		if (ci->num >= 0 && ci->num <= ret.len)
			goto done;

		ch = doc_next(ci->focus, m2);
		if (ch == WEOF)
			strcpy(buf, "   ");
		else
			sprintf(buf, "%02x ", ch & 0xff);
		buf_concat(&ret, buf);
		if (i == 7)
			buf_append(&ret, ' ');
	}

	buf_concat(&ret, "  <fg:red>");
	for (i = 0; i < 16; i++) {
		wint_t ch;

		ch = doc_next(ci->focus, m);
		if (ch == WEOF)
			ch = ' ';
		if (ch < ' ')
			ch = '?';
		buf_append(&ret, ch);
		if (ch == '<')
			/* '<<' to quote the '<' */
			buf_append(&ret, ch);
		buf_append(&ret, ' ');
		if (i == 7)
			buf_append(&ret, ' ');
	}
	buf_concat(&ret, "</>\n");
done:
	if (m)
		mark_free(m);
	rv = comm_call(ci->comm2, "callback:render", ci->focus, pm_offset, NULL,
		       buf_final(&ret));
	free(ret.b);
	return rv ?: 1;
}

DEF_CMD(render_line_prev)
{
	/* If ->num is 0, round down to multiple of 16.
	 * if it is 1, subtract a further 16.
	 */
	int to, from;

	if (!ci->mark)
		return Enoarg;
	call("CountLines", ci->focus, 0, ci->mark);

	from = attr_find_int(*mark_attr(ci->mark), "char") - 1;
	to = from & ~0xF;
	if (ci->num) {
		if (to >= 16)
			to -= 16;
		else
			return Efail;
	}
	while (to < from) {
		doc_prev(ci->focus, ci->mark);
		from -= 1;
	}
	return 1;
}

DEF_CMD(hex_char)
{
	struct he_data *he = ci->home->data;

	if (he->bytes)
		return home_call(ci->home->parent, "doc:byte", ci->focus,
				 ci->num, ci->mark, ci->str,
				 ci->num2, ci->mark2, ci->str2);
	else
		return home_call(ci->home->parent, "doc:char", ci->focus,
				 ci->num, ci->mark, ci->str,
				 ci->num2, ci->mark2, ci->str2);
}

static void render_hex_register_map(void)
{
	he_map = key_alloc();

	key_add(he_map, "doc:EOL", &render_hex_eol);
	key_add(he_map, "doc:char", &hex_char);

	key_add(he_map, "doc:render-line-prev", &render_line_prev);
	key_add(he_map, "doc:render-line", &render_line);

	key_add(he_map, "Close", &render_hex_close);
	key_add(he_map, "Clone", &render_hex_clone);
	key_add(he_map, "doc:replaced", &render_hex_notify_replace);
}

static struct pane *do_render_hex_attach(struct pane *parent safe)
{
	struct he_data *he;
	struct pane *p;
	char *charset = pane_attr_get(parent, "doc:charset");

	if (!he_map)
		render_hex_register_map();

	p = pane_register(parent, 0, &render_hex_handle.c);
	if (!p)
		return NULL;
	he = p->data;
	call("doc:request:doc:replaced", p);
	attr_set_str(&p->attrs, "render-wrap", "no");
	attr_set_str(&p->attrs, "heading", "<bold>          00 11 22 33 44 55 66 77  88 99 aa bb cc dd ee ff   0 1 2 3 4 5 6 7  8 9 a b c d e f</>");
	he->pane = p;
	he->bytes = (charset && strcmp(charset, "8bit") != 0);
	return call_ret(pane, "attach-render-lines", p);
}

DEF_CMD(render_hex_attach)
{
	struct pane *p = do_render_hex_attach(ci->focus);

	if (!p)
		return Efail;
	return comm_call(ci->comm2, "callback:attach", p);
}

DEF_CMD(hex_appeared)
{
	char *t = pane_attr_get(ci->focus, "doc-type");
	if (t && strcmp(t, "text") == 0)
		attr_set_str(&ci->focus->attrs, "render-cmd-H", "hex");
	return Efallthrough;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &render_hex_attach, 0, NULL, "attach-render-hex");
	call_comm("global-set-command", ed, &hex_appeared, 0, NULL, "doc:appeared-hex");
}
