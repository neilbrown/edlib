/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
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

#include "core.h"
#include "misc.h"

struct he_data {
	struct pane	*pane;
};

static struct map *he_map;
static struct pane *do_render_hex_attach(struct pane *parent);

DEF_LOOKUP_CMD(render_hex_handle, he_map);

DEF_CMD(render_hex_close)
{
	struct pane *p = ci->home;
	struct he_data *he = p->data;

	he->pane = NULL;
	p->data = NULL;
	p->handle = NULL;
	free(he);
	return 1;
}

DEF_CMD(render_hex_clone)
{
	struct pane *p = ci->home;
	struct pane *parent = ci->focus;
	struct pane *c;

	do_render_hex_attach(parent);
	c = pane_child(p);
	if (c)
		return comm_call_pane(c, "Clone", parent->focus,
				      0, NULL, NULL, 0, NULL);
	return 1;
}

DEF_CMD(render_hex_notify_replace)
{

	/* If change happens only after the view port, we don't
	 * need damage.
	 * If before, we might need to update addresses.
	 * However we cannot currently access the view port, so
	 * always signal damage.
	 * This pane_child call is a hack - it may not be the
	 * render-lines that we want.
	 */
	pane_damaged(pane_child(ci->home), DAMAGED_CONTENT);
	return 1;
}


DEF_CMD(render_hex_eol)
{
	wint_t ch = 1;
	int rpt = RPT_NUM(ci);
	int pos;
	struct cmd_info ci2 = {0};

	ci2.key = "CountLines";
	ci2.home = ci2.focus = ci->home;
	ci2.mark = ci->mark;
	key_handle(&ci2);
	pos = attr_find_int(*mark_attr(ci->mark), "chars");

	pos = attr_find_int(*mark_attr(ci->mark), "chars");
	while (rpt > 0 && ch != WEOF) {
		while ((pos & 15) != 15 &&
		       (ch = mark_next_pane(ci->focus, ci->mark)) != WEOF)
			pos += 1;
		rpt -= 1;
		if (rpt) {
			ch = mark_next_pane(ci->focus, ci->mark);
			pos += 1;
		}
	}
	while (rpt < 0 && ch != WEOF) {
		while ((pos & 15) != 0 &&
		       (ch = mark_prev_pane(ci->focus, ci->mark)) != WEOF)
			pos -= 1;
		rpt += 1;
		if (rpt) {
			ch = mark_prev_pane(ci->focus, ci->mark);
			pos -= 1;
		}
	}
	return 1;
}

DEF_CMD(render_line)
{
	struct buf ret;
	struct cmd_info ci2 = {0};
	struct mark *m = NULL;
	struct mark *pm = ci->mark2;
	int pos;
	int i;
	char buf[10];
	int rv;

	if (!ci->focus || !ci->mark)
		return -1;

	ci2.key = "CountLines";
	ci2.home = ci2.focus = ci->home;
	ci2.mark = ci->mark;
	key_handle(&ci2);
	pos = attr_find_int(*mark_attr(ci->mark), "chars");

	buf_init(&ret);
	if (doc_following_pane(ci->focus, ci->mark) == WEOF)
		goto done;
	sprintf(buf, "<bold>%08x:</> ", pos);
	buf_concat(&ret, buf);
	m = mark_dup(ci->mark, 0);
	for (i = 0; i < 16; i++) {
		wint_t ch;
		struct mark *m2 = ci->mark;

		if (pm && mark_same_pane(ci->home, m2, pm, NULL))
			goto done;
		if (ci->numeric >= 0 && ci->numeric != NO_NUMERIC &&
		    ci->numeric <= ret.len)
			goto done;

		ch = mark_next_pane(ci->focus, m2);
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

		ch = mark_next_pane(ci->focus, m);
		if (ch == WEOF)
			ch = ' ';
		if (ch < ' ')
			ch = '?';
		buf_append(&ret, ch);
		buf_append(&ret, ' ');
		if (i == 7)
			buf_append(&ret, ' ');
	}
	buf_concat(&ret, "</>\n");
done:
	if (m)
		mark_free(m);
	rv = comm_call(ci->comm2, "callback:render", ci->focus, 0, NULL,
		       buf_final(&ret), 0);
	free(ret.b);
	return rv;
}

DEF_CMD(render_line_prev)
{
	/* If ->numeric is 0, round down to multiple of 16.
	 * if it is 1, subtract a further 16.
	 */
	struct cmd_info ci2 = {0};
	int to, from;

	ci2.key = "CountLines";
	ci2.home = ci2.focus = ci->home;
	ci2.mark = ci->mark;
	key_handle(&ci2);

	from = attr_find_int(*mark_attr(ci->mark), "chars");
	to = from & ~0xF;
	if (ci->numeric && to >= 16)
		to -= 16;
	while (to < from) {
		mark_prev_pane(ci->focus, ci->mark);
		from -= 1;
	}
	return 1;
}

static void render_hex_register_map(void)
{
	he_map = key_alloc();

	key_add(he_map, "Move-EOL", &render_hex_eol);

	key_add(he_map, "render-line-prev", &render_line_prev);
	key_add(he_map, "render-line", &render_line);

	key_add(he_map, "Close", &render_hex_close);
	key_add(he_map, "Clone", &render_hex_clone);
	key_add(he_map, "Notify:Replace", &render_hex_notify_replace);
}

static struct pane *do_render_hex_attach(struct pane *parent)
{
	struct he_data *he = malloc(sizeof(*he));
	struct pane *p;

	if (!he_map)
		render_hex_register_map();

	p = pane_register(parent, 0, &render_hex_handle.c, he, NULL);
	call3("Request:Notify:Replace", p, 0, NULL);
	attr_set_str(&p->attrs, "render-wrap", "no", -1);
	attr_set_str(&p->attrs, "heading", "<bold>          00 11 22 33 44 55 66 77  88 99 aa bb cc dd ee ff   0 1 2 3 4 5 6 7  8 9 a b c d e f</>", -1);
	he->pane = p;
	render_attach("lines", p);

	return p;
}

DEF_CMD(render_hex_attach)
{
	return comm_call(ci->comm2, "callback:attach",
			 do_render_hex_attach(ci->focus),
			 0, NULL, NULL, 0);
}

DEF_CMD(hex_appeared)
{
	char *t = pane_attr_get(ci->focus, "doc-type");
	if (t && strcmp(t, "text") == 0)
		call7("doc:attr-set", ci->focus, 0, NULL, "render-Chr-H", 0,
		      "hex", NULL);
	return 0;
}

void edlib_init(struct pane *ed)
{
	call_comm("global-set-command", ed, 0, NULL, "attach-render-hex",
		  0, &render_hex_attach);
	call_comm("global-set-command", ed, 0, NULL, "doc:appeared-hex",
		  0, &hex_appeared);
}
