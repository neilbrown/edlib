/*
 * Copyright Neil Brown ©2016-2021 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * lib-rfc822header: parse rfc822 email headers.
 * When instanciated, headers in the parent document are parsed and a mark
 * is moved beyond the headers.
 * Subsequently the "get-header" command and be used to extract headers.
 * If a focus/point is given, the header is copied into the target pane
 * with charset decoding performed and some attributes added to allow
 * control over the display.
 * If no point is given, the named header is parsed and added to this
 * pane as an attribute. Optionally comments are removed.
 *
 * RFC2047 allows headers to contains words:
 *  =?charset?encoding?text?=
 *  "charset" can be an set, e.g. "iso-8859-1" "utf-8" "us-ascii" "Windows-1252"
 *     Currently support utf-8 and us-ascii transparently, others if
 *     a converter exists.
 *  "encoding" can be Q or B (or q or b)
 *     Q recognizes '=' and treat next 2 has HEX, and '_' implies SPACE
 *     B is base64.
 */

#define _GNU_SOURCE /*  for asprintf */
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "core.h"
#include "misc.h"

struct header_info {
	int vnum;
};

DEF_CMD(header_close)
{
	struct pane *p = ci->home;
	struct header_info *hi = p->data;
	struct mark *m;

	while ((m = vmark_first(p, hi->vnum, p)) != NULL)
		mark_free(m);
	call("doc:del-view", p, hi->vnum);
	return 1;
}

static char *get_hname(struct pane *p safe, struct mark *m safe)
{
	char hdr[80];
	int len = 0;
	wint_t ch;

	while ((ch = doc_next(p, m)) != ':' &&
	       (ch > ' ' && ch <= '~')) {
		hdr[len++] = ch;
		if (len > 77)
			break;
	}
	hdr[len] = 0;
	if (len == 0 || ch != ':')
		return NULL;
	return strdup(hdr);
}

static void find_headers(struct pane *p safe, struct mark *start safe,
			 struct mark *end safe)
{
	struct header_info *hi = p->data;
	struct mark *m, *hm safe;
	wint_t ch;
	char *hname;

	m = vmark_new(p, hi->vnum, p);
	if (!m)
		return;
	mark_to_mark(m, start);
	hm = mark_dup_view(m);
	while (m->seq < end->seq &&
	       (hname = get_hname(p, m)) != NULL) {
		attr_set_str(&hm->attrs, "header", hname);
		free(hname);
		while ((ch = doc_next(p, m)) != WEOF &&
		       m->seq < end->seq &&
		       (ch != '\n' ||
			(ch = doc_following(p, m)) == ' ' || ch == '\t'))
			;
		hm = mark_dup_view(m);
	}
	/* Skip over trailing blank line */
	if (doc_following(p, m) == '\r')
		doc_next(p, m);
	if (doc_following(p, m) == '\n')
		doc_next(p, m);
	mark_to_mark(start, m);
	mark_free(m);
}

static int from_hex(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return 10 + c - 'a';
	if (c >= 'A' && c <= 'F')
		return 10 + c - 'A';
	return 0;
}

static int is_b64(char c)
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
	else if (c <= '9')
		return (c - '0') + 52;
	else if (c == '=')
		return 64;
	else if (c <= 'Z')
		return (c - 'A') + 0;
	else if (c == '/')
		return 63;
	else
		return (c - 'a') + 26;
}

static char *safe charset_word(struct pane *doc safe, struct mark *m safe)
{
	/* RFC2047 decoding.
	 * Search for second '?' and capture charset, detect 'Q' or 'B',
	 * then decode based on that.
	 * Finish on ?= or non-printable
	 * =?charset?encoding?code?=
	 */
	struct buf buf;
	int qmarks = 0;
	char code = 0;
	int bits = -1;
	int tmp = 0;
	static char *last = NULL;
	char *charset = NULL;
	wint_t ch;
	struct mark *m2;

	free(last);
	last = NULL;

	buf_init(&buf);
	while ((ch = doc_next(doc, m)) != WEOF &&
	       ch > ' ' && ch < 0x7f && qmarks < 4) {
		if (ch == '?') {
			if (qmarks == 2) {
				charset = buf_final(&buf);
				buf_init(&buf);
			}
			qmarks++;
			continue;
		}
		if (qmarks < 3 && isupper(ch))
			ch = tolower(ch);
		if (qmarks == 1) {
			/* gathering charset */
			buf_append(&buf, ch);
			continue;
		}
		if (qmarks == 2 && ch == 'q')
			code = 'q';
		if (qmarks == 2 && ch == 'b')
			code = 'b';
		if (qmarks != 3)
			continue;
		switch(code) {
		default:
			buf_append(&buf, ch);
			break;
		case 'q':
			if (bits >= 0) {
				tmp = (tmp<<4) + from_hex(ch);
				bits += 4;
				if (bits == 8) {
					buf_append_byte(&buf, tmp);
					tmp = 0;
					bits = -1;
				}
				break;
			}
			switch(ch) {
			default:
				buf_append(&buf, ch);
				break;
			case '_':
				buf_append(&buf, ' ');
				break;
			case '=':
				tmp = 0;
				bits = 0;
				break;
			}
			break;

		case 'b':
			if (bits < 0) {
				bits = 0;
				tmp = 0;
			}
			if (!is_b64(ch) || ch == '=')
				break;
			tmp = (tmp << 6) | from_b64(ch);
			bits += 6;
			if (bits >= 8) {
				bits -= 8;
				buf_append_byte(&buf, (tmp >> bits) & 255);
				tmp &= (1<<bits)-1;
			}
			break;
		}
	}
	last = buf_final(&buf);
	if (charset && last) {
		char *cmd = NULL;
		char *cvt = NULL;

		asprintf(&cmd, "charset-to-utf8-%s", charset);
		if (cmd)
			cvt = call_ret(str, cmd, doc, 0, NULL, last);
		if (cvt) {
			free(last);
			last = cvt;
		}
	}
	/* If there is only LWS to the next quoted word,
	 * skip that so words join up
	 */
	m2 = mark_dup(m);
	if (!m2)
		return last;
	while ((ch = doc_next(doc, m2)) == ' ' ||
	       ch == '\t' || ch == '\r' || ch == '\n')
		;
	if (ch == '=' && doc_following(doc, m2) == '?') {
		doc_prev(doc, m2);
		mark_to_mark(m, m2);
	}
	mark_free(m2);
	return last;
}

static void copy_header(struct pane *doc safe,
			const char *hdr safe, const char *type,
			struct mark *start safe, struct mark *end safe,
			struct pane *p safe, struct mark *point safe)
{
	/* Copy the header in 'doc' from 'start' to 'end' into
	 * the document 'p' at 'point'.
	 * 'type' can be:
	 *  NULL : no explicit wrapping
	 *  "text": add wrap points between words
	 *  "list": convert commas to wrap points.
	 * 'hdr' is the name of the header -  before the ':'.
	 * '\n', '\r' are copied as a single space, and subsequent
	 * spaces are skipped.
	 */
	struct mark *m;
	struct mark *hstart;
	int sol = 0;
	char buf[20];
	wint_t ch;
	char attr[100];
	int is_text = type && strcmp(type, "text") == 0;
	int is_list = type && strcmp(type, "list") == 0;

	m = mark_dup(start);
	hstart = mark_dup(point);
	/* put hstart before point, so it stays here */
	mark_step(hstart, 0);
	while ((ch = doc_next(doc, m)) != WEOF &&
	       m->seq < end->seq) {
		char *b;
		int i;

		if (ch < ' ' && ch != '\t') {
			sol = 1;
			continue;
		}
		if (sol && (ch == ' ' || ch == '\t'))
			continue;
		if (sol) {
			call("doc:replace", p, 1, NULL, " ", 0, point,
			     is_text ? ",render:rfc822header-wrap=1" : NULL);
			sol = 0;
		}
		buf[0] = ch;
		buf[1] = 0;
		if (ch == '=' && doc_following(doc, m) == '?')
			b = charset_word(doc, m);
		else
			b = buf;
		for (i = 0; b[i]; i++)
			if (b[i] > 0 && b[i] < ' ')
				b[i] = ' ';
		call("doc:replace", p, 1, NULL, b, 0, point);
		if (ch == ',' && is_list) {
			/* This comma is no in a quoted word, so it really marks
			 * part of a list, and together with following spaces
			 * is a wrap-point.
			 */
			struct mark *p2 = mark_dup(point);
			int cnt = 1;
			doc_prev(p, p2);
			while ((ch = doc_following(doc, m)) == ' ') {
				call("doc:replace", p, 1, NULL, " ", 0, point);
				doc_next(doc, m);
				cnt += 1;
			}
			if (ch == '\n' || ch == '\r')
				cnt += 1;
			snprintf(buf, sizeof(buf), "%d", cnt);
			call("doc:set-attr", p, 1, p2, "render:rfc822header-wrap", 0, NULL, buf);
			mark_free(p2);
		}
	}
	if (is_text) {
		/* flag all space as wrapping.  We didn't do this before
		 * as they might have been hiding in quoted words.
		 */
		struct mark *m2 = mark_dup(hstart);
		while (m2->seq < point->seq && !mark_same(m2, point)) {
			struct mark *ms;
			int cnt = 1;
			int c = doc_next(p, m2);
			if (c != ' ' && c != '\t')
				continue;
			ms = mark_dup(m2);
			doc_prev(p, ms);
			while ((c = doc_following(p, m2)) == ' ' ||
			       c == '\t') {
				cnt += 1;
				doc_next(p, m2);
			}
			snprintf(buf, sizeof(buf), "%d", cnt);
			call("doc:set-attr", p, 1, ms, "render:rfc822header-wrap",
			     0, NULL, buf);
		}
	}
	call("doc:replace", p, 1, NULL, "\n", 0, point);
	snprintf(buf, sizeof(buf), "%zd", strlen(hdr)+1);
	call("doc:set-attr", p, 1, hstart, "render:rfc822header", 0, NULL, buf);
	snprintf(attr, sizeof(attr), "render:rfc822header-%s", hdr);
	call("doc:set-attr", p, 1, hstart, attr, 0, NULL, "10000");

	mark_free(hstart);
	mark_free(m);
}

static void copy_headers(struct pane *p safe, const char *hdr safe,
			 const char *type,
			 struct pane *doc safe, struct mark *pt safe)
{
	struct header_info *hi = p->data;
	struct mark *m, *n;

	for (m = vmark_first(p, hi->vnum, p); m ; m = n) {
		char *h = attr_find(m->attrs, "header");
		n = vmark_next(m);
		if (n && h && strcasecmp(h, hdr) == 0)
			copy_header(p, hdr, type, m, n, doc, pt);
	}
}

static char *extract_header(struct pane *p safe, struct mark *start safe,
			    struct mark *end safe)

{
	/* This is used for headers that control parsing, such as
	 * MIME-Version and Content-type.
	 */
	struct mark *m;
	int sol = 0;
	int found = 0;
	struct buf buf;
	wint_t ch;

	buf_init(&buf);
	m = mark_dup(start);
	while ((ch = doc_next(p, m)) != WEOF &&
	       m->seq < end->seq) {
		if (!found && ch == ':') {
			found = 1;
			continue;
		}
		if (!found)
			continue;
		if (ch < ' ' && ch != '\t') {
			sol = 1;
			continue;
		}
		if (sol && (ch == ' ' || ch == '\t'))
			continue;
		if (sol) {
			buf_append(&buf, ' ');
			sol = 0;
		}
		if (ch == '=' && doc_following(p, m) == '?') {
			char *b = charset_word(p, m);
			buf_concat(&buf, b);
		} else
			buf_append(&buf, ch);
	}
	return buf_final(&buf);
}

static char *load_header(struct pane *home safe, const char *hdr safe)
{
	struct header_info *hi = home->data;
	struct mark *m, *n;

	for (m = vmark_first(home, hi->vnum, home); m; m = n) {
		char *h = attr_find(m->attrs, "header");
		n = vmark_next(m);
		if (n && h && strcasecmp(h, hdr) == 0)
			return extract_header(home, m, n);
	}
	return NULL;
}

DEF_CMD(header_get)
{
	const char *hdr = ci->str;
	const char *type = ci->str2;
	char *attr = NULL;
	char *c, *t;

	if (!hdr)
		return Enoarg;

	if (ci->mark) {
		copy_headers(ci->home, hdr, type, ci->focus, ci->mark);
		return 1;
	}
	asprintf(&attr, "rfc822-%s", hdr);
	if (!attr)
		return Efail;
	for (c = attr; *c; c++)
		if (isupper(*c))
			*c = tolower(*c);
	t = load_header(ci->home, hdr);
	attr_set_str(&ci->home->attrs, attr, t);
	free(attr);
	free(t);
	return t ? 1 : 2;
}

DEF_CMD(header_clip)
{
	struct header_info *hi = ci->home->data;

	marks_clip(ci->home, ci->mark, ci->mark2, hi->vnum, ci->home, !!ci->num);
	return Efallthrough;
}

static struct map *header_map safe;

static void header_init_map(void)
{
	header_map = key_alloc();
	key_add(header_map, "Close", &header_close);
	key_add(header_map, "Free", &edlib_do_free);
	key_add(header_map, "get-header", &header_get);
	key_add(header_map, "Notify:clip", &header_clip);
}

DEF_LOOKUP_CMD(header_handle, header_map);
DEF_CMD(header_attach)
{
	struct header_info *hi;
	struct pane *p;
	struct mark *start = ci->mark;
	struct mark *end = ci->mark2;

	alloc(hi, pane);
	p = pane_register(ci->focus, 0, &header_handle.c, hi);
	if (!p) {
		free(hi);
		return Efail;
	}

	hi->vnum = home_call(ci->focus, "doc:add-view", p) - 1;
	if (start && end)
		find_headers(p, start, end);

	return comm_call(ci->comm2, "callback:attach", p);
}

void edlib_init(struct pane *ed safe)
{
	header_init_map();
	call_comm("global-set-command", ed, &header_attach, 0, NULL, "attach-rfc822header");
}
