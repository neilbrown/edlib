/*
 * Copyright Neil Brown ©2016-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Filter a view on a document to make base64 look like the
 * decoded bytes.  A UTF-8 filter would be needed if the base64
 * is actually utf-8.
 *
 * Each mark needs not just a location in the b64, but also
 * which byte of a QUAD (4 b64 characters) it is at.
 * We store this in the mark as attribute "b64-pos", which makes
 * stacking b64 impossible - but who would want to?
 * This can have a value "0", "1", "2".  A mark is never on the
 * 4th char of a QUAD.
 * doc:set-ref initialises this as does a mark:arrived notification
 * which references another mark.  doc:char and doc:byte will use
 * the pos to know how to interpret, and will update it after any
 * movement as will doc:content.
 */

#include <unistd.h>
#include <stdlib.h>
#include <wctype.h>

#include "core.h"

static struct map *b64_map safe;
DEF_LOOKUP_CMD(b64_handle, b64_map);

static int is_b64(wint_t c)
{
	return (c >= 'A' && c <= 'Z') ||
		(c >= 'a' && c <= 'z') ||
		(c >= '0' && c <= '9') ||
		c == '+' || c == '/' || c == '=';
}

static wint_t from_b64(char c)
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

static wint_t get_b64(struct pane *p safe, struct mark *m safe)
{
	wint_t c;
	wint_t ret;

	do {
		c = doc_next(p, m);
	} while (c != WEOF && !is_b64(c));
	if (c == WEOF)
		return WEOF;
	ret = from_b64(c);
	while ((c = doc_following(p, m)) != WEOF && !is_b64(c))
		doc_next(p, m);
	return ret;
}

static wint_t get_b64_rev(struct pane *p safe, struct mark *m safe)
{
	wint_t c;

	do {
		c = doc_prev(p, m);
	} while (c != WEOF && !is_b64(c));
	if (c == WEOF)
		return WEOF;
	return from_b64(c);
}

static void set_pos(struct mark *m safe, int pos)
{
	char ps[2] = "0";

	while (pos < 0)
		pos += 4;
	while (pos >= 4)
		pos -= 4;
	ps[0] += pos;
	attr_set_str(&m->attrs, "b64-pos", ps);
	mark_watch(m);
}

static int get_pos(struct mark *m safe)
{
	char *ps = attr_find(m->attrs, "b64-pos");
	if (ps && strlen(ps) == 1 &&
	    ps[0] >= '0' && ps[1] <= '2')
		return ps[0] - '0';
	return -1;
}

static int base64_step(struct pane *home safe, struct mark *mark,
		       int forward, int move)
{
	int pos = 0;
	struct mark *m;
	struct pane *p = home->parent;
	wint_t c1, c2, b;

	if (!mark)
		return Enoarg;
	pos = get_pos(mark);
	if (pos < 0)
		/* bug? */
		pos = 0;

	m = mark_dup(mark);
retry:
	if (forward) {
		c1 = get_b64(p, m);
		c2 = get_b64(p, m);
		/* If we found '=', there is no more to find */
		if (c1 == 64 || c2 == 64) {
			while (pos < 2 && c2 != WEOF) {
				c2 = get_b64(p, m);
				pos += 1;
			}
			pos = 0;
			if (c2 != WEOF)
				/* hopefully it was 64 aka '=' */
				goto retry;
		}
	} else {
		if (pos)
			if (get_b64(p, m) == WEOF)
				pos = 0;
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
				pos = 2;
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
	if (move) {
		if (forward) {
			if (pos < 2)
				/* Step back to look at the last char read */
				doc_prev(p, m);
			else
				pos += 1;
		}
		mark_to_mark(mark, m);
		if (forward)
			set_pos(mark, pos+1);
		else
			set_pos(mark, pos);
	}

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
	while (steps && ret != CHAR_RET(WEOF) && (!end || !mark_same(m, end))) {
		ret = base64_step(ci->home, m, forward, 1);
		steps -= forward*2 - 1;
	}
	if (end)
		return 1 + (forward ? ci->num - steps : steps - ci->num);
	if (ret == CHAR_RET(WEOF) || ci->num2 == 0)
		return ret;
	if (ci->num && (ci->num2 < 0) == forward)
		return ret;
	/* Want the 'next' char */
	return base64_step(ci->home, m, ci->num2 > 0, 0);
}

DEF_CMD(base64_setref)
{
	if (ci->mark)
		/* Start and end are always pos 0 */
		set_pos(ci->mark, 0);
	return Efallthrough;
}

DEF_CMD(base64_arrived)
{
	struct mark *m = ci->mark;
	struct mark *ref = ci->mark2;
	int pos;

	if (!m)
		return 1;

	if (get_pos(m) >= 0)
		/* Interesting mark, keep tracking it */
		mark_watch(m);
	if (!ref)
		return 1;
	pos = get_pos(ref);
	if (pos >= 0)
		set_pos(m, pos);
	return 1;
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

static int b64_bulk(struct b64c *c safe, wchar_t first, const char *s safe, int len)
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
			if (rv <= 0)
				ret = rv;
			c->nobulk = True;
			break;
		}
		i += rv;
		if (i < out_pos)
			/* Only some was consumed, so need to
			 * advance c->m by the amount of chars that
			 * were consumed - in 'home'.
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

	if (!is_b64(wc))
		return 1;
	/* Mark as advances down in the doc so we didn't see it.
	 * Need to explicitly set pos
	 */
	set_pos(ci->mark, (c->pos+1)%4);
	if (!c->nobulk && wc != '=' && (c->pos % 4) == 0 &&
	    ci->str && ci->num2 >= 4) {
		mark_to_mark(c->m, ci->mark);
		ret = b64_bulk(c, wc, ci->str, ci->num2);
		if (ret > 0)
			return ret;
	}

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
	c.pos = get_pos(ci->mark);
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

DEF_CMD(b64_attach)
{
	struct pane *p;

	p = pane_register(ci->focus, 0, &b64_handle.c);
	if (!p)
		return Efail;
	call("doc:request:mark:arrived", p);

	return comm_call(ci->comm2, "callback:attach", p);
}

void edlib_init(struct pane *ed safe)
{

	b64_map = key_alloc();

	key_add(b64_map, "doc:char", &base64_char);
	key_add(b64_map, "doc:byte", &base64_char);
	key_add(b64_map, "doc:content", &base64_content);
	key_add(b64_map, "doc:content-bytes", &base64_content);
	key_add(b64_map, "doc:set-ref", &base64_setref);
	key_add(b64_map, "mark:arrived", &base64_arrived);
	key_add(b64_map, "Free", &edlib_do_free);

	call_comm("global-set-command", ed, &b64_attach, 0, NULL, "attach-base64");
}
