/*
 * Copyright Neil Brown ©2016-2019 <neil@brown.name>
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
 *  "charset" can be "iso-8859-1" "utf-8" "us-ascii" "Windows-1252"
 *    For now I'll assume utf-8 !!
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
	p->data = safe_cast NULL;
	free(hi);
	return 1;
}

static char *get_hname(struct pane *p safe, struct mark *m safe)
{
	char hdr[80];
	int len = 0;
	wint_t ch;

	while ((ch = mark_next_pane(p, m)) != ':' &&
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
		while ((ch = mark_next_pane(p, m)) != WEOF &&
		       m->seq < end->seq &&
		       (ch != '\n' ||
			(ch = doc_following_pane(p, m)) == ' ' || ch == '\t'))
			;
		hm = mark_dup_view(m);
	}
	/* Skip over trailing blank line */
	if (doc_following_pane(p, m) == '\r')
		mark_next_pane(p, m);
	if (doc_following_pane(p, m) == '\n')
		mark_next_pane(p, m);
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
	/* RFC2047  decoding.
	 * Search for second '?' (assume utf-8), detect 'Q' or 'B',
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
	wint_t ch;
	struct mark *m2;

	free(last);
	last = NULL;

	buf_init(&buf);
	while ((ch = mark_next_pane(doc, m)) != WEOF &&
	       ch > ' ' && ch < 0x7f && qmarks < 4) {
		if (ch == '?') {
			qmarks++;
			continue;
		}
		if (qmarks == 2 && (ch == 'q' ||ch == 'Q'))
			code = 'q';
		if (qmarks == 2 && (ch == 'b' || ch == 'B'))
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
	/* If there is only LWS to the next quoted word,
	 * skip that so words join up
	 */
	m2 = mark_dup(m);
	if (!m2)
		return last;
	while ((ch = mark_next_pane(doc, m2)) == ' ' ||
	       ch == '\t' || ch == '\r' || ch == '\n')
		;
	if (ch == '=' && doc_following_pane(doc, m2) == '?') {
		mark_prev_pane(doc, m2);
		mark_to_mark(m, m2);
	}
	mark_free(m2);
	return last;
}

static void copy_header(struct pane *doc safe, char *hdr safe, char *type,
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
	mark_make_first(hstart);
	/* FIXME decode RFC2047 words */
	while ((ch = mark_next_pane(doc, m)) != WEOF &&
	       m->seq < end->seq) {
		char *b;

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
		if (ch == '=' && doc_following_pane(doc, m) == '?')
			b = charset_word(doc, m);
		else
			b = buf;
		call("doc:replace", p, 1, NULL, b, 0, point,
		     ch == ' ' && is_text ? ",render:rfc822header-wrap=1" : NULL);
		if (ch == ',' && is_list) {
			struct mark *p2 = mark_dup(point);
			int cnt = 1;
			mark_prev_pane(p, p2);
			while ((ch = doc_following_pane(doc, m)) == ' ') {
				call("doc:replace", p, 1, NULL, " ", 0, point);
				mark_next_pane(doc, m);
				cnt += 1;
			}
			if (ch == '\n' || ch == '\r')
				cnt += 1;
			snprintf(buf, sizeof(buf), "%d", cnt);
			call("doc:set-attr", p, 1, p2, "render:rfc822header-wrap", 0, NULL, buf);
			mark_free(p2);
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

static void copy_headers(struct pane *p safe, char *hdr safe, char *type,
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
	int found = 0;
	struct buf buf;
	wint_t ch;

	buf_init(&buf);
	m = mark_dup(start);
	while ((ch = mark_next_pane(p, m)) != WEOF &&
	       m->seq < end->seq) {
		if (!found && ch == ':') {
			found = 1;
			continue;
		}
		if (!found)
			continue;
		if (ch == '=' && doc_following_pane(p, m) == '?') {
			char *b = charset_word(p, m);
			buf_concat(&buf, b);
		} else
			buf_append(&buf, ch);
	}
	return buf_final(&buf);
}

static char *load_header(struct pane *home safe, char *hdr safe)
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
	char *hdr = ci->str;
	char *type = ci->str2;
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

	marks_clip(ci->home, ci->mark, ci->mark2, hi->vnum, ci->home);
	return 0;
}

static struct map *header_map safe;

static void header_init_map(void)
{
	header_map = key_alloc();
	key_add(header_map, "Close", &header_close);
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

	hi = calloc(1, sizeof(*hi));
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
