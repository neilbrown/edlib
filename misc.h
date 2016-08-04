/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Assorted utility functions used by edlib
 *
 */

struct buf {
	char *b safe;
	int len;
	int size;
};

void buf_init(struct buf *b safe);
void buf_concat(struct buf *b safe, char *s safe);
void buf_concat_len(struct buf *b safe, char *s safe, int l);
void buf_append(struct buf *b safe, wchar_t wch);
void buf_append_byte(struct buf *b safe, char c);
static inline char *buf_final(struct buf *b safe) safe
{
	if ((void*)b->b)
		b->b[b->len] = 0;
	return b->b;
}
