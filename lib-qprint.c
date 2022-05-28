/*
 * Copyright Neil Brown ©2016-2022 <neil@brown.name>
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

static int qp_step(struct pane *home safe, struct mark *mark safe,
		   int num, int num2)
{
	int forward = num;
	int move = num2;
	struct pane *p = home->parent;
	wint_t ch, c2, c3;
	struct mark *m = mark;

	if (!m)
		return Enoarg;

retry:
	if (forward) {
		if (move)
			ch = doc_next(p, m);
		else
			ch = doc_following(p, m);
		if (ch != '=' && ch != ' ' && ch != '\t' && ch != '\r') {
			if (m != mark) {
				if (move)
					mark_to_mark(mark, m);
				mark_free(m);
			}
			goto normalize;
		}
		if (ch == '\r') {
			/* assume CR-LF - skip an extra char */
			if (move)
				doc_next(p, m);
			if (m != mark) {
				if (move)
					mark_to_mark(mark, m);
				mark_free(m);
			}
			ch = '\n';
			goto normalize;
		}
		if (m == mark)
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
					mark_to_mark(mark, m);
			}
			mark_free(m);
			goto normalize;
		}
		/* Whitespace, ignore if at eol */
		if (move)
			mark_to_mark(mark, m);
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
		m = mark;
		/* If next is "=\n" we need to skip over it. */
		if (doc_following(p, m) != '=')
			return CHAR_RET(ch);
		m = mark_dup(mark);
		doc_next(p, m);
		while ((c2 = doc_next(p, m)) == ' ' ||
		       c2 == '\t' || c2 == '\r')
			;
		if (c2 != '\n') {
			/* Don't need to skip this */
			mark_free(m);
			return CHAR_RET(ch);
		}
		mark_to_mark(mark, m);
		mark_free(m);
		goto normalize_more;
	} else {
		if (move)
			ch = doc_prev(p, m);
		else
			ch = doc_prior(p, m);
		if (ch == '\n') {
			if (m == mark)
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
				mark_to_mark(mark, m);
			mark_free(m);
			return CHAR_RET('\n');
		}
		if (hex(ch) < 0) {
			if (m != mark) {
				if (move)
					mark_to_mark(mark, m);
				mark_free(m);
			}
			return CHAR_RET(ch);
		}
		if (m == mark)
			m = mark_dup(m);
		else if (move)
			mark_to_mark(mark, m);
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
					mark_to_mark(mark, m);
				mark_free(m);
				return CHAR_RET(ch);
			}
		}
		mark_free(m);
		return CHAR_RET(ch);
	}
}

DEF_CMD(qp_char)
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
		ret = qp_step(ci->home, m, forward, 1);
		steps -= forward*2 - 1;
	}
	if (end)
		return 1 + (forward ? ci->num - steps : steps - ci->num);
	if (ret == CHAR_RET(WEOF) || ci->num2 == 0)
		return ret;
	if (ci->num && (ci->num2 < 0) == forward)
		return ret;
	/* Want the 'next' char */
	return qp_step(ci->home, m, ci->num2 > 0, 0);
}

struct qpcb {
	struct command c;
	struct command *cb safe;
	struct pane *p safe;
	char state; /* \0 or '=' or hexit */
	int size;
	struct buf lws;
	struct mark *lws_start; /* after first lws char */
};

static int qpflush(struct qpcb *c safe, const struct cmd_info *ci, wint_t ch,
		   const char *remainder, int rlen)
{
	char *lws = buf_final(&c->lws);
	int lws_len = c->lws.len;
	int ret = 1;
	int i;

	while (ret > 0 && lws_len > 0 && c->lws_start) {
		ret = comm_call(c->cb, ci->key, c->p, *lws, c->lws_start, lws+1,
				lws_len - 1, NULL, NULL, c->size, 0);
		doc_next(ci->home, c->lws_start);
		c->size = 0;
		if (ret > 0) {
			lws += ret;
			lws_len -= ret;
		}
	}
	buf_reinit(&c->lws);
	mark_free(c->lws_start);
	c->lws_start = NULL;
	if (!ch)
		return ret;
	for (i = 0; remainder && i < rlen; i++)
		if (strchr("=\r\n", remainder[i])) {
			rlen = i;
			if (remainder[i] == '=')
				break;
			/* ignore trailing white space */
			while (i > 0 && remainder[i] <= ' ')
				i -= 1;
			rlen = i;
		}
	if (ret > 0)
		ret = comm_call(c->cb, ci->key, c->p, ch, ci->mark, remainder,
				rlen, NULL, NULL, c->size, 0);
	c->size = 0;
	return ret;
}

DEF_CMD(qp_content_cb)
{
	struct qpcb *c = container_of(ci->comm, struct qpcb, c);
	wint_t wc = ci->num;
	int ret = 1;

	if (!ci->mark)
		return Enoarg;
	if (ci->x)
		c->size = ci->x;

	if (c->state && c->state != '=') {
		/* Must see a hexit */
		int h = hex(wc);
		if (h >= 0) {
			ret = qpflush(c, ci,  (hex(c->state) << 4) | h, NULL, 0);
			c->state = 0;
			return ret;
		}
		/* Pass first 2 literally */
		ret = qpflush(c, ci, '=', NULL, 0);
		if (ret > 0)
			ret = qpflush(c, ci, c->state, NULL, 0);
		c->state = 0;
	}

	if (wc == '\r')
		/* Always skip \r */
		return ret;
	if (!c->state) {
		if (wc == '=') {
			/* flush lws even if this turns out to be "=\n    \n" */
			if (ret)
				ret = qpflush(c, ci, 0, NULL, 0);
			c->state = wc;
			return ret;
		}
		if (wc == ' ' || wc == '\t') {
			if (!c->lws_start)
				c->lws_start = mark_dup(ci->mark);
			buf_append(&c->lws, wc);
			return ret;
		}
		if (wc == '\n') {
			/* drop any trailing space */
			buf_reinit(&c->lws);
			mark_free(c->lws_start);
			c->lws_start = NULL;
		}
		if (ret > 0)
			ret = qpflush(c, ci, wc, ci->str, ci->num2);
		return ret;
	}
	/* Previous was '='. */
	if (hex(wc) >= 0) {
		c->state = wc;
		return ret;
	}
	if (wc == ' ' || wc == '\t')
		/* Ignore space after =, incase at eol */
		return ret;
	c->state = 0;
	if (wc == '\n')
		/* The '=' was hiding the \n */
		return ret;
	if (ret > 0)
		ret = qpflush(c, ci, '=', NULL, 0);
	if (ret > 0)
		ret = qpflush(c, ci, wc, NULL, 0);
	return ret;
}

DEF_CMD(qp_content)
{
	struct qpcb c;
	int ret;

	if (!ci->comm2 || !ci->mark)
		return Enoarg;
	/* No need to check ->key as providing bytes as chars
	 * is close enough.
	 */

	c.c = qp_content_cb;
	c.cb = ci->comm2;
	c.p = ci->focus;
	c.size = 0;
	c.state = 0;
	buf_init(&c.lws);
	c.lws_start = NULL;
	ret = home_call_comm(ci->home->parent, ci->key, ci->home,
			     &c.c, 0, ci->mark, NULL, 0, ci->mark2);
	free(c.lws.b);
	mark_free(c.lws_start);
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

	key_add(qp_map, "doc:char", &qp_char);
	key_add(qp_map, "doc:byte", &qp_char);
	key_add(qp_map, "doc:content", &qp_content);
	key_add(qp_map, "doc:content-bytes", &qp_content);

	call_comm("global-set-command", ed, &qp_attach, 0, NULL, "attach-quoted_printable");
	call_comm("global-set-command", ed, &qp_attach, 0, NULL, "attach-qprint");
}
