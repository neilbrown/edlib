/*
 * Copyright Neil Brown ©2016-2021 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Filter a view on a document to make base64 look like the
 * decoded bytes.  A UTF-8 filter would be needed if the base64
 * is actually utf-8.
 *
 * We create our own set of marks and place them at the start of
 * quartets of base64 chars (which means start of triplets of visible chars)
 * We intercept doc:char.
 * To find how to interpret a given position, we find the previous mark,
 * and move forward, counting chars, until we reach the position.
 * Every MAX_QUAD we create a new mark.
 * A position should only ever be on the 1st, 2nd, or 3rd char of a quad,
 * never on the last.
 */

#include <unistd.h>
#include <stdlib.h>
#include <wctype.h>

#include "core.h"

struct b64info {
	int view;
};
#define MAX_QUAD	10

static struct map *b64_map safe;
DEF_LOOKUP_CMD(b64_handle, b64_map);

static int is_b64(wint_t c)
{
	return (c >= 'A' && c <= 'Z') ||
		(c >= 'a' && c <= 'z') ||
		(c >= '0' && c <= '9') ||
		c == '+' || c == '/' || c == '=';
}

static int from_b64(char c)
{
	/* This assumes that 'c' is_b64() */
	if (c <= '+')
		return 62;
	else if (c == '/')
		return 63;
	else if (c <= '9')
		return (c - '0') + 52;
	else if (c == '=')
		return 64;
	else if (c <= 'Z')
		return (c - 'A') + 0;
	else
		return (c - 'a') + 26;
}

static int get_b64_x(struct pane *p safe, struct mark *m safe)
{
	wint_t c;

	while ((c = doc_following(p, m)) != WEOF &&
	       !is_b64(c))
		doc_next(p, m);

	if (c == WEOF)
		return WEOF;
	return from_b64(c);
}

static int get_b64(struct pane *p safe, struct mark *m safe)
{
	wint_t c;
	int ret;

	do {
		c = doc_next(p, m);
	} while (c != WEOF && !is_b64(c));
	if (c == WEOF)
		return WEOF;
	/* Need to leave mark immediately before a b64 char */
	ret = from_b64(c);
	while ((c = doc_following(p, m)) != WEOF && !is_b64(c))
		doc_next(p, m);
	return ret;
}

static int get_b64_rev(struct pane *p safe, struct mark *m safe)
{
	wint_t c;

	do {
		c = doc_prev(p, m);
	} while (c != WEOF && !is_b64(c));
	if (c == WEOF)
		return WEOF;
	return from_b64(c);
}

static int locate_mark(struct pane *p safe, struct pane *owner,
		       int view, struct mark *m safe)
{
	struct mark *st, *tmp, *prev;
	int pos = 0;
	wint_t ch;

	st = vmark_at_or_before(p, m, view, owner);
	if (st) {
		tmp = mark_dup(st);
		prev = st;
	} else {
		tmp = vmark_new(p, MARK_UNGROUPED, NULL);
		pos = (MAX_QUAD + 1) * 4;
		prev = NULL;
	}
	if (!tmp)
		return 0;
	while ((ch = get_b64_x(p, tmp)) != WEOF) {
		if (mark_ordered_or_same(m, tmp))
			break;
		if ((pos %4) == 0 && pos/4 >= MAX_QUAD) {
			struct mark *m2 = vmark_new(p, view, owner);
			if (m2) {
				if (prev)
					/* Moving to a vmark (prev)
					 * first is faster than
					 * directly to tmp.
					 */
					mark_to_mark(m2, prev);
				mark_to_mark(m2, tmp);
				prev = m2;
			}
			pos = 0;
		}
		doc_next(p, tmp); pos++;
	}
	mark_free(tmp);
	return pos%4;
}

DEF_CMD(base64_step)
{
	int pos = 0;
	int forward = ci->num;
	int move = ci->num2;
	struct mark *m;
	struct pane *p = ci->home->parent;
	wint_t c1, c2, b;
	struct b64info *bi = ci->home->data;

	if (!ci->mark)
		return Enoarg;
	pos = locate_mark(p, ci->home, bi->view, ci->mark);

	m = mark_dup(ci->mark);
retry:
	if (forward) {
		c1 = get_b64(p, m);
		c2 = get_b64(p, m);
		/* If we found '=', there is no more to find */
		if (c1 == 64 || c2 == 64) {
			while (pos < 2 && c2 != WEOF) {
				c2 = get_b64(p, m);
				pos -= 1;
			}
			pos = 0;
			if (c2 != WEOF)
				/* hopefully it was 64 aka '=' */
				goto retry;
		}
	} else {
		if (pos)
			get_b64(p, m);
		c2 = get_b64_rev(p, m);
		c1 = get_b64_rev(p, m);
		if (pos <= 0)
			pos = 2;
		else
			pos -= 1;
		while (c2 == 64) {
			c2 = c1;
			c1 = get_b64_rev(p, m);
			if (pos <= 0)
				pos = 1;
			else
				pos -= 1;
		}
	}

	if (c1 == WEOF || c2 == WEOF) {
		mark_free(m);
		return CHAR_RET(WEOF);
	}

	switch(pos) {
	case 0:
		b = (c1 << 2) | (c2 >> 4);
		break;
	case 1:
		b = ((c1 << 4) & 0xF0) | (c2 >> 2);
		break;
	case 2:
		b = ((c1 << 6) & 0xC0) | c2;
		break;
	default:
		b = 0;
	}
	if (forward && pos < 2 && move)
		/* Step back to look at the last char read */
		doc_prev(p, m);
	if (move)
		mark_to_mark(ci->mark, m);

	mark_free(m);
	return CHAR_RET(b);
}

DEF_CMD(base64_char)
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
	while (steps && ret != CHAR_RET(WEOF) && (!end || mark_same(m, end))) {
		ret = comm_call(&base64_step, "", ci->home, forward, m, NULL, 1);
		steps -= forward*2 - 1;
	}
	if (end)
		return 1 + (forward ? ci->num - steps : steps - ci->num);
	if (ret == CHAR_RET(WEOF) || ci->num2 == 0)
		return ret;
	if (ci->num && (ci->num2 < 0) == forward)
		return ret;
	/* Want the 'next' char */
	return comm_call(&base64_step, "", ci->home, ci->num2 > 0, m, NULL, 0);
}

struct b64c {
	struct command c;
	struct command *cb;
	struct pane *p safe;
	struct pane *home safe;
	struct mark *m safe; /* trails 1 b64 char behind main mark */
	int pos;
	int size;
	char c1;
	bool nobulk;
};

static int b64_bulk(struct b64c *c, wchar_t first, const char *s safe, int len)
{
	/* Parse out 4char->3byte section of 's' and then
	 * pass them to c->cb.
	 * Return the number of chars processed.
	 */
	int ret = 0;
	char *out = malloc((len+1)*3/4);
	int b[4];
	int in_pos = 0, out_pos = 0, buf_pos = 0;
	int i;

	if (!out)
		return ret;
	if (is_b64(first))
		b[buf_pos++] = from_b64(first);
	while (len > 0) {
		wint_t wc = (unsigned char)s[in_pos++];
		len -= 1;
		if (!is_b64(wc))
			continue;
		b[buf_pos++] = from_b64(wc);
		if (b[buf_pos-1] == 64)
			break;
		if (buf_pos < 4)
			continue;
		out[out_pos++] = ((b[0] << 2) & 0xFC) | (b[1] >> 4);
		out[out_pos++] = ((b[1] << 4) & 0xF0) | (b[2] >> 2);
		out[out_pos++] = ((b[2] << 6) & 0xC0) | (b[3] >> 0);
		ret = in_pos+1;
		buf_pos = 0;
	}

	/* Now send 'out' to callback */
	i = 0;
	while (i < out_pos) {
		int rv = comm_call(c->cb, "cb", c->p, out[i], c->m,
				   out+i+1, out_pos-i-1, NULL, NULL,
				   c->size, 0);
		c->size = 0;
		if (rv <= 0 || rv > (out_pos - i) + 1) {
			ret = rv;
			c->nobulk = True;
			break;
		}
		i += rv;
		if (i < out_pos)
			/* Only some was consumed, so need to
			 * advance c->m by the amount that
			 * was consumed - in 'home'.
			 */
			call("doc:char", c->home, rv, c->m);
	}
	free(out);
	return ret;
}

DEF_CMD(base64_content_cb)
{
	struct b64c *c = container_of(ci->comm, struct b64c, c);
	wint_t wc = ci->num;
	char c2;
	wint_t b;
	int ret;

	if (!ci->mark)
		return Enoarg;
	if (ci->x)
		c->size = ci->x * 3 / 4;

	if (!c->nobulk && wc != '=' && (c->pos % 4) == 0 &&
	    ci->str && ci->num2 >= 4) {
		mark_to_mark(c->m, ci->mark);
		ret = b64_bulk(c, wc, ci->str, ci->num2);
		if (ret > 0)
			return ret;
	}

	if (!is_b64(wc))
		return 1;
	c2 = from_b64(wc);
	if (c2 == 64) {
		/* We've found a padding '=', that's all folks. */
		c->c1 = 64;
		return Efalse;
	}
	if (c->pos <= 0 || c->pos > 3) {
		c->c1 = c2;
		c->pos = 1;
		mark_to_mark(c->m, ci->mark);
		return 1;
	}
	if (c->c1 == 64) {
		/* This is first b64 */
		c->c1 = c2;
		c->pos = (c->pos + 1) % 4;
		mark_to_mark(c->m, ci->mark);
		return 1;
	}
	/* Have 2 b64 chars, can report one char */
	switch(c->pos) {
	case 1:
		b = (c->c1 << 2) | (c2 >> 4);
		break;
	case 2:
		b = ((c->c1 << 4) & 0xF0) | (c2 >> 2);
		break;
	case 3:
		b = ((c->c1 << 6) & 0xC0) | c2;
		break;
	default:
		b = 0;
	}
	c->pos += 1;
	c->c1 = c2;
	if (c->pos == 4)
		mark_to_mark(c->m, ci->mark);
	ret = comm_call(c->cb, ci->key, c->p, b, c->m, NULL,
			0, NULL, NULL, c->size, 0);
	if (c->pos != 4)
		mark_to_mark(c->m, ci->mark);
	c->size = 0;
	if (ret == Efalse)
		c->c1 = 64;
	return ret;
}

DEF_CMD(base64_content)
{
	struct b64c c;
	struct b64info *bi = ci->home->data;
	int ret;

	if (!ci->comm2 || !ci->mark)
		return Enoarg;
	/* No need to check ->key as providing bytes as chars
	 * is close enough.
	 */

	c.c = base64_content_cb;
	c.nobulk = False;
	c.cb = ci->comm2;
	c.p = ci->focus;
	c.home = ci->home;
	c.pos = locate_mark(ci->home->parent, ci->home, bi->view, ci->mark);
	c.size = 0;
	c.m = mark_dup(ci->mark);
	c.c1 = 64;
	ret = home_call_comm(ci->home->parent, ci->key, ci->home, &c.c,
			     0, ci->mark, NULL, 0, ci->mark2);
	if (c.c1 != 64 && (c.pos % 4) > 0 && ci->mark2) {
		/* We must have reached mark2, but need one more
		 * b64 char.  Skip space if needed to find it.
		 */
		wint_t c2;
		while ((c2 = doc_next(ci->home->parent, c.m)) != WEOF &&
		       iswspace(c2))
			;
		if (c2 != WEOF)
			comm_call(&c.c, "cb", ci->home->parent,
				  c2, ci->mark2);
	}
	mark_free(c.m);
	return ret;
}

DEF_CMD(b64_clip)
{
	struct b64info *bi = ci->home->data;

	marks_clip(ci->home, ci->mark, ci->mark2, bi->view, ci->home, !!ci->num);
	return Efallthrough;
}

DEF_CMD(b64_attach)
{
	struct pane *p;
	struct b64info *bi;

	alloc(bi, pane);
	p = pane_register(ci->focus, 0, &b64_handle.c, bi);
	if (!p) {
		free(bi);
		return Efail;
	}
	bi->view = home_call(ci->focus, "doc:add-view", p) - 1;

	return comm_call(ci->comm2, "callback:attach", p);
}

void edlib_init(struct pane *ed safe)
{

	b64_map = key_alloc();

	key_add(b64_map, "doc:char", &base64_char);
	key_add(b64_map, "doc:byte", &base64_char);
	key_add(b64_map, "doc:content", &base64_content);
	key_add(b64_map, "doc:content-bytes", &base64_content);
	key_add(b64_map, "Free", &edlib_do_free);
	key_add(b64_map, "Notify:clip", &b64_clip);

	call_comm("global-set-command", ed, &b64_attach, 0, NULL, "attach-base64");
}
