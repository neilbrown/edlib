/*
 * Copyright Neil Brown ©2016-2020 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Filter a view on a document to make quoted-printable look like the
 * decoded bytes.  A UTF-8 filter would be needed if the text
 * is actually utf-8.
 *
 * Chars are passed through except for '=' and following.
 * =HH decodes the hex
 * =\r\n disappears
 * \r\n -> \n
 * space/tab at eol ignored
 *
 * So when stepping backward if we see a \n or hex char we need
 * to look further to see what is really there.
 * When stepping forwards, we need only check for '=' or white space.
 */

#include <unistd.h>
#include <stdlib.h>

#include "core.h"

static struct map *qp_map safe;
DEF_LOOKUP_CMD(qp_handle, qp_map);

static int hex(wint_t c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

DEF_CMD(qp_step)
{
	int forward = ci->num;
	int move = ci->num2;
	struct pane *p = ci->home->parent;
	wint_t ch, c2, c3;
	struct mark *m = ci->mark;

	if (!m)
		return Enoarg;

retry:
	if (forward) {
		ch = doc_step(p, m, 1, move);
		if (ch != '=' && ch != ' ' && ch != '\t' && ch != '\r') {
			if (m != ci->mark) {
				if (move)
					mark_to_mark(ci->mark, m);
				mark_free(m);
			}
			goto normalize;
		}
		if (ch == '\r') {
			/* assume CR-LF */
			if (move)
				doc_next(p, m);
			if (m != ci->mark) {
				if (move)
					mark_to_mark(ci->mark, m);
				mark_free(m);
			}
			ch = '\n';
			goto normalize;
		}
		if (m == ci->mark)
			m = mark_dup(m);
		if (!move)
			doc_next(p, m);
		if (ch == '=') {
			/* CRLF or HexHex expected. */
			c2 = doc_next(p, m);
			if (c2 == '\n')
				goto retry;
			c3 = doc_next(p, m);
			if (c2 == '\r' && c3 == '\n')
				goto retry;
			if (hex(c2) >= 0 && hex(c3) >= 0) {
				ch = hex(c2)*16 + hex(c3);
				if (move)
					mark_to_mark(ci->mark, m);
			}
			mark_free(m);
			goto normalize;
		}
		/* Whitespace, ignore if at eol */
		if (move)
			mark_to_mark(ci->mark, m);
		while ((c2 = doc_next(p, m)) == ' ' || c2 == '\t')
			;
		if (c2 == '\r')
			/* Found the white-space, retry from here and see the '\n' */
			goto retry;
		if (c2 == '\n') {
			/* No \r, just \n.  Step back to see it */
			doc_prev(p, m);
			goto retry;
		}
		/* Just normal white space */
		mark_free(m);
	normalize:
		if (!move)
			return CHAR_RET(ch);
	normalize_more:
		m = ci->mark;
		/* If next is "=\n" we need to skip over it. */
		if (doc_following(p, m) != '=')
			return CHAR_RET(ch);
		m = mark_dup(ci->mark);
		doc_next(p, m);
		while ((c2 = doc_next(p, m)) == ' ' ||
		       c2 == '\t' || c2 == '\r')
			;
		if (c2 != '\n') {
			/* Don't need to skip this */
			mark_free(m);
			return CHAR_RET(ch);
		}
		mark_to_mark(ci->mark, m);
		mark_free(m);
		goto normalize_more;
	} else {
		ch = doc_step(p, m, 0, move);
		if (ch == '\n') {
			if (m == ci->mark)
				m = mark_dup(m);
			if (!move)
				doc_prev(p, m);
			/* '\n', skip '\r' and white space */
			while ((ch = doc_prior(p, m)) == '\r' ||
			       ch == ' ' || ch == '\t')
				doc_prev(p, m);
			if (ch == '=') {
				doc_prev(p, m);
				goto retry;
			}
			if (move)
				mark_to_mark(ci->mark, m);
			mark_free(m);
			return CHAR_RET('\n');
		}
		if (hex(ch) < 0) {
			if (m != ci->mark) {
				if (move)
					mark_to_mark(ci->mark, m);
				mark_free(m);
			}
			return CHAR_RET(ch);
		}
		if (m == ci->mark)
			m = mark_dup(m);
		else if (move)
			mark_to_mark(ci->mark, m);
		if (!move)
			doc_prev(p, m);

		/* Maybe =HH */
		c3 = ch;
		c2 = doc_prev(p, m);
		if (hex(c2) >= 0) {
			wint_t ceq = doc_prev(p, m);
			if (ceq == '=') {
				/* =HH */
				ch = hex(c2)*16 + hex(c3);
				if (move)
					mark_to_mark(ci->mark, m);
				mark_free(m);
				return CHAR_RET(ch);
			}
		}
		mark_free(m);
		return CHAR_RET(ch);
	}
}

struct qpcb {
	struct command c;
	struct command *cb safe;
	struct pane *p safe;
	char state; /* \0 or '=' or hexit */
	int size;
	struct buf lws;
};

static void qpflush(struct qpcb *c safe, const struct cmd_info *ci, wint_t ch)
{
	char *lws = buf_final(&c->lws);

	while (*lws) {
		comm_call(c->cb, ci->key, c->p, *lws, ci->mark, NULL,
			  0, NULL, NULL, c->size, 0);
		c->size = 0;
		lws += 1;
	}
	buf_reinit(&c->lws);
	comm_call(c->cb, ci->key, c->p, ch, ci->mark, NULL,
		  0, NULL, NULL, c->size, 0);
	c->size = 0;
}

DEF_CMD(qp_content_cb)
{
	struct qpcb *c = container_of(ci->comm, struct qpcb, c);
	wint_t wc = ci->num;

	if (ci->x)
		c->size = ci->x;

	if (c->state && c->state != '=') {
		/* Must see a hexit */
		int h = hex(wc);
		if (h >= 0) {
			qpflush(c, ci,  (hex(c->state) << 4) | h);
			c->state = 0;
			return 1;
		}
		/* Pass first 2 literally */
		qpflush(c, ci, '=');
		qpflush(c, ci, c->state);
		c->state = 0;
	}

	if (wc == '\r')
		/* Always skip \r */
		return 1;
	if (!c->state) {
		if (wc == '=') {
			c->state = wc;
			return 1;
		}
		if (wc == ' ' || wc == '\t') {
			buf_append(&c->lws, wc);
			return 1;
		}
		if (wc == '\n')
			/* drop any trailing space */
			buf_reinit(&c->lws);
		qpflush(c, ci, wc);
		return 1;
	}
	/* Previous was '='. */
	if (hex(wc) >= 0) {
		c->state = wc;
		return 1;
	}
	if (wc == ' ' || wc == '\t')
		/* Ignore space after =, incase at eol */
		return 1;
	c->state = 0;
	if (wc == '\n')
		/* The '=' was hiding the \n */
		return 1;
	qpflush(c, ci, '=');
	qpflush(c, ci, wc);
	return 1;
}

DEF_CMD(qp_content)
{
	struct qpcb c;
	int ret;

	if (!ci->comm2 || !ci->mark)
		return Enoarg;
	/* No need to check ->num as providing bytes as chars
	 * is close enough.
	 */

	c.c = qp_content_cb;
	c.cb = ci->comm2;
	c.p = ci->focus;
	c.size = 0;
	c.state = 0;
	buf_init(&c.lws);
	ret = home_call_comm(ci->home->parent, ci->key, ci->focus,
			     &c.c, 0, ci->mark, NULL, 0, ci->mark2);
	free(c.lws.b);
	return ret;
}

DEF_CMD(qp_attach)
{
	struct pane *p;

	p = pane_register(ci->focus, 0, &qp_handle.c);
	if (!p)
		return Efail;

	return comm_call(ci->comm2, "callback:attach", p);
}

void edlib_init(struct pane *ed safe)
{

	qp_map = key_alloc();

	key_add(qp_map, "doc:step", &qp_step);
	key_add(qp_map, "doc:step-bytes", &qp_step);
	key_add(qp_map, "doc:content", &qp_content);

	call_comm("global-set-command", ed, &qp_attach, 0, NULL, "attach-quoted_printable");
	call_comm("global-set-command", ed, &qp_attach, 0, NULL, "attach-qprint");
}
