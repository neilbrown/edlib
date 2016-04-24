/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Assorted utility functions used by edlib
 *
 */
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "misc.h"

void buf_init(struct buf *b)
{
	b->b = NULL;
	b->len = 0;
	b->size = 0;
}

void buf_concat_len(struct buf *b, char *s, int l)
{

	if (b->len + l >= b->size) {
		while (b->len + l >= b->size)
			b->size += 128;
		b->b = realloc(b->b, b->size);
	}
	strncpy(b->b + b->len, s, l);
	b->len += l;
	b->b[b->len] = 0;
}

void buf_concat(struct buf *b, char *s)
{
	int l = strlen(s);
	buf_concat_len(b, s, l);
}

void buf_append(struct buf *b, wchar_t wch)
{
	char t[5];
	mbstate_t ps = {};
	size_t l;

	if (wch <= 0x7f) {
		t[0] = wch;
		l = 1;
	} else
		l = wcrtomb(t, wch, &ps);
	buf_concat_len(b, t, l);
}

void buf_append_byte(struct buf *b, char c)
{
	buf_concat_len(b, &c, 1);
}
