/*
 * Copyright Neil Brown ©2016-2018 <neil@brown.name>
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
 *  doc:step
 *  doc:get-attr doc:set-attr?
 * and might capture doc:revisit to hide??
 * others are doc:load-file,same-file,save-file
 *  doc:replace doc:reundo doc:get-str doc:modified
 */

#define _GNU_SOURCE /* for asprintf */
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include "core.h"
#include "misc.h"

struct email_info {
	struct pane	*email safe;
	struct pane	*spacer safe;
};

static int handle_content(struct pane *p safe, char *type, char *xfer,
			  struct mark *start safe, struct mark *end safe,
			  struct pane *mp safe, struct pane *spacer safe,
			  char *path safe);

DEF_CMD(email_close)
{
	struct email_info *ei = ci->home->data;
	// ??? ;
	call("doc:closed", ei->spacer);
	free(ei);
	return 1;
}

static int cond_append(struct buf *b safe, char *txt safe, char *tag safe,
		       int offset, struct mark *pm, struct mark *m safe)
{
	char *tagf = "active-tag:email-";
	int prelen = 1 + strlen(tagf) + strlen(tag) + 1 + 1;
	int postlen = 1 + 3;
	int len = prelen + strlen(txt) + postlen;
	if (offset != NO_NUMERIC && offset >= 0 && offset <= b->len + len)
		return 0;
	buf_concat(b, "<");
	buf_concat(b, tagf);
	buf_concat(b, tag);
	buf_concat(b, ">[");
	if (pm && pm->rpos == m->rpos) {
		buf_concat_len(b, txt, strlen(txt)/2);
		return 0;
	}
	buf_concat(b, txt);
	buf_concat(b, "]</>");
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
	int o = ci->num;
	char *attr;
	int ret;
	int ok = 1;

	if (!m)
		return Enoarg;

	attr = pane_mark_attr(ci->focus, m, "email:visible");
	if (attr && *attr == '0')
		visible = 0;
	attr = pane_mark_attr(ci->home, m, "multipart-prev:email:actions");
	if (!attr)
		attr = "hide";

	m->rpos = 0;
	if (pm && (pm->rpos == NO_RPOS || pm->rpos == NEVER_RPOS))
		pm->rpos = 0;
	buf_init(&b);
	buf_concat(&b, "<fg:red>");

	while (ok && attr && *attr) {
		if (is_attr("hide", attr))
			ok = cond_append(&b, visible ? "HIDE" : "SHOW", "1", o, pm, m);
		else if (is_attr("save", attr))
			ok = cond_append(&b, "Save", "2", o, pm, m);
		else if (is_attr("open", attr))
			ok = cond_append(&b, "Open", "3", o, pm, m);
		attr = strchr(attr, ':');
		if (attr)
			attr += 1;
	}
	/* end of line */
	if (ok) {
		if ((o < 0 || o == NO_NUMERIC)) {
			buf_concat(&b, "</>\n");
			m->rpos = 0;
			mark_next_pane(ci->focus, m);
		} else
			m->rpos -= 1;
	}

	ret = comm_call(ci->comm2, "callback:render", ci->focus, 0, NULL,
			buf_final(&b));
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
		return Enoarg;
	a = pane_mark_attr(ci->home, m, "renderline:func");
	if (!a || strcmp(a, "doc:email:render-spacer") != 0)
		return Efallthrough;
	a = pane_mark_attr(ci->home, m, "multipart-prev:email:actions");
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
		int vis = 1;
		a = pane_mark_attr(ci->focus, m, "email:visible");
		if (a && *a == '0')
			vis = 0;
		call("doc:set-attr", ci->focus, 1, m, "email:visible", 0, NULL,
		     vis ? "0" : "1");
	}
	return 1;
}

DEF_CMD(email_get_attr)
{
	/* The "renderline:fields" attribute needs to be synthesized
	 * from the per-part email:actions attribute
	 */
	char *a;
	int fields;
	char ret[12];
	if (!ci->str || strcmp(ci->str, "renderline:fields") != 0)
		return Efallthrough;
	if (!ci->mark || !ci->home->parent)
		return Efallthrough;

	a = pane_mark_attr(ci->home->parent, ci->mark, "multipart-prev:email:actions");
	if (!a)
		return 1;
	fields = 0;
	while (a && *a) {
		a = strchr(a, ':');
		if (a)
			a += 1;
		fields += 1;
	}
	snprintf(ret, sizeof(ret), "%d", fields);
	return comm_call(ci->comm2, "callback", ci->focus, 0, ci->mark, ret);
}
static struct map *email_map safe;
static struct map *email_view_map safe;

DEF_LOOKUP_CMD(email_handle, email_map);
DEF_LOOKUP_CMD(email_view_handle, email_view_map);

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
			     struct pane *mp safe, struct pane *spacer safe,
			     char *path)
{
	struct pane *h;
	int need_charset = 0;
	char *charset;
	char *major, *minor = NULL;
	int majlen, minlen;
	char *ctype = NULL;

	h = call_ret(pane, "attach-crop", p, 0, start, NULL, 0, end);
	if (!h)
		return 0;

	if (xfer) {
		int xlen;
		xfer = get_822_token(&xfer, &xlen);
		if (xfer && xlen == 16 &&
		    strncasecmp(xfer, "quoted-printable", 16) == 0) {
			struct pane *hx = call_ret(pane, "attach-quoted_printable", h);
			if (hx) {
				h = hx;
				need_charset = 1;
			}
		}
		if (xfer && xlen == 6 &&
		    strncasecmp(xfer, "base64", 6) == 0) {
			struct pane *hx = call_ret(pane, "attach-base64", h);
			if (hx) {
				h = hx;
				need_charset = 1;
			}
		}
	}
	if (type && need_charset &&
	    (charset = get_822_attr(type, "charset")) != NULL &&
	    strcasecmp(charset, "utf-8") == 0) {
		struct pane *hx = call_ret(pane, "attach-utf8", h);
		if (hx)
			h = hx;
	}
	major = get_822_token(&type, &majlen);
	if (major && tok_matches(major, majlen, "text"))
		attr_set_str(&h->attrs, "email:actions", "hide:save");
	else
		attr_set_str(&h->attrs, "email:actions", "hide:open");
	if (major) {
		minor = get_822_token(&type, &minlen);
		if (minor && tok_matches(minor, minlen, "/"))
			minor = get_822_token(&type, &minlen);
		else
			minor = NULL;
	}
	if (minor)
		asprintf(&ctype, "%1.*s/%1.*s", majlen, major, minlen, minor);
	else
		asprintf(&ctype, "%1.*s", majlen, major);
	if (ctype) {
		int i;
		for (i = 0; ctype[i]; i++)
			if (isupper(ctype[i]))
				ctype[i] = tolower(ctype[i]);
		attr_set_str(&h->attrs, "email:content-type", ctype);
		free(ctype);
	}
	attr_set_str(&h->attrs, "email:path", path);

	home_call(mp, "multipart-add", h);
	home_call(mp, "multipart-add", spacer);
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
			    struct pane *mp safe, struct pane *spacer safe,
			    char *path safe)
{
	char *boundary = get_822_attr(type, "boundary");
	int found_end = 0;
	struct mark *pos, *part_end;
	char *tok;
	int len;
	int partnum = 0;
	char *newpath;

	if (!boundary)
		/* FIXME need a way to say "just display the text" */
		return 1;

	found_end = find_boundary(p, start, end, NULL, boundary);
	if (found_end != 0)
		return 1;
	tok = get_822_token(&type, &len);
	if (tok) {
		tok = get_822_token(&type, &len);
		if (tok && tok[0] == '/')
			tok = get_822_token(&type, &len);
	}
	boundary = strdup(boundary);
	pos = mark_dup(start);
	part_end = mark_dup(pos);
	while (found_end == 0 &&
	       (found_end = find_boundary(p, pos, end, part_end, boundary)) >= 0) {
		struct pane *hdr = call_ret(pane, "attach-rfc822header", p, 0, start, NULL,
		                            0, part_end);
		char *ptype, *pxfer;

		if (!hdr)
			break;
		call("get-header", hdr, 0, NULL, "content-type",
		     0, NULL, "cmd");
		call("get-header", hdr, 0, NULL, "content-transfer-encoding",
		     0, NULL, "cmd");
		ptype = attr_find(hdr->attrs, "rfc822-content-type");
		pxfer = attr_find(hdr->attrs, "rfc822-content-transfer-encoding");

		pane_close(hdr);

		newpath = NULL;
		asprintf(&newpath, "%s%s%1.*s:%d", path, path[0] ? ",":"",
			 len, tok, partnum);
		partnum++;

		handle_content(p, ptype, pxfer, start, part_end, mp, spacer,
			       newpath ?:"");
		free(newpath);
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
			  struct pane *mp safe, struct pane *spacer safe,
			  char *path safe)
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
		return handle_text_plain(p, type, xfer, start, end, mp, spacer, path);

	if (tok_matches(major, mjlen, "multipart"))
		return handle_multipart(p, type, start, end, mp, spacer, path);

	/* default to plain text until we get a better default */
	return handle_text_plain(p, type, xfer, start, end, mp, spacer, path);
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
		return Efallthrough;
	fd = open(ci->str+6, O_RDONLY);
	p = call_ret(pane, "doc:open", ci->focus, fd, NULL, ci->str + 6, 1);
	if (!p)
		return Efallthrough;
	start = vmark_new(p, MARK_UNGROUPED);
	if (!start)
		return Efallthrough;
	end = mark_dup(start);
	call("doc:set-ref", p, 0, end);

	ei = calloc(1, sizeof(*ei));
	ei->email = p;
	h2 = call_ret(pane, "attach-rfc822header", p, 0, start, NULL, 0, end);
	if (!h2)
		goto out;
	p = call_ret(pane, "doc:from-text", p, 0, NULL, NULL, 0, NULL, "\v");
	if (!p) {
		pane_close(h2);
		goto out;
	}
	ei->spacer = p;
	point = vmark_new(p, MARK_POINT);
	call("doc:set-ref", p, 1, point);
	call("doc:set-attr", p, 1, point, "renderline:func", 0,
	     NULL, "doc:email:render-spacer");
	mark_free(point);

	doc = doc_new(ci->focus, "text", ci->focus);
	if (!doc)
		goto out;
	call("doc:set:autoclose", doc, 1);
	point = vmark_new(doc, MARK_POINT);
	if (!point)
		goto out;
	home_call(h2, "get-header", doc, 0, point, "From");
	home_call(h2, "get-header", doc, 0, point, "Date");
	home_call(h2, "get-header", doc, 0, point, "Subject", 0, NULL, "text");
	home_call(h2, "get-header", doc, 0, point, "To", 0, NULL, "list");
	home_call(h2, "get-header", doc, 0, point, "Cc", 0, NULL, "list");

	call("get-header", h2, 0, NULL, "MIME-Version", 0, NULL, "cmd");
	call("get-header", h2, 0, NULL, "content-type", 0, NULL, "cmd");
	call("get-header", h2, 0, NULL, "content-transfer-encoding", 0, NULL, "cmd");
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
	call("doc:set:autoclose", p, 1);
	attr_set_str(&doc->attrs, "email:actions", "hide");
	home_call(p, "multipart-add", doc);
	home_call(p, "multipart-add", ei->spacer);
	call("doc:set:autoclose", doc, 1);

	if (handle_content(ei->email, type, xfer, start, end, p, ei->spacer, "") == 0)
		goto out;

	h = pane_register(p, 0, &email_handle.c, ei, NULL);
	if (h) {
		call("doc:set:filter", h, 1);
		mark_free(start);
		mark_free(end);
		attr_set_str(&h->attrs, "render-default", "text");
		attr_set_str(&p->attrs, "filename", ci->str+6);
		attr_set_str(&p->attrs, "doc-type", "email");
		return comm_call(ci->comm2, "callback:attach", h);
	}
out:
	mark_free(start);
	mark_free(end);
	free(ei);
	// FIXME free stuff
	return Efail;
}

struct email_view {
	int	parts;
	char	*invis safe;
};

DEF_CMD(email_view_close)
{
	struct email_view *evi = ci->home->data;

	free(evi->invis);
	free(evi);
	return 1;
}

static int get_part(struct pane *p safe, struct mark *m safe)
{
	char *a = pane_mark_attr(p, m, "multipart:part-num");

	if (!a)
		return Efail;
	return atoi(a);
}

DEF_CMD(email_step)
{
	struct pane *p = ci->home;
	struct email_view *evi = p->data;
	int ret;
	int n;

	if (!p->parent || !ci->mark)
		return Enoarg;
	if (ci->num) {
		ret = home_call(p->parent, ci->key, ci->focus, ci->num, ci->mark, ci->str,
				ci->num2);
		if (ci->num2 && ret != CHAR_RET(WEOF))
			while ((n = get_part(p->parent, ci->mark)) >= 0 &&
			       n < evi->parts &&
			       evi->invis[n])
				home_call(p->parent, "doc:step-part", ci->focus,
					  ci->num, ci->mark);
	} else {
		/* When moving backwards we need a tmp mark to see
		 * if the result was from an invisible pane.
		 * Note: we could optimize a bit using the knowledge that
		 * every other pane contains only a '\v' and is visible
		 */
		struct mark *m = mark_dup(ci->mark);

		ret = home_call(p->parent, ci->key, ci->focus, ci->num, m, ci->str, 1);
		while (ret != CHAR_RET(WEOF) &&
		       (n = get_part(p->parent, m)) >= 0 &&
		       n < evi->parts &&
		       evi->invis[n]) {
			/* ret is from an invisible pane - sorry */
			if (n == 0) {
				/* No where to go, so go nowhere */
				mark_free(m);
				return CHAR_RET(WEOF);
			}
			home_call(p->parent, "doc:step-part", ci->focus, ci->num, m);
			ret = home_call(p->parent, ci->key, ci->focus, ci->num,
					m, ci->str, 1);
		}
		if (ci->num2)
			mark_to_mark(ci->mark, m);
		mark_free(m);
	}
	return ret;
}

DEF_CMD(email_set_ref)
{
	struct pane *p = ci->home;
	struct email_view *evi = p->data;
	int n;

	if (!p->parent || !ci->mark)
		return Enoarg;
	home_call(p->parent, ci->key, ci->focus, ci->num);
	if (ci->num) {
		/* set to start, need to normalize */
		while ((n = get_part(p->parent, ci->mark)) >= 0 &&
		       n < evi->parts &&
		       evi->invis[n])
			home_call(p->parent, "doc:step-part", ci->focus, 1, ci->mark);
	}
	/* When move to the end, no need to normalize */
	return 1;
}

DEF_CMD(email_view_get_attr)
{
	int p, v;
	struct email_view *evi = ci->home->data;

	if (!ci->str || !ci->mark || !ci->home->parent)
		return Enoarg;
	if (strcmp(ci->str, "email:visible") == 0) {
		p = get_part(ci->home->parent, ci->mark);
		/* only parts can be invisible, not separators */
		p &= ~1;
		v = (p >= 0 && p < evi->parts) ? !evi->invis[p] : 0;

		return comm_call(ci->comm2, "callback", ci->focus, 0, ci->mark,
				 v ? "1":"0");
	}
	return Efallthrough;
}

DEF_CMD(email_view_set_attr)
{
	int p, v;
	struct email_view *evi = ci->home->data;

	if (!ci->str || !ci->mark || !ci->home->parent)
		return Enoarg;
	if (strcmp(ci->str, "email:visible") == 0) {
		p = get_part(ci->home->parent, ci->mark);
		/* only parts can be invisible, not separators */
		p &= ~1;
		v = ci->str2 && atoi(ci->str2) >= 1;
		if (p >= 0 && p < evi->parts)
			evi->invis[p] = !v;
		if (!v) {
			/* Tell viewers that visibility has changed */
			struct mark *m1, *m2;
			m1 = mark_dup(ci->mark);
			home_call(ci->home->parent, "doc:step-part", ci->focus, 0, m1);
			if (get_part(ci->home->parent, m1) != p) {
				mark_prev_pane(ci->home->parent, m1);
				home_call(ci->home->parent, "doc:step-part", ci->focus, 0, m1);
			}
			while ((m2 = doc_prev_mark_all(m1)) != NULL &&
			       mark_same(m1, m2))
				mark_to_mark(m1, m2);
			m2 = mark_dup(m1);
			home_call(ci->home->parent, "doc:step-part", ci->focus, 1, m2);
			call("Notify:change", ci->focus, 0, m1, NULL, 0, m2);
			call("Notify:clip", ci->focus, 0, m1, NULL, 0, m2);
			mark_free(m1);
			mark_free(m2);
		}

		return 1;
	}
	return Efallthrough;
}

DEF_CMD(attach_email_view)
{
	struct pane *p;
	struct email_view *evi;
	struct mark *m;
	int n;

	m = vmark_new(ci->focus, MARK_UNGROUPED);
	if (!m)
		return Esys;
	call("doc:set-ref", ci->focus, 0, m);
	n = get_part(ci->focus, m);
	mark_free(m);
	if (n <= 0 || n > 1000 )
		return Einval;

	evi = calloc(1, sizeof(*evi));
	evi->parts = n;
	evi->invis = calloc(n, sizeof(char));
	p = pane_register(ci->focus, 0, &email_view_handle.c, evi, NULL);
	if (!p) {
		free(evi);
		return Esys;
	}
	return comm_call(ci->comm2, "callback:attach", p);
}

static void email_init_map(void)
{
	email_map = key_alloc();
	key_add(email_map, "Close", &email_close);
	key_add(email_map, "doc:email:render-spacer", &email_spacer);
	key_add(email_map, "doc:email:select", &email_select);
	key_add(email_map, "doc:get-attr", &email_get_attr);

	email_view_map = key_alloc();
	key_add(email_view_map, "Close", &email_view_close);
	key_add(email_view_map, "doc:step", &email_step);
	key_add(email_view_map, "doc:set-ref", &email_set_ref);
	key_add(email_view_map, "doc:set-attr", &email_view_set_attr);
	key_add(email_view_map, "doc:get-attr", &email_view_get_attr);
}

void edlib_init(struct pane *ed safe)
{
	email_init_map();
	call_comm("global-set-command", ed, &open_email, 0, NULL, "open-doc-email");
	call_comm("global-set-command", ed, &attach_email_view, 0, NULL, "attach-email-view");
}
