/*
 * Copyright Neil Brown ©2017-2020 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Filter a view on a document to convert utf-8 sequences into
 * the relevant unicode characters.
 */

#include <unistd.h>
#include <stdlib.h>

#include "core.h"

static struct map *utf8_map safe;
DEF_LOOKUP_CMD(utf8_handle, utf8_map);

DEF_CMD(utf8_step)
{
	int forward = ci->num;
	int move = ci->num2;
	struct pane *p = ci->home->parent;
	wint_t ch;
	struct mark *m = ci->mark;
	char buf[10];
	const char *b;
	int i;
	wint_t ret;

	if (!m)
		return Enoarg;

	ch = doc_step(p, m, forward, move);
	if (ch == WEOF || (ch & 0x7f) == ch)
		return CHAR_RET(ch);
	if (!move)
		m = mark_dup(m);
	if (forward) {
		i = 0;
		buf[i++] = ch;
		while ((ch = doc_following(p, m)) != WEOF &&
		       (ch & 0xc0) == 0x80 && i < 10) {
			buf[i++] = ch;
			doc_next(p, m);
		}
		b = buf;
		ret = get_utf8(&b, b+i);
	} else {
		i = 10;
		buf[--i] = ch;
		while (ch != WEOF && (ch & 0xc0) != 0xc0 && i > 0) {
			ch = doc_prev(p, m);
			buf[--i] = ch;
		}
		b = buf + i;
		ret = get_utf8(&b, buf+10);
	}
	if (!move)
		mark_free(m);
	return CHAR_RET(ret);
}

DEF_CMD(utf8_attach)
{
	struct pane *p;

	p = pane_register(ci->focus, 0, &utf8_handle.c);
	if (!p)
		return Efail;

	return comm_call(ci->comm2, "callback:attach", p);
}

void edlib_init(struct pane *ed safe)
{

	utf8_map = key_alloc();

	key_add(utf8_map, "doc:step", &utf8_step);

	call_comm("global-set-command", ed, &utf8_attach, 0, NULL, "attach-charset-utf_8");
	call_comm("global-set-command", ed, &utf8_attach, 0, NULL, "attach-utf8");
}
