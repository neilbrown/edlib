/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Assorted utility functions used by edlib
 *
 */

struct buf {
	char *b;
	int len;
	int size;
};

void buf_init(struct buf *b);
void buf_concat(struct buf *b, char *s);
void buf_concat_len(struct buf *b, char *s, int l);
void buf_append(struct buf *b, wchar_t wch);
void buf_append_byte(struct buf *b, char c);
static inline char *buf_final(struct buf *b)
{
	if (b->b)
		b->b[b->len] = 0;
	return b->b;
}
