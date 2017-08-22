/*
 * Copyright Neil Brown ©2016 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * doc-email: Present an email message as its intended content, with
 * part recognition and decoding etc.
 *
 * Version 0.1: Use lib-crop to display just the headers, and a separate
 *              instance to display the body.
 *
 * Not so easy.  Need to be careful about redirecting doc commands.
 * A document needs:
 *  doc:set-ref
 *  doc:mark-same
 *  doc:step
 *  doc:get-attr doc:set-attr?
 * and might capture doc:revisit to hide??
 * others are doc:load-file,same-file,save-file
 *  doc:replace doc:reundo doc:get-str doc:modified
 */

#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include "core.h"
#include "misc.h"

struct email_info {
	struct pane	*email safe;
	struct pane	*spacer safe;
};

static int handle_content(struct pane *p safe, char *type, char *xfer,
			  struct mark *start safe, struct mark *end safe,
			  struct pane *mp safe, struct pane *spacer safe);

DEF_CMD(email_close)
{
	struct email_info *ei = ci->home->data;
	// ??? ;
	call3("doc:closed", ei->spacer, 0, NULL);
	free(ei);
	return 1;
}

static int cond_append(struct buf *b safe, char *txt safe, int offset,
			struct mark *pm, struct mark *m safe)
{
	if (offset != NO_NUMERIC && offset >= 0 && offset < b->len + (int)strlen(txt))
		return 0;
	if (pm && pm->rpos == m->rpos) {
		buf_concat_len(b, txt, strlen(txt)/2);
		return 0;
	}
	buf_concat(b, txt);
	m->rpos += 1;
	return 1;
}

static int is_attr(char *a safe, char *attrs safe)
{
	int l = strlen(a);
	if (strncmp(a, attrs, l) != 0)
		return 0;
	if (attrs[l] == ':' || attrs[l] == '\0')
		return 1;
	return 0;
}

DEF_CMD(email_spacer)
{
	struct buf b;
	int visible = 1;
	struct mark *m = ci->mark;
	struct mark *pm = ci->mark2;
	int o = ci->numeric;
	char *attr;
	int ret;
	int ok = 1;

	if (!m)
		return -1;

	attr = pane_mark_attr(ci->home, m, 1, "multipart-prev:multipart:visible");
	if (attr && *attr == '0')
		visible = 0;
	attr = pane_mark_attr(ci->home, m, 1, "multipart-prev:email:actions");
	if (!attr)
		attr = "hide";

	m->rpos = 0;
	if (pm && (pm->rpos == NO_RPOS || pm->rpos == NEVER_RPOS))
		pm->rpos = 0;
	buf_init(&b);
	buf_concat(&b, "<fg:red>");

	while (ok && attr && *attr) {
		if (is_attr("hide", attr))
			ok = cond_append(&b, visible ? "[HIDE]" : "[SHOW]", o, pm, m);
		else if (is_attr("save", attr))
			ok = cond_append(&b, "[Save]",  o, pm, m);
		else if (is_attr("open", attr))
			ok =cond_append(&b, "[Open]",  o, pm, m);
		attr = strchr(attr, ':');
		if (attr)
			attr += 1;
	}
	/* end of line */
	if (ok) {
		if ((o < 0 || o == NO_NUMERIC)) {
			buf_concat(&b, "</>\n");
			m->rpos = 0;
			mark_next_pane(ci->home, m);
		} else
			m->rpos -= 1;
	}

	ret = comm_call(ci->comm2, "callback:render", ci->focus, 0, NULL,
			buf_final(&b), 0);
	free(b.b);
	return ret;
}

DEF_CMD(email_select)
{
	/* If mark is on a button, press it... */
	struct mark *m = ci->mark;
	char *a;
	int r;

	if (!m)
		return -1;
	a = pane_mark_attr(ci->home, m, 1, "renderline:func");
	if (!a || strcmp(a, "doc:email:render-spacer") != 0)
		return 0;
	a = pane_mark_attr(ci->home, m, 1, "multipart-prev:email:actions");
	if (!a)
		a = "hide";
	r = m->rpos;
	while (r > 0 && a) {
		a = strchr(a, ':');
		if (a)
			a += 1;
		r -= 1;
	}
	if (a && is_attr("hide", a)) {
		a = pane_mark_attr(ci->home, m, 1, "multipart-prev:multipart:visible");
		if (a && *a == '0')
			call7("doc:set-attr", ci->home, 1, m, "multipart-prev:multipart:visible", 0, "1", NULL);
		else
			call7("doc:set-attr", ci->home, 1, m, "multipart-prev:multipart:visible", 0, "0", NULL);
	}
	return 1;
}

static struct map *email_map safe;

static void email_init_map(void)
{
	email_map = key_alloc();
	key_add(email_map, "Close", &email_close);
	key_add(email_map, "doc:email:render-spacer", &email_spacer);
	key_add(email_map, "doc:email:select", &email_select);
}
DEF_LOOKUP_CMD(email_handle, email_map);

static char tspecials[] = "()<>@,;:\\\"/[]?=";

static int lws(char c) {
	return c == ' '  || c == '\t' || c == '\r' || c == '\n';
}

static char *get_822_token(char **hdrp safe, int *len safe)
{
	/* A "token" is one of:
	 * Quoted string ""
	 * single char from tspecials
	 * string on of LWS, and none tspecials
	 *
	 * (comments) are skipped.
	 * Start is returned, hdrp is moved, len is reported.
	 */
	char *hdr = *hdrp;
	char *start;
	*len = 0;
	if (!hdr)
		return NULL;
	while (1) {
		while (lws(*hdr))
			hdr++;
		if (*hdr == '(') {
			while (*hdr && *hdr != ')')
				hdr++;
			continue;
		}
		if (*hdr == '"') {
			start = ++hdr;
			while (*hdr && *hdr != '"')
				hdr++;
			*len = hdr - start;
			hdr++;
			*hdrp = hdr;
			return start;
		}
		if (!*hdr) {
			*hdrp = NULL;
			return NULL;
		}
		if (strchr(tspecials, *hdr)) {
			start = hdr;
			hdr++;
			*len = 1;
			*hdrp = hdr;
			return start;
		}
		start = hdr;
		while (*hdr && !lws(*hdr) && !strchr(tspecials, *hdr))
			hdr++;
		*len = hdr - start;
		*hdrp = hdr;
		return start;
	}
}

static char *get_822_attr(char *shdr safe, char *attr safe)
{
	/* If 'hdr' contains "$attr=...", return "..."
	 * with "quotes" stripped
	 */
	int len, alen;
	char *hdr = shdr;
	char *h;
	static char *last = NULL;

	free(last);
	last = NULL;

	alen = strlen(attr);
	while (hdr) {
		while ((h = get_822_token(&hdr, &len)) != NULL &&
		       (len != alen || strncasecmp(h, attr, alen) != 0))
			;
		h = get_822_token(&hdr, &len);
		if (!h || len != 1 || *h != '=')
			continue;
		h = get_822_token(&hdr, &len);
		if (!h)
			continue;
		last = strndup(h, len);
		return last;
	}
	return NULL;
}

static char *get_822_word(char *hdr safe)
{
	/* Get the first word from header, is static space */
	static char *last = NULL;
	int len;
	char *h;

	free(last);
	last = NULL;
	h = get_822_token(&hdr, &len);
	if (!h)
		return h;
	last = strndup(h, len);
	return last;
}

static int tok_matches(char *tok, int len, char *match safe)
{
	if (!tok)
		return 0;
	if (len != (int)strlen(match))
		return 0;
	return strncasecmp(tok, match, len) == 0;
}

static int handle_text_plain(struct pane *p safe, char *type, char *xfer,
			     struct mark *start safe, struct mark *end safe,
			     struct pane *mp safe, struct pane *spacer safe)
{
	struct pane *h;
	int need_charset = 0;
	char *charset;
	char *major;
	int mlen;

	h = call_pane8("attach-crop", p, 0, start, end, 0, NULL, NULL);
	if (!h)
		return 0;

	if (xfer) {
		int xlen;
		xfer = get_822_token(&xfer, &xlen);
		if (xfer && xlen == 16 &&
		    strncasecmp(xfer, "quoted-printable", 16) == 0) {
			struct pane *hx = call_pane("attach-quoted_printable", h, 0, NULL, 0);
			if (hx) {
				h = hx;
				need_charset = 1;
			}
		}
		if (xfer && xlen == 6 &&
		    strncasecmp(xfer, "base64", 6) == 0) {
			struct pane *hx = call_pane("attach-base64", h, 0, NULL, 0);
			if (hx) {
				h = hx;
				need_charset = 1;
			}
		}
	}
	if (type && need_charset &&
	    (charset = get_822_attr(type, "charset")) != NULL &&
	    strcasecmp(charset, "utf-8") == 0) {
		struct pane *hx = call_pane("attach-utf8", h, 0, NULL, 0);
		if (hx)
			h = hx;
	}
	major = get_822_token(&type, &mlen);
	if (major && tok_matches(major, mlen, "text"))
		attr_set_str(&h->attrs, "email:actions", "hide:save");
	else
		attr_set_str(&h->attrs, "email:actions", "hide:open");

	call_home(mp, "multipart-add", h, 0, NULL, NULL);
	call_home(mp, "multipart-add", spacer, 0, NULL, NULL);
	return 1;
}

/* Found a multipart boundary between start and end, moving
 * 'start' to after the boundary, and 'pos' to just before it.
 */
static int find_boundary(struct pane *p safe,
			 struct mark *start safe, struct mark *end safe,
			 struct mark *pos,
			 char *boundary safe)
{
	char *bpos = NULL;
	int dashcnt = 0;

	while (start->seq < end->seq) {
		wint_t ch = mark_next_pane(p, start);

		if (ch == WEOF)
			break;

		if (bpos && *bpos == (char)ch) {
			bpos++;
			if (*bpos)
				continue;
			bpos = NULL;
			dashcnt = 0;
			while ( (ch = mark_next_pane(p, start)) != '\n') {
				if (ch == '\r')
					continue;
				if (ch == '-') {
					dashcnt++;
					continue;
				}
				break;
			}
			if (ch != '\n')
				continue;
			if (dashcnt == 0)
				return 0;
			if (dashcnt == 2)
				return 1;
			dashcnt = -1;
			continue;
		}
		bpos = NULL;
		if (dashcnt >= 0 && ch == '-') {
			dashcnt++;
			if (dashcnt < 2)
				continue;
			dashcnt = -1;
			bpos = boundary;
			continue;
		}
		dashcnt = -1;
		if (ch == '\n') {
			if (pos)
				mark_to_mark(pos, start);
			dashcnt = 0;
		}
	}
	return -1;
}

static int handle_multipart(struct pane *p safe, char *type safe,
			    struct mark *start safe, struct mark *end safe,
			    struct pane *mp safe, struct pane *spacer safe)
{
	char *boundary = get_822_attr(type, "boundary");
	int found_end = 0;
	struct mark *pos, *part_end;

	if (!boundary)
		/* FIXME need a way to say "just display the text" */
		return 1;

	found_end = find_boundary(p, start, end, NULL, boundary);
	if (found_end != 0)
		return 1;
	boundary = strdup(boundary);
	pos = mark_dup(start, 1);
	part_end = mark_dup(pos, 1);
	while (found_end == 0 &&
	       (found_end = find_boundary(p, pos, end, part_end, boundary)) >= 0) {
		struct pane *hdr = call_pane8("attach-rfc822header", p, 0, start,
					      part_end, 0, NULL, NULL);
		char *ptype, *pxfer;

		if (!hdr)
			break;
		call_home7(hdr, "get-header", hdr, 0, NULL, "content-type",
			   0, "cmd", NULL, NULL);
		call_home7(hdr, "get-header", hdr, 0, NULL, "content-transfer-encoding",
			   0, "cmd", NULL, NULL);
		ptype = attr_find(hdr->attrs, "rfc822-content-type");
		pxfer = attr_find(hdr->attrs, "rfc822-content-transfer-encoding");

		pane_close(hdr);
		handle_content(p, ptype, pxfer, start, part_end, mp, spacer);
		mark_to_mark(start, pos);
	}
	mark_to_mark(start, pos);
	mark_free(pos);
	mark_free(part_end);
	free(boundary);
	return 1;
}

static int handle_content(struct pane *p safe, char *type, char *xfer,
			  struct mark *start safe, struct mark *end safe,
			  struct pane *mp safe, struct pane *spacer safe)
{
	char *hdr = type;
	char *major, *minor = NULL;
	int mjlen, mnlen;

	major = get_822_token(&hdr, &mjlen);
	if (major) {
		minor = get_822_token(&hdr, &mnlen);
		if (minor && minor[0] == '/')
			minor = get_822_token(&hdr, &mnlen);
	}
	if (major == NULL ||
	    tok_matches(major, mjlen, "text"))
		return handle_text_plain(p, type, xfer, start, end, mp, spacer);

	if (tok_matches(major, mjlen, "multipart"))
		return handle_multipart(p, type, start, end, mp, spacer);

	/* default to plain text until we get a better default */
	return handle_text_plain(p, type, xfer, start, end, mp, spacer);
}

DEF_CMD(open_email)
{
	int fd;
	struct email_info *ei;
	struct mark *start, *end;
	struct pane *p, *h, *h2;
	char *mime;
	char *xfer = NULL, *type = NULL;
	struct pane *doc;
	struct mark *point;

	if (ci->str == NULL ||
	    strncmp(ci->str, "email:", 6) != 0)
		return 0;
	fd = open(ci->str+6, O_RDONLY);
	p = call_pane7("doc:open", ci->focus, fd, NULL, 1, ci->str + 6, NULL);
	if (!p)
		return 0;
	start = vmark_new(p, MARK_UNGROUPED);
	if (!start)
		return 0;
	end = mark_dup(start, 1);
	call3("doc:set-ref", p, 0, end);

	ei = calloc(1, sizeof(*ei));
	ei->email = p;
	h2 = call_pane8("attach-rfc822header", p, 0, start, end, 0, NULL, NULL);
	if (!h2)
		goto out;
	p = call_pane7("doc:from-text", p, 0, NULL, 0, NULL, "\v");
	if (!p) {
		pane_close(h2);
		goto out;
	}
	ei->spacer = p;
	point = vmark_new(p, MARK_POINT);
	call3("doc:set-ref", p, 1, point);
	call7("doc:set-attr", p, 1, point, "renderline:func", 0,
	      "doc:email:render-spacer", NULL);
	call7("doc:set-attr", p, 1, point, "renderline:fields", 0,
	      "3", NULL);
	mark_free(point);

	doc = doc_new(ci->focus, "text", ci->focus);
	if (!doc)
		goto out;
	call5("doc:set:autoclose", doc, 1, NULL, NULL, 0);
	point = vmark_new(doc, MARK_POINT);
	if (!point)
		goto out;
	call_home7(h2, "get-header", doc, 0, point, "From", 0, NULL, NULL, NULL);
	call_home7(h2, "get-header", doc, 0, point, "Date", 0, NULL, NULL, NULL);
	call_home7(h2, "get-header", doc, 0, point, "Subject", 0, "text", NULL, NULL);
	call_home7(h2, "get-header", doc, 0, point, "To", 0, "list", NULL, NULL);
	call_home7(h2, "get-header", doc, 0, point, "Cc", 0, "list", NULL, NULL);

	call_home7(h2, "get-header", h2, 0, NULL, "MIME-Version", 0, "cmd", NULL, NULL);
	call_home7(h2, "get-header", h2, 0, NULL, "content-type", 0, "cmd", NULL, NULL);
	call_home7(h2, "get-header", h2, 0, NULL, "content-transfer-encoding", 0, "cmd", NULL, NULL);
	mime = attr_find(h2->attrs, "rfc822-mime-version");
	if (mime)
		mime = get_822_word(mime);
	if (mime && strcmp(mime, "1.0") == 0) {
		type = attr_find(h2->attrs, "rfc822-content-type");
		xfer = attr_find(h2->attrs, "rfc822-content-transfer-encoding");
	}
	pane_close(h2);

	p = doc_new(ci->home, "multipart", ei->email);
	if (!p)
		goto out;
	attr_set_str(&doc->attrs, "email:actions", "hide");
	call_home(p, "multipart-add", doc, 0, NULL, NULL);
	call_home(p, "multipart-add", ei->spacer, 0, NULL, NULL);
	call5("doc:set:autoclose", doc, 1, NULL, NULL, 0);

	if (handle_content(ei->email, type, xfer, start, end, p, ei->spacer) == 0)
		goto out;

	h = pane_register(p, 0, &email_handle.c, ei, NULL);
	if (h) {
		call5("doc:set:filter", h, 1, NULL, NULL, 0);
		mark_free(start);
		mark_free(end);
		attr_set_str(&h->attrs, "render-default", "text");
		attr_set_str(&p->attrs, "filename", ci->str+6);
		attr_set_str(&p->attrs, "doc-type", "email");
		return comm_call(ci->comm2, "callback:attach", h, 0, NULL, NULL, 0);
	}
out:
	mark_free(start);
	mark_free(end);
	free(ei);
	// FIXME free stuff
	return -1;
}


void edlib_init(struct pane *ed safe)
{
	email_init_map();
	call_comm("global-set-command", ed, 0, NULL, "open-doc-email", 0, &open_email);
}
