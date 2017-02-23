/*
 * Copyright Neil Brown ©2016 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * lib-ref822header: display a document containing rfc822 headers, in
 * a nicely readable way.
 * As we need to re-order lines (so headers are in a standard order)
 * and decode RFC2047 charset encoding, we don't try to translate on the fly,
 * but instead create a secondary document (plain text) and present that.
 *
 * RFC2047 allows headers to contains words:
 *  =?charset?encoding?text?=
 *  "charset" can be "iso-8859-1" "utf-8" "us-ascii" "Windows-1252"
 *    For now I'll assume utf-8 !!
 *  "encoding" can be Q or B (or q or b)
 *     Q recognizes '=' and treat next 2 has HEX, and '_' implies SPACE
 *     B is base64.
 */

#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "core.h"
#include "misc.h"

struct hdr_list {
	struct list_head list;
	int is_list; // do commas separate units
	int is_text; // wrap on spaces
	char header[];
};

struct header_info {
	struct list_head headers;
	int vnum;
	struct pane *orig safe;
};

#define IS_LIST 1
#define IS_TEXT 2

static void header_add(struct header_info *hi safe, char *header, int type)
{
	struct hdr_list *hl = malloc(sizeof(*hl) + strlen(header) + 1);
	strcpy(hl->header, header);
	hl->is_list = type & IS_LIST;
	hl->is_text = type & IS_TEXT;
	list_add_tail(&hl->list, &hi->headers);
}

DEF_CMD(header_close)
{
	struct pane *p = ci->home;
	struct header_info *hi = p->data;
	struct mark *m;

	while ((m = vmark_first(hi->orig, hi->vnum)) != NULL)
		mark_free(m);
	doc_del_view(hi->orig, hi->vnum);
	p->data = safe_cast NULL;
	free(hi);
	return 1;
}

static struct map *header_map safe;

static void header_init_map(void)
{
	header_map = key_alloc();
	key_add(header_map, "Close", &header_close);
}

static char *get_hname(struct pane *p safe, struct mark *m safe)
{
	char hdr[80];
	int len = 0;
	wint_t ch;

	while ((ch = mark_next_pane(p, m)) != ':' &&
	       (ch >= 33 && ch <= 126)) {
		hdr[len++] = ch;
		if (len > 77)
			break;
	}
	hdr[len] = 0;
	if (ch == WEOF)
		return NULL;
	return strdup(hdr);
}

static void find_headers(struct pane *p safe)
{
	struct header_info *hi = p->data;
	struct mark *m, *hm safe;
	wint_t ch;
	char *hname;
	struct pane *doc = hi->orig;

	m = vmark_new(doc, hi->vnum);
	if (!m)
		return;
	call3("doc:set-ref", doc, 1, m);
	hm = mark_dup(m, 0);
	while ((hname = get_hname(doc, m)) != NULL) {
		attr_set_str(&hm->attrs, "header", hname);
		free(hname);
		while ((ch = mark_next_pane(doc, m)) != WEOF &&
		       (ch != '\n' ||
			(ch = doc_following_pane(doc, m)) == ' ' || ch == '\t'))
			;
		hm = mark_dup(m, 0);
	}
	mark_free(m);
}

static void copy_header(struct pane *p safe, struct hdr_list *hdr safe,
			struct mark *start safe, struct mark *end safe)
{
	struct mark *m;
	struct mark *point = vmark_new(p, MARK_POINT);
	struct mark *hstart;
	int sol = 0;
	char buf[5];
	wint_t ch;
	struct header_info *hi = p->data;
	struct pane *doc = hi->orig;
	char attr[100];

	if (!point)
		return;
	m = mark_dup(start, 1);
	call3("doc:set-ref", p, 0, point);
	hstart = mark_dup(point, 1);
	if (hstart->seq > point->seq)
		/* put hstart before point, so it stays here */
		mark_to_mark(hstart, point);
	/* FIXME decode RFC2047 words */
	while ((ch = mark_next_pane(doc, m)) != WEOF &&
	       m->seq < end->seq) {
		if (ch < ' ') {
			sol = 1;
			continue;
		}
		if (sol && (ch == ' ' || ch == '\t'))
			continue;
		if (sol) {
			call7("doc:replace", p, 1, NULL, " ", 1,
			      hdr->is_text ? ",render:rfc822header-wrap=1" : NULL,
			      point);
			sol = 0;
		}
		buf[0] = ch;
		buf[1] = 0;
		call7("doc:replace", p, 1, NULL, buf, 1,
		      ch == ' ' && hdr->is_text ? ",render:rfc822header-wrap=1" : NULL,
		      point);
		if (ch == ',' && hdr->is_list) {
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
	sprintf(buf, "%zd", strlen(hdr->header)+1);
	call7("doc:set-attr", p, 1, hstart, "render:rfc822header", 0, buf, NULL);
	snprintf(attr, sizeof(attr), "render:rfc822header-%s", hdr->header);
	call7("doc:set-attr", p, 1, hstart, attr, 0, "10000", NULL);

	mark_free(hstart);
	mark_free(point);

	mark_free(m);
}

static void add_headers(struct pane *p safe, struct hdr_list *hdr safe)
{
	struct header_info *hi = p->data;
	struct mark *m, *n;

	for (m = vmark_first(hi->orig, hi->vnum); m ; m = n) {
		char *h = attr_find(m->attrs, "header");
		n = vmark_next(m);
		if (n && h && strcasecmp(h, hdr->header) == 0)
			copy_header(p, hdr, m, n);
	}
}

DEF_LOOKUP_CMD(header_handle, header_map);
DEF_CMD(header_attach)
{
	struct header_info *hi;
	struct pane *p;
	struct hdr_list *he;
	struct pane *doc;

	doc = doc_new(ci->focus, "text", ci->focus);
	if (!doc)
		return -1;
	call3("doc:autoclose", doc, 1, NULL);
	hi = calloc(1, sizeof(*hi));
	INIT_LIST_HEAD(&hi->headers);
	hi->orig = ci->focus;
	p = pane_register(doc, 0, &header_handle.c, hi, NULL);
	if (!p) {
		free(hi);
		pane_close(doc);
		return -1;
	}
	if (ci->numeric == 0) {
		/* add defaults */
		header_add(hi, "from", 0);
		header_add(hi, "date", 0);
		header_add(hi, "subject", IS_TEXT);
		header_add(hi, "to", IS_LIST);
		header_add(hi, "cc", IS_LIST);
	}
	hi->vnum = doc_add_view(hi->orig);
	find_headers(p);
	list_for_each_entry(he, &hi->headers, list)
		add_headers(p, he);
	call7("doc:replace", p, 1, NULL, "\n", 1, NULL, NULL);

	return comm_call(ci->comm2, "callback:attach", p, 0, NULL, NULL, 0);
}

void edlib_init(struct pane *ed safe)
{
	header_init_map();
	call_comm("global-set-command", ed, 0, NULL, "attach-rfc822header", 0,
		  &header_attach);
}
