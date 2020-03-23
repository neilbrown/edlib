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
			goto normalize;
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
			ch = '\n';
			goto normalize;
		}
		if (m == ci->mark)
			m = mark_dup(m);
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
			goto normalize;
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
	normalize:
		if (!move)
			return CHAR_RET(ch);
	normalize_more:
		m = ci->mark;
		/* If next is "=\n" we need to skip over it. */
		if (doc_following_pane(p, m) != '=')
			return CHAR_RET(ch);
		m = mark_dup(ci->mark);
		mark_next_pane(p, m);
		while ((c2 = mark_next_pane(p, m)) == ' ' ||
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
		ch = mark_step_pane(p, m, 0, move);
		if (ch == '\n') {
			if (m == ci->mark)
				m = mark_dup(m);
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
	key_add(qp_map, "Free", &edlib_do_free);

	call_comm("global-set-command", ed, &qp_attach, 0, NULL, "attach-quoted_printable");
	call_comm("global-set-command", ed, &qp_attach, 0, NULL, "attach-qprint");
}
