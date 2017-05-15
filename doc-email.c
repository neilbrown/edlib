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

struct email_info {
	struct pane	*email safe;
};


DEF_CMD(email_close)
{
	struct email_info *ei = ci->home->data;
	// ??? ;
	free(ei);
	return 1;
}


static struct map *email_map safe;

static void email_init_map(void)
{
	email_map = key_alloc();
	key_add(email_map, "Close", &email_close);
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

DEF_CMD(open_email)
{
	int fd;
	struct email_info *ei;
	struct mark *start, *end;
	struct pane *p, *h, *h2;
	char *mime;
	char *xfer = NULL, *type = NULL, *charset = NULL;
	int need_charset = 0;
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

	doc = doc_new(ci->focus, "text", ci->focus);
	if (!doc)
		goto out;
	call3("doc:autoclose", doc, 1, NULL);
	point = vmark_new(doc, MARK_POINT);
	if (!point)
		goto out;
	call_home7(h2, "get-header", doc, 0, point, "From", 0, NULL, NULL, NULL);
	call_home7(h2, "get-header", doc, 0, point, "Date", 0, NULL, NULL, NULL);
	call_home7(h2, "get-header", doc, 0, point, "Subject", 0, "text", NULL, NULL);
	call_home7(h2, "get-header", doc, 0, point, "To", 0, "list", NULL, NULL);
	call_home7(h2, "get-header", doc, 0, point, "Cc", 0, "list", NULL, NULL);

	call7("doc:replace", doc, 1, point, "\n", 1, NULL, NULL);

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

	/* Assume text/plain for now */
	h = call_pane8("attach-crop", p, 0, start, end, 0, NULL, NULL);
	if (!h)
		goto out;

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

	p = doc_new(ci->home, "multipart", ei->email);
	if (!p)
		goto out;
	call_home(p, "multipart-add", doc, 0, NULL, NULL);
	call_home(p, "multipart-add", h, 0, NULL, NULL);
	call3("doc:autoclose", p, 1, NULL);

	h = pane_register(p, 0, &email_handle.c, ei, NULL);
	if (h) {
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
