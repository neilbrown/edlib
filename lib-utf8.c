/*
 * Copyright Neil Brown ©2017-2023 <neil@brown.name>
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

static int utf8_step(struct pane *home safe, struct mark *mark safe,
		     int num, int num2)
{
	int dir = num ? 1 : -1;
	int move = num2;
	struct pane *p = home->parent;
	wint_t ch;
	struct mark *m = mark;
	char buf[10];
	const char *b;
	int i;
	wint_t ret;

	if (move)
		ch = doc_move(p, m, dir);
	else
		ch = doc_pending(p, m, dir);
	if (ch == WEOF || (ch & 0x7f) == ch)
		return CHAR_RET(ch);
	if (!move) {
		m = mark_dup(m);
		doc_move(p, m, dir);
	}
	if (dir > 0) {
		i = 0;
		buf[i++] = ch;
		while ((ch = doc_following(p, m)) != WEOF &&
		       (ch & 0xc0) == 0x80 && i < 10) {
			buf[i++] = ch;
			doc_next(p, m);
		}
		b = buf;
		ret = get_utf8(&b, b+i);
		if (ret == WERR)
			ret = (unsigned char)buf[0];
	} else {
		i = 10;
		buf[--i] = ch;
		while (ch != WEOF && (ch & 0xc0) != 0xc0 && i > 0) {
			ch = doc_prev(p, m);
			buf[--i] = ch;
		}
		b = buf + i;
		ret = get_utf8(&b, buf+10);
		if (ret == WERR)
			ret = (unsigned char)buf[i];
	}
	if (!move)
		mark_free(m);
	return CHAR_RET(ret);
}

DEF_CMD(utf8_char)
{
	struct mark *m = ci->mark;
	struct mark *end = ci->mark2;
	int steps = ci->num;
	int forward = steps > 0;
	int ret = Einval;

	if (!m)
		return Enoarg;
	if (end && mark_same(m, end))
		return 1;
	if (end && (end->seq < m->seq) != (steps < 0))
		/* Can never cross 'end' */
		return Einval;
	while (steps && ret != CHAR_RET(WEOF) && (!end || !mark_same(m, end))) {
		ret = utf8_step(ci->home, m, forward, 1);
		steps -= forward*2 - 1;
	}
	if (end)
		return 1 + (forward ? ci->num - steps : steps - ci->num);
	if (ret == CHAR_RET(WEOF) || ci->num2 == 0)
		return ret;
	if (ci->num && (ci->num2 < 0) == forward)
		return ret;
	/* Want the 'next' char */
	return utf8_step(ci->home, m, ci->num2 > 0, 0);
}

DEF_CMD(utf8_byte)
{
	return call("doc:char", ci->home->parent, ci->num, ci->mark, ci->str,
		    ci->num2, ci->mark2, ci->str2, ci->x, ci->y);
}

struct utf8cb {
	struct command c;
	struct command *cb safe;
	struct pane *p safe;
	char b[5];
	short have, expect;
	int size;
};

DEF_CMD(utf8_content_cb)
{
	struct utf8cb *c = container_of(ci->comm, struct utf8cb, c);
	wint_t wc = ci->num;
	int ret = 1;

	if (ci->x)
		c->size = ci->x;

	if ((wc & ~0x7f) == 0) {
		/* 7bit char - easy.  Pass following string too,
		 * utf8 is expected.
		 */
		if (c->expect)
			c->expect = c->have = 0;
		ret = comm_call(c->cb, ci->key, c->p, wc, ci->mark, ci->str,
				ci->num2, NULL, NULL, c->size, 0);
		c->size = 0;
		return ret;
	}
	if ((wc & 0xc0) == 0x80) {
		/* Continuation char */
		if (!c->expect)
			/* Ignore it */
			return 1;
		c->b[c->have++] = wc;
		if (c->have >= c->expect) {
			const char *b = c->b;
			wc = get_utf8(&b, b+c->have);
			if (wc == WERR)
				wc = c->b[0];
			c->expect = 0;
			ret = comm_call(c->cb, ci->key, c->p,
					wc, ci->mark, ci->str,
					ci->num2, NULL, NULL, c->size, 0);
			c->size = 0;
		}
		return ret;
	}
	/* First char of multi-byte */
	c->have = 1;
	c->b[0] = wc;

	if (wc < 0xe0)
		c->expect = 2;
	else if (wc < 0xf0)
		c->expect = 3;
	else if (wc < 0xf8)
		c->expect = 4;
	else
		c->expect = 5;
	return 1;
}

DEF_CMD(utf8_content)
{
	struct utf8cb c;

	if (!ci->comm2 || !ci->mark)
		return Enoarg;

	c.c = utf8_content_cb;
	c.cb = ci->comm2;
	c.p = ci->focus;
	c.size = 0;
	c.expect = 0;
	return home_call_comm(ci->home->parent, ci->key, ci->home,
			      &c.c, 1, ci->mark, NULL, 0, ci->mark2);
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

	key_add(utf8_map, "doc:char", &utf8_char);
	key_add(utf8_map, "doc:byte", &utf8_byte);
	key_add(utf8_map, "doc:content", &utf8_content);
	/* No doc:content-bytes, that wouldn't make sense */

	call_comm("global-set-command", ed, &utf8_attach, 0, NULL, "attach-charset-utf-8");
	call_comm("global-set-command", ed, &utf8_attach, 0, NULL, "attach-utf8");
}
