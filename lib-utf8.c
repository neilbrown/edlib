/*
 * Copyright Neil Brown ©2017-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Filter a view on a document to convert utf-8 sequences into
 * the relevant unicode characters.
 */

#include <unistd.h>
#include <stdlib.h>

#define DOC_NEXT utf8_next
#define DOC_PREV utf8_prev
#define PANE_DATA_VOID
#include "core.h"

static struct map *utf8_map safe;
DEF_LOOKUP_CMD(utf8_handle, utf8_map);

static inline wint_t utf8_next(struct pane *home safe, struct mark *mark safe,
			       struct doc_ref *r, bool bytes)
{
	int move = r == &mark->ref;
	struct pane *p = home->parent;
	wint_t ch;
	struct mark *m = mark;
	char buf[10];
	const char *b;
	int i;
	wint_t ret;

	if (move)
		ch = doc_move(p, m, 1);
	else
		ch = doc_pending(p, m, 1);
	if (ch == WEOF || (ch & 0x7f) == ch)
		return ch;
	if (!move) {
		m = mark_dup(m);
		doc_move(p, m, 1);
	}
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
	if (!move)
		mark_free(m);
	return ret;
}

static inline wint_t utf8_prev(struct pane *home safe, struct mark *mark safe,
			       struct doc_ref *r, bool bytes)
{
	int move = r == &mark->ref;
	struct pane *p = home->parent;
	wint_t ch;
	struct mark *m = mark;
	char buf[10];
	const char *b;
	int i;
	wint_t ret;

	if (move)
		ch = doc_move(p, m, -1);
	else
		ch = doc_pending(p, m, -1);
	if (ch == WEOF || (ch & 0x7f) == ch)
		return ch;
	if (!move) {
		m = mark_dup(m);
		doc_move(p, m, -1);
	}
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

	if (!move)
		mark_free(m);
	return ret;
}

DEF_CMD(utf8_char)
{
	return do_char_byte(ci);
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
