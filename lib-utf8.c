/*
 * copyright Neil Brown ©2017 <neil@brown.name>
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

static wint_t decode_utf8(char *b safe, int len)
{
	wint_t ret = 0;
	int i;

	for (i = 0; i < len ; i++) {
		if (i)
			ret = ret << 6 | (b[i] & 0x3f);
		else if ((b[i] & 0xf8) == 0xf0)
			ret = (b[i] & 0x07);
		else if ((b[i] & 0xf0) == 0xe0)
			ret = (b[i] & 0x0f);
		else if ((b[i] & 0xe0) == 0xc0)
			ret = (b[i] & 0x1f);
		else
			ret = 0;
	}
	return ret;
}

DEF_CMD(utf8_step)
{
	int forward = ci->num;
	int move = ci->num2;
	struct pane *p = ci->home->parent;
	wint_t ch;
	struct mark *m = ci->mark;
	char buf[10];
	int i;
	wint_t ret;

	if (!m || !p)
		return 0;

	ch = mark_step_pane(p, m, forward, move);
	if (ch == WEOF || (ch & 0x7f) == ch)
		return CHAR_RET(ch);
	if (!move)
		m = mark_dup(m, 1);
	if (forward) {
		i = 0;
		buf[i++] = ch;
		while ((ch = doc_following_pane(p, m)) != WEOF &&
		       (ch & 0xc0) == 0x80 && i < 10) {
			buf[i++] = ch;
			mark_next_pane(p, m);
		}
		ret = decode_utf8(buf, i);
	} else {
		i = 10;
		buf[--i] = ch;
		while (ch != WEOF && (ch & 0xc0) != 0xc0 && i > 0) {
			ch = mark_prev_pane(p, m);
			buf[--i] = ch;
		}
		ret = decode_utf8(buf+i, 10-i);
	}
	if (!move)
		mark_free(m);
	return CHAR_RET(ret);
}

DEF_CMD(utf8_same)
{
	return 0;
}

DEF_CMD(utf8_attach)
{
	struct pane *p;

	p = pane_register(ci->focus, 0, &utf8_handle.c, NULL, NULL);
	if (!p)
		return -1;
	call("doc:set:filter", p, 1);

	return comm_call(ci->comm2, "callback:attach", p);
}

void edlib_init(struct pane *ed safe)
{

	utf8_map = key_alloc();

	key_add(utf8_map, "doc:step", &utf8_step);
	key_add(utf8_map, "doc:mark-same", &utf8_same);

	call_comm("global-set-command", ed, &utf8_attach, 0, NULL, "attach-charset-utf_8");
	call_comm("global-set-command", ed, &utf8_attach, 0, NULL, "attach-utf8");
}
