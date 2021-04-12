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
	if (!move) {
		m = mark_dup(m);
		doc_step(p, m, forward, 1);
	}
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

	if (ci->x)
		c->size = ci->x;

	if ((wc & ~0x7f) == 0) {
		/* 7bit char - easy */
		if (c->expect)
			c->expect = c->have = 0;
		comm_call(c->cb, ci->key, c->p, wc, ci->mark, NULL,
			  0, NULL, NULL, c->size, 0);
		c->size = 0;
		return 1;
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
			c->expect = 0;
			comm_call(c->cb, ci->key, c->p, wc, ci->mark, NULL,
				  0, NULL, NULL, c->size, 0);
			c->size = 0;
		}
		return 1;
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
	if (ci->num)
		/* Fall through and let parent provide bytes */
		return Efallthrough;

	c.c = utf8_content_cb;
	c.cb = ci->comm2;
	c.p = ci->focus;
	c.size = 0;
	c.expect = 0;
	return home_call_comm(ci->home->parent, ci->key, ci->focus,
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

	key_add(utf8_map, "doc:step", &utf8_step);
	key_add(utf8_map, "doc:content", &utf8_content);

	call_comm("global-set-command", ed, &utf8_attach, 0, NULL, "attach-charset-utf_8");
	call_comm("global-set-command", ed, &utf8_attach, 0, NULL, "attach-utf8");
}
