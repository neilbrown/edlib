/*
 * Copyright Neil Brown ©2016 <neil@brown.name>
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

	while ((m = vmark_first(p, hi->vnum)) != NULL)
		mark_free(m);
	doc_del_view(p, hi->vnum);
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

	m = vmark_new(p, hi->vnum);
	if (!m)
		return;
	mark_to_mark(m, start);
	hm = mark_dup(m, 0);
	while (m->seq < end->seq &&
	       (hname = get_hname(p, m)) != NULL) {
		attr_set_str(&hm->attrs, "header", hname);
		free(hname);
		while ((ch = mark_next_pane(p, m)) != WEOF &&
		       m->seq < end->seq &&
		       (ch != '\n' ||
			(ch = doc_following_pane(p, m)) == ' ' || ch == '\t'))
			;
		hm = mark_dup(m, 0);
	}
	/* Skip over trailing blank line */
	if (doc_following_pane(p, m) == '\r')
		mark_next_pane(p, m);
	if (doc_following_pane(p, m) == '\n')
		mark_next_pane(p, m);
	mark_to_mark(start, m);
	mark_free(m);
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
	char buf[5];
	wint_t ch;
	char attr[100];
	int is_text = type && strcmp(type, "text") == 0;
	int is_list = type && strcmp(type, "list") == 0;

	m = mark_dup(start, 1);
	hstart = mark_dup(point, 1);
	if (hstart->seq > point->seq)
		/* put hstart before point, so it stays here */
		mark_to_mark(hstart, point);
	/* FIXME decode RFC2047 words */
	while ((ch = mark_next_pane(doc, m)) != WEOF &&
	       m->seq < end->seq) {
		if (ch < ' ' && ch != '\t') {
			sol = 1;
			continue;
		}
		if (sol && (ch == ' ' || ch == '\t'))
			continue;
		if (sol) {
			call7("doc:replace", p, 1, NULL, " ", 1,
			      is_text ? ",render:rfc822header-wrap=1" : NULL,
			      point);
			sol = 0;
		}
		buf[0] = ch;
		buf[1] = 0;
		call7("doc:replace", p, 1, NULL, buf, 1,
		      ch == ' ' && is_text ? ",render:rfc822header-wrap=1" : NULL,
		      point);
		if (ch == ',' && is_list) {
			struct mark *p2 = mark_dup(point, 1);
			int cnt = 1;
			mark_prev_pane(p, p2);
			while ((ch = doc_following_pane(doc, m)) == ' ') {
				call7("doc:replace", p, 1, NULL, " ", 1, NULL, point);
				mark_next_pane(doc, m);
				cnt += 1;
			}
			if (ch == '\n' || ch == '\r')
				cnt += 1;
			sprintf(buf, "%d", cnt);
			call7("doc:set-attr", p, 1, p2, "render:rfc822header-wrap", 0, buf, NULL);
			mark_free(p2);
		}
	}
	call7("doc:replace", p, 1, NULL, "\n", 1, NULL, point);
	sprintf(buf, "%zd", strlen(hdr)+1);
	call7("doc:set-attr", p, 1, hstart, "render:rfc822header", 0, buf, NULL);
	snprintf(attr, sizeof(attr), "render:rfc822header-%s", hdr);
	call7("doc:set-attr", p, 1, hstart, attr, 0, "10000", NULL);

	mark_free(hstart);
	mark_free(m);
}

static void copy_headers(struct pane *p safe, char *hdr safe, char *type,
			 struct pane *doc safe, struct mark *pt safe)
{
	struct header_info *hi = p->data;
	struct mark *m, *n;

	for (m = vmark_first(p, hi->vnum); m ; m = n) {
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
	m = mark_dup(start, 1);
	while ((ch = mark_next_pane(p, m)) != WEOF &&
	       m->seq < end->seq) {
		if (!found && ch == ':') {
			found = 1;
			continue;
		}
		if (!found)
			continue;
		buf_append(&buf, ch);
	}
	return buf_final(&buf);
}

static char *load_header(struct pane *home safe, char *hdr safe)
{
	struct header_info *hi = home->data;
	struct mark *m, *n;

	for (m = vmark_first(home, hi->vnum); m; m = n) {
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
		return -1;

	if (ci->mark) {
		copy_headers(ci->home, hdr, type, ci->focus, ci->mark);
		return 1;
	}
	asprintf(&attr, "rfc822-%s", hdr);
	if (!attr)
		return -1;
	for (c = attr; *c; c++)
		if (isupper(*c))
			*c = tolower(*c);
	t = load_header(ci->home, hdr);
	attr_set_str(&ci->home->attrs, attr, t);
	free(attr);
	free(t);
	return t ? 1 : 2;
}

static struct map *header_map safe;

static void header_init_map(void)
{
	header_map = key_alloc();
	key_add(header_map, "Close", &header_close);
	key_add(header_map, "get-header", &header_get);
}

DEF_LOOKUP_CMD(header_handle, header_map);
DEF_CMD(header_attach)
{
	struct header_info *hi;
	struct pane *p;
	struct mark *start = ci->mark;
	struct mark *end = ci->mark2;

	hi = calloc(1, sizeof(*hi));
	p = pane_register(ci->focus, 0, &header_handle.c, hi, NULL);
	if (!p) {
		free(hi);
		return -1;
	}

	hi->vnum = doc_add_view(p);
	if (start && end)
		find_headers(p, start, end);

	return comm_call(ci->comm2, "callback:attach", p, 0, NULL, NULL, 0);
}

void edlib_init(struct pane *ed safe)
{
	header_init_map();
	call_comm("global-set-command", ed, 0, NULL, "attach-rfc822header", 0,
		  &header_attach);
}
