/*
 * Copyright Neil Brown ©2016 <neil@brown.name>
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

	if (!m || !p)
		return 0;

	retry:
	if (forward) {
		ch = mark_step_pane(p, m, 1, move);
		if (ch != '=' && ch != ' ' && ch != '\t' && ch != '\r') {
			if (m != ci->mark) {
				if (move)
					mark_to_mark(ci->mark, m);
				mark_free(m);
			}
			return CHAR_RET(ch);
		}
		if (ch == '\r') {
			/* assume CR-LF */
			if (move)
				mark_next_pane(p, m);
			if (m != ci->mark) {
				if (move)
					mark_to_mark(ci->mark, m);
				mark_free(m);
			}
			return CHAR_RET('\n');
		}
		if (m == ci->mark)
			m = mark_dup(m, 1);
		if (!move)
			mark_next_pane(p, m);
		if (ch == '=') {
			/* CRLF or HexHex expected. */
			c2 = mark_next_pane(p, m);
			if (c2 == '\n')
				goto retry;
			c3 = mark_next_pane(p, m);
			if (c2 == '\r' && c3 == '\n')
				goto retry;
			if (hex(c2) >= 0 && hex(c3) >= 0) {
				ch = hex(c2)*16 + hex(c3);
				if (move)
					mark_to_mark(ci->mark, m);
			}
			mark_free(m);
			return CHAR_RET(ch);
		}
		/* Whitespace, ignore if at eol */
		if (move)
			mark_to_mark(ci->mark, m);
		while ((c2 = mark_next_pane(p, m)) == ' ' || c2 == '\t')
			;
		if (c2 == '\r')
			/* Found the white-space, retry from here and see the '\n' */
			goto retry;
		if (c2 == '\n') {
			/* No \r, just \n.  Step back to see it */
			mark_prev_pane(p, m);
			goto retry;
		}
		/* Just normal white space */
		mark_free(m);
		return CHAR_RET(ch);
	} else {
		ch = mark_step_pane(p, m, 0, move);
		if (ch == '\n') {
			if (m == ci->mark)
				m = mark_dup(m, 1);
			if (!move)
				mark_prev_pane(p, m);
			/* '\n', skip '\r' and white space */
			while ((ch = doc_prior_pane(p, m)) == '\r' ||
			       ch == ' ' || ch == '\t')
				mark_prev_pane(p, m);
			if (ch == '=') {
				mark_prev_pane(p, m);
				goto retry;
			}
			if (move)
				mark_to_mark(ci->mark, m);
			mark_free(m);
			return CHAR_RET('\n');
		}
		if (hex(ch) < 0)
			return CHAR_RET(ch);
		if (m == ci->mark)
			m = mark_dup(m, 1);
		else if (move)
			mark_to_mark(ci->mark, m);
		if (!move)
			mark_prev_pane(p, m);

		/* Maybe =HH */
		c3 = ch;
		c2 = mark_prev_pane(p, m);
		if (hex(c2) >= 0) {
			wint_t ceq = mark_prev_pane(p, m);
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

DEF_CMD(qp_same)
{
	struct pane *p = ci->home->parent;
	struct mark *m1 = ci->mark;
	struct mark *m2 = ci->mark2;

	if (!p || !m1 || !m2)
		return -1;
	if (m1 == m2)
		return 1;
	if (mark_same_pane(p, m1, m2))
		return 1;
	/* If the only thing separating the earlier one from the later one
	 * is "=space\r\n" sequences, then they are the same
	 */
	if (m1->seq > m2->seq) {
		struct mark *m = m1;
		m1 = m2;
		m2 = m;
	}
	if (doc_following_pane(p, m1) != '=')
		return 2;
	m1 = mark_dup(m1, 1);
	while (1) {
		wint_t ch = doc_following_pane(p, m1);
		if (ch != '=')
			break;
		mark_next_pane(p, m1);
		while (m1->seq < m2->seq &&
		       ((ch = doc_following_pane(p, m1)) == ' ' ||
			ch == '\t' || ch == '\r')) {
			mark_next_pane(p, m1);
			continue;
		}
		if (ch != '\n')
			break;
		mark_next_pane(p, m1);
	}
	if (m1->seq > m2->seq || mark_same_pane(p, m1, m2)) {
		mark_free(m1);
		return 1;
	}
	mark_free(m1);
	return 2;
}

DEF_CMD(qp_attach)
{
	struct pane *p;

	p = pane_register(ci->focus, 0, &qp_handle.c, NULL, NULL);
	if (!p)
		return -1;
	call("doc:set:filter", p, 1);

	return comm_call(ci->comm2, "callback:attach", p);
}

void edlib_init(struct pane *ed safe)
{

	qp_map = key_alloc();

	key_add(qp_map, "doc:step", &qp_step);
	key_add(qp_map, "doc:mark-same", &qp_same);

	call_comm("global-set-command", ed, 0, NULL, "attach-quoted_printable",
		  &qp_attach);
	call_comm("global-set-command", ed, 0, NULL, "attach-qprint",
		  &qp_attach);
}
