/*
 * Copyright Neil Brown ©2016 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * lib-ref822header: display a document containing rfc822 headers, in
 * a nicely readable way.
 * This is done by intercepting doc:step, render-line and render-line-prev
 * doc:step keeps the mark in a visible header, but not necessarily on
 * a visible character (yet).  i.e. it could be in the 'charset' of a
 * RFC2047 encoded word.
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
	char header[];
};

struct header_info {
	struct list_head headers;
	int vnum;
};

static void header_add(struct header_info *hi safe, char *header)
{
	struct hdr_list *hl = malloc(sizeof(*hl) + strlen(header) + 1);
	strcpy(hl->header, header);
	list_add(&hl->list, &hi->headers);
}

DEF_CMD(header_step)
{
	struct pane *p = ci->home;
	struct header_info *hi = p->data;
	struct mark *m = ci->mark;
	bool forward = ci->numeric;
	struct mark *st;
	struct mark *ed;

	if (!m)
		return 0;
	st = vmark_at_or_before(p, m, hi->vnum);
	if (st)
		ed = vmark_next(st);
	if (!st || !ed)
		return CHAR_RET(WEOF);
	if (st->seq < m->seq && m->seq < ed->seq &&
	    attr_find_int(st->attrs, "visible") == 1)
		/* let document handle this */
		return 0;
	if (forward) {
		while (ed && (m->seq >= ed->seq ||
			      attr_find_int(st->attrs, "visible") != 1)) {
			st = ed;
			ed = vmark_next(ed);
		}
		if (st)
			mark_to_mark(m, st);
		else
			return CHAR_RET(WEOF);
	} else {
		while (st && (m->seq <= st->seq ||
			      attr_find_int(st->attrs, "visible") != 1)) {
			ed = st;
			st = vmark_prev(st);
		}
		if (ed)
			mark_to_mark(m, ed);
		else
			return CHAR_RET(WEOF);
	}
	return 0;
}

DEF_CMD(header_attr)
{
	struct pane *p = ci->home;
	struct header_info *hi = p->data;
	struct mark *st;
	struct mark *ed;
	struct mark *m = ci->mark;

	if (!ci->mark)
		return 0;

	st = vmark_at_or_before(p, m, hi->vnum);
	if (st)
		ed = vmark_next(st);
	if (!st || !ed)
		return 0;
	if (st->seq < m->seq && m->seq < ed->seq &&
	    attr_find_int(st->attrs, "visible") == 1)
		/* let document handle this */
		;
	else {
		while (ed && (m->seq >= ed->seq ||
			      attr_find_int(st->attrs, "visible") != 1)) {
			st = ed;
			ed = vmark_next(ed);
		}
		if (st)
			mark_to_mark(m, st);
	}
	if (strcmp(ci->str, "render:") == 0 && ci->extra == 1) {
		if (st && mark_same_pane(p, st, ci->mark)) {
			char *h = attr_find(st->attrs, "header");
			char n[5];
			sprintf(n, "%d", (int)(h ? strlen(h)+1 : 0));
			comm_call7(ci->comm2, "callback:get-attr", ci->focus,
				   0, NULL, n, 0, "render:rfc822header", NULL);
		}
	}
	return 0;
}

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

static struct map *header_map safe;

static void header_init_map(void)
{
	header_map = key_alloc();
	key_add(header_map, "doc:step", &header_step);
	key_add(header_map, "doc:get-attr", &header_attr);
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
	struct pane *par = p->parent;

	if (!par)
		return;

	m = vmark_new(p, hi->vnum);
	if (!m)
		return;
	call3("doc:set-ref", p, 1, m);
	hm = mark_dup(m, 0);
	while ((hname = get_hname(par, m)) != NULL) {
		attr_set_str(&hm->attrs, "header", hname);
		free(hname);
		while ((ch = mark_next_pane(par, m)) != WEOF &&
		       (ch != '\n' || isspace(doc_following_pane(par, m))))
			;
		hm = mark_dup(m, 0);
	}
	mark_free(m);
}

static int check_header(struct header_info *hi, char *h)
{
	struct hdr_list *he;
	list_for_each_entry(he, &hi->headers, list) {
		if (strcasecmp(he->header, h) == 0)
			return 1;
	}
	return 0;
}

static void classify_headers(struct pane *p safe)
{
	struct header_info *hi = p->data;
	struct mark *m;
	m = vmark_first(p, hi->vnum);
	while (m) {
		char *h = attr_find(m->attrs, "header");
		if (h && check_header(hi, h))
			attr_set_int(&m->attrs, "visible", 1);
		else
			attr_set_int(&m->attrs, "visible", 0);
		if (h)
			attr_set_int(&m->attrs, "render:rfc822header", strlen(h));
		m = vmark_next(m);
	}
}

DEF_LOOKUP_CMD(header_handle, header_map);
DEF_CMD(header_attach)
{
	struct header_info *hi;
	struct pane *p;

	hi = calloc(1, sizeof(*hi));
	INIT_LIST_HEAD(&hi->headers);
	p = pane_register(ci->focus, 0, &header_handle.c, hi, NULL);
	if (!p) {
		free(hi);
		return -1;
	}
	if (ci->numeric == 0) {
		/* add defaults */
		header_add(hi, "From");
		header_add(hi, "Date");
		header_add(hi, "Subject");
		header_add(hi, "To");
		header_add(hi, "Cc");
	}
	hi->vnum = doc_add_view(p);
	find_headers(p);
	classify_headers(p);

	return comm_call(ci->comm2, "callback:attach", p, 0, NULL, NULL, 0);
}

void edlib_init(struct pane *ed safe)
{
	header_init_map();
	call_comm("global-set-command", ed, 0, NULL, "attach-rfc822header", 0,
		  &header_attach);
}
