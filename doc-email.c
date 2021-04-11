/*
 * Copyright Neil Brown ©2016-2020 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * doc-email: Present an email message as its intended content, with
 * part recognition and decoding etc.
 *
 * A multipart document is created where every other part is a "spacer"
 * where buttons can be placed to control visibility of the previous part,
 * or to act on it in some other way.
 * The first part is the headers which are copied to a temp text document.
 * Subsequent non-spacer parts are cropped sections of the email, possibly
 * with filters overlayed to handle the transfer encoding.
 * Alternately, they might be temp documents similar to the headers
 * storing e.g. transformed HTML or an image.
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

static inline bool is_orig(int p)
{
	return p >= 0 && p % 2 == 0;
}

static inline bool is_spacer(int p)
{
	return p >= 0 && p % 2 == 1;
}

static inline int to_orig(int p)
{
	if (p < 0)
		return p;
	return p - p % 2;
}

struct email_info {
	struct pane	*email safe;
	struct pane	*spacer safe;
};

static bool handle_content(struct pane *p safe, char *type, char *xfer,
			   struct mark *start safe, struct mark *end safe,
			   struct pane *mp safe, struct pane *spacer safe,
			   char *path safe);

static bool cond_append(struct buf *b safe, char *txt safe, char *tag safe,
			int offset, struct mark *m safe, int *cp safe)
{
	char *tagf = "active-tag:email-";
	int prelen = 1 + strlen(tagf) + strlen(tag) + 1 + 1;
	int postlen = 1 + 3;
	int len = prelen + strlen(txt) + postlen;
	if (offset != NO_NUMERIC && offset >= 0 && offset <= b->len + len)
		return False;
	buf_concat(b, "<");
	buf_concat(b, tagf);
	buf_concat(b, tag);
	buf_concat(b, ">[");
	if (*cp == 0)
		return False;
	if (*cp > 0)
		*cp -= 1;
	buf_concat(b, txt);
	buf_concat(b, "]</>");
	return True;
}

static bool is_attr(char *a safe, char *attrs safe)
{
	int l = strlen(a);
	if (strncmp(a, attrs, l) != 0)
		return False;
	if (attrs[l] == ':' || attrs[l] == '\0')
		return True;
	return False;
}

DEF_CMD(email_spacer)
{
	struct buf b;
	int visible = 1;
	struct mark *m = ci->mark;
	struct mark *pm = ci->mark2;
	int o = ci->num;
	int cp = -1;
	char *attr;
	int ret;
	bool ok = True;

	if (!m)
		return Enoarg;
	if (pm) {
		/* Count the number of chars before the cursor.
		 * This tells us which button to highlight.
		 */
		cp = 0;
		pm = mark_dup(pm);
		while (pm->seq > m->seq && !mark_same(pm, m)) {
			doc_prev(ci->focus, pm);
			cp += 1;
		}
		mark_free(pm);
	}

	buf_init(&b);
	buf_concat(&b, "<fg:red>");

	attr = pane_mark_attr(ci->home, m, "multipart-prev:email:path");
	if (attr) {
		buf_concat(&b, attr);
		buf_concat(&b, " ");
	}

	attr = pane_mark_attr(ci->focus, m, "email:visible");
	if (attr && strcmp(attr, "none") == 0)
		visible = 0;
	attr = pane_mark_attr(ci->home, m, "multipart-prev:email:actions");
	if (!attr)
		attr = "hide";

	while (ok && attr && *attr) {
		if (is_attr("hide", attr))
			ok = cond_append(&b, visible ? "HIDE" : "SHOW", "1",
					 o, m, &cp);
		else if (is_attr("save", attr))
			ok = cond_append(&b, "Save", "2", o, m, &cp);
		else if (is_attr("open", attr))
			ok = cond_append(&b, "Open", "3", o, m, &cp);
		attr = strchr(attr, ':');
		if (attr)
			attr += 1;
	}
	/* end of line, only display if we haven't reached
	 * the cursor or offset
	 *
	 * if cp < 0, we aren't looking for a cursor, so don't stop.
	 * if cp > 0, we haven't reached cursor yet, so don't stop
	 * if cp == 0, this is cursor pos, so stop.
	 */
	if (ok && cp != 0 && ((o < 0 || o == NO_NUMERIC))) {
		wint_t wch;
		buf_concat(&b, "</>");
		attr = pane_mark_attr(ci->focus, m,
				      "multipart-prev:email:content-type");
		if (attr) {
			buf_concat(&b, " ");
			buf_concat(&b, attr);
		}
		buf_concat(&b, "\n");
		while ((wch = doc_next(ci->focus, m)) &&
		       wch != '\n' && wch != WEOF)
			;
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
	int r = 0;

	if (!m)
		return Enoarg;
	a = pane_mark_attr(ci->focus, m, "markup:func");
	if (!a || strcmp(a, "doc:email:render-spacer") != 0)
		return Efallthrough;
	a = pane_mark_attr(ci->focus, m, "multipart-prev:email:actions");
	if (!a)
		a = "hide";
	while (r > 0 && a) {
		a = strchr(a, ':');
		if (a)
			a += 1;
		r -= 1;
	}
	if (a && is_attr("hide", a)) {
		int vis = 1;
		a = pane_mark_attr(ci->focus, m, "email:visible");
		if (a && strcmp(a, "none") == 0)
			vis = 0;
		call("doc:set-attr", ci->focus, 1, m, "email:visible", 0, NULL,
		     vis ? "none" : "orig");
	}
	return 1;
}

static struct map *email_view_map safe;

DEF_LOOKUP_CMD(email_view_handle, email_view_map);

static char tspecials[] = "()<>@,;:\\\"/[]?=";

static int lws(char c) {
	return c == ' '  || c == '\t' || c == '\r' || c == '\n';
}

static char *get_822_token(char **hdrp safe, int *len safe)
{
	/* A "token" is one of:
	 * - Quoted string ""
	 * - single char from tspecials (except '(' or '"')
	 * - string of non-LWS, and non-tspecials
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
	 * with "quotes" stripped.  Return value can be used
	 * until the next call, when it will be free.
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
	/* Get the first word from header, in static
	 * space (freed on next call)
	 */
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

static bool tok_matches(char *tok, int len, char *match safe)
{
	if (!tok)
		return False;
	if (len != (int)strlen(match))
		return False;
	return strncasecmp(tok, match, len) == 0;
}

static bool handle_text(struct pane *p safe, char *type, char *xfer,
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
		return False;

	if (xfer) {
		int xlen;
		xfer = get_822_token(&xfer, &xlen);
		if (xfer && xlen == 16 &&
		    strncasecmp(xfer, "quoted-printable", 16) == 0) {
			struct pane *hx = call_ret(pane,
						   "attach-quoted_printable",
						   h);
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
	if (ctype && strcmp(ctype, "text/html") == 0) {
		struct pane *html;
		html = call_ret(pane, "html-to-text", h);
		if (html) {
			pane_close(h);
			h = html;
		}
	}
	if (ctype && strcmp(ctype, "application/pdf") == 0) {
		struct pane *pdf;
		pdf = call_ret(pane, "pdf-to-text", h);
		if (pdf) {
			pane_close(h);
			h = pdf;
		}
	}
	if (ctype) {
		int i;
		for (i = 0; ctype[i]; i++)
			if (isupper(ctype[i]))
				ctype[i] = tolower(ctype[i]);
		attr_set_str(&h->attrs, "email:content-type", ctype);
		free(ctype);
	} else
		attr_set_str(&h->attrs, "email:content-type", "text/plain");
	attr_set_str(&h->attrs, "email:path", path);
	attr_set_str(&h->attrs, "email:which", "orig");

	home_call(mp, "multipart-add", h);
	home_call(mp, "multipart-add", spacer);
	return True;
}

/* Find a multipart boundary between start and end, moving
 * 'start' to after the boundary, and 'pos' to just before it.
 * Return 0 if a non-terminal boundary is found
 * Return 1 if a terminal boundary is found (trailing --)
 * Return -1 if nothing is found.
 */
#define is_lws(c) ({int __c2 = c; __c2 == ' ' || __c2 == '\t' || is_eol(__c2); })
static int find_boundary(struct pane *p safe,
			 struct mark *start safe, struct mark *end safe,
			 struct mark *pos,
			 char *boundary safe)
{
	char *patn = NULL;
	int ret;
	int len = strlen(boundary);

	asprintf(&patn, "^--(?%d:%s)(--)?[ \\t\\r]*$", len, boundary);
	ret = call("text-search", p, 0, start, patn, 0, end);
	if (ret <= 0)
		return -1;
	ret -= 1;
	if (pos) {
		int cnt = ret;
		mark_to_mark(pos, start);
		while (cnt > 0 && doc_prev(p, pos) != WEOF)
			cnt -= 1;
		/* Previous char is CRLF, and must be swallowed */
		if (doc_prior(p, pos) == '\n')
			doc_prev(p, pos);
		if (doc_prior(p, pos) == '\r')
			doc_prev(p, pos);
	}
	while (is_lws(doc_prior(p, start))) {
		len -= 1;
		doc_prev(p, start);
	}
	while (is_lws(doc_following(p, start)))
		doc_next(p, start);
	if (ret == 2 + len)
		return 0;
	if (ret == 2 + len + 2)
		return 1;
	return -1;
}

static bool handle_multipart(struct pane *p safe, char *type safe,
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
		return True;

	found_end = find_boundary (p, start, end, NULL, boundary);
	if (found_end != 0)
		return True;
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
	       (found_end = find_boundary(p, pos, end, part_end,
					  boundary)) >= 0) {
		struct pane *hdr = call_ret(pane, "attach-rfc822header", p,
					    0, start, NULL,
					    0, part_end);
		char *ptype, *pxfer;

		if (!hdr)
			break;
		call("get-header", hdr, 0, NULL, "content-type",
		     0, NULL, "cmd");
		call("get-header", hdr, 0, NULL, "content-transfer-encoding",
		     0, NULL, "cmd");
		ptype = attr_find(hdr->attrs, "rfc822-content-type");
		pxfer = attr_find(hdr->attrs,
				  "rfc822-content-transfer-encoding");

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
	return True;
}

static bool handle_content(struct pane *p safe, char *type, char *xfer,
			   struct mark *start safe, struct mark *end safe,
			   struct pane *mp safe, struct pane *spacer safe,
			   char *path safe)
{
	char *hdr;
	char *major, *minor = NULL;
	int mjlen, mnlen;

	if (!type)
		type = "text/plain";
	hdr = type;

	major = get_822_token(&hdr, &mjlen);
	if (major) {
		minor = get_822_token(&hdr, &mnlen);
		if (minor && minor[0] == '/')
			minor = get_822_token(&hdr, &mnlen);
	}
	if (major == NULL ||
	    tok_matches(major, mjlen, "text"))
		return handle_text(p, type, xfer, start, end,
				   mp, spacer, path);

	if (tok_matches(major, mjlen, "multipart"))
		return handle_multipart(p, type, start, end, mp, spacer, path);

	/* default to plain text until we get a better default */
	return handle_text(p, type, xfer, start, end, mp, spacer, path);
}

DEF_CMD(open_email)
{
	int fd;
	struct email_info *ei;
	struct mark *start, *end;
	struct pane *p, *h2;
	char *mime;
	char *xfer = NULL, *type = NULL;
	struct pane *hdrdoc;
	struct mark *point;

	if (ci->str == NULL ||
	    strncmp(ci->str, "email:", 6) != 0)
		return Efallthrough;
	fd = open(ci->str+6, O_RDONLY);
	p = call_ret(pane, "doc:open", ci->focus, fd, NULL, ci->str + 6, 1);
	if (!p)
		return Efallthrough;
	start = vmark_new(p, MARK_UNGROUPED, NULL);
	if (!start)
		return Efallthrough;
	end = mark_dup(start);
	call("doc:set-ref", p, 0, end);

	alloc(ei, pane);
	ei->email = p;
	h2 = call_ret(pane, "attach-rfc822header", p, 0, start, NULL, 0, end);
	if (!h2)
		goto out;
	p = call_ret(pane, "doc:from-text", p, 0, NULL, NULL, 0, NULL,
		     "0123456789\n");
	if (!p) {
		pane_close(h2);
		goto out;
	}
	attr_set_str(&p->attrs, "email:which", "spacer");
	ei->spacer = p;
	point = vmark_new(p, MARK_POINT, NULL);
	call("doc:set-ref", p, 1, point);
	call("doc:set-attr", p, 1, point, "markup:func", 0,
	     NULL, "doc:email:render-spacer");
	mark_free(point);

	hdrdoc = call_ret(pane, "attach-doc-text", ci->focus);
	if (!hdrdoc)
		goto out;
	call("doc:set:autoclose", hdrdoc, 1);
	point = vmark_new(hdrdoc, MARK_POINT, NULL);
	if (!point)
		goto out;

	/* copy some headers to the header temp document */
	home_call(h2, "get-header", hdrdoc, 0, point, "From");
	home_call(h2, "get-header", hdrdoc, 0, point, "Date");
	home_call(h2, "get-header", hdrdoc, 0, point, "Subject", 0, NULL, "text");
	home_call(h2, "get-header", hdrdoc, 0, point, "To", 0, NULL, "list");
	home_call(h2, "get-header", hdrdoc, 0, point, "Cc", 0, NULL, "list");

	/* copy some headers into attributes for later analysis */
	call("get-header", h2, 0, NULL, "MIME-Version", 0, NULL, "cmd");
	call("get-header", h2, 0, NULL, "content-type", 0, NULL, "cmd");
	call("get-header", h2, 0, NULL, "content-transfer-encoding",
	     0, NULL, "cmd");
	mime = attr_find(h2->attrs, "rfc822-mime-version");
	if (mime)
		mime = get_822_word(mime);
	if (mime && strcmp(mime, "1.0") == 0) {
		type = attr_find(h2->attrs, "rfc822-content-type");
		xfer = attr_find(h2->attrs, "rfc822-content-transfer-encoding");
	}
	pane_close(h2);

	p = call_ret(pane, "attach-doc-multipart", ci->home);
	if (!p)
		goto out;
	call("doc:set:autoclose", p, 1);
	attr_set_str(&hdrdoc->attrs, "email:actions", "hide");
	attr_set_str(&hdrdoc->attrs, "email:which", "orig");
	attr_set_str(&hdrdoc->attrs, "email:content-type", "text/rfc822-headers");
	home_call(p, "multipart-add", hdrdoc);
	home_call(p, "multipart-add", ei->spacer);
	call("doc:set:autoclose", hdrdoc, 1);

	attr_set_str(&hdrdoc->attrs, "email:path", "headers");

	if (!handle_content(ei->email, type, xfer, start, end,
			    p, ei->spacer, "body"))
		goto out;

	mark_free(start);
	mark_free(end);
	attr_set_str(&p->attrs, "render-default", "text");
	attr_set_str(&p->attrs, "filename", ci->str+6);
	attr_set_str(&p->attrs, "doc-type", "email");
	return comm_call(ci->comm2, "callback:attach", p);

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

DEF_CMD(email_view_free)
{
	struct email_view *evi = ci->home->data;

	free(evi->invis);
	unalloc(evi, pane);
	return 1;
}

static int get_part(struct pane *p safe, struct mark *m safe)
{
	char *a = pane_mark_attr(p, m, "multipart:part-num");

	if (!a)
		return Efail;
	return atoi(a);
}

static int count_buttons(struct pane *p safe, struct mark *m safe)
{
	int cnt = 0;
	char *attr = pane_mark_attr(p, m, "multipart-prev:email:actions");
	if (!attr)
		attr = "hide";
	while (attr) {
		cnt += 1;
		attr = strchr(attr, ':');
		if (attr)
			attr++;
	}
	return cnt;
}

DEF_CMD(email_step)
{
	struct pane *p = ci->home;
	struct email_view *evi = p->data;
	wint_t ret;
	int n = -1;

	if (!ci->mark)
		return Enoarg;
	if (ci->num) {
		ret = home_call(p->parent, ci->key, ci->focus,
				ci->num, ci->mark, evi->invis,
				ci->num2);
		n = get_part(p->parent, ci->mark);
		if (ci->num2 && is_spacer(n)) {
			/* Moving in a spacer, If after valid buttons,
			 * move to end
			 */
			wint_t c;
			unsigned int buttons;
			buttons = count_buttons(p, ci->mark);
			while (isdigit(c = doc_following(p->parent, ci->mark)) &&
			       (c - '0') >= buttons)
					doc_next(p->parent, ci->mark);
		}
	} else {
		ret = home_call(p->parent, ci->key, ci->focus,
				ci->num, ci->mark, evi->invis, 1);
		n = get_part(p->parent, ci->mark);
		if (is_spacer(n) && ci->num2 && isdigit(ret & 0xfffff)) {
			/* Just stepped back over the 9 at the end of a spacer,
			 * Maybe step further if there aren't 10 buttons.
			 */
			unsigned int buttons = count_buttons(p, ci->mark);
			wint_t c = ret & 0xfffff;

			while (isdigit(c) && c - '0' >= buttons)
				c = doc_prev(p->parent, ci->mark);
			ret = CHAR_RET(c);
		}
	}
	return ret;
}

DEF_CMD(email_set_ref)
{
	struct pane *p = ci->home;
	struct email_view *evi = p->data;

	if (!ci->mark)
		return Enoarg;
	home_call(p->parent, ci->key, ci->focus, ci->num, ci->mark, evi->invis);
	return 1;
}

DEF_CMD(email_view_get_attr)
{
	int p, v;
	struct email_view *evi = ci->home->data;

	if (!ci->str || !ci->mark)
		return Enoarg;
	if (strcmp(ci->str, "email:visible") == 0) {
		p = get_part(ci->home->parent, ci->mark);
		/* only parts can be invisible, not separators */
		p = to_orig(p);
		v = (p >= 0 && p < evi->parts) ? evi->invis[p] != 'i' : 0;

		return comm_call(ci->comm2, "callback", ci->focus, 0, ci->mark,
				 v ? "orig":"none", 0, NULL, ci->str);
	}
	return Efallthrough;
}

DEF_CMD(email_view_set_attr)
{
	int p, v;
	struct email_view *evi = ci->home->data;

	if (!ci->str || !ci->mark)
		return Enoarg;
	if (strcmp(ci->str, "email:visible") == 0) {
		struct mark *m1, *m2;

		p = get_part(ci->home->parent, ci->mark);
		/* only parts can be invisible, not separators */
		p = to_orig(p);
		v = ci->str2 && strcmp(ci->str2, "none") != 0;
		if (p >= 0 && p < evi->parts)
			evi->invis[p] = v ? 'v' : 'i';

		/* Tell viewers that visibility has changed */
		m1 = mark_dup(ci->mark);
		home_call(ci->home->parent, "doc:step-part", ci->focus,
			  0, m1);
		if (get_part(ci->home->parent, m1) != p)
			home_call(ci->home->parent, "doc:step-part",
				  ci->focus, -1, m1);

		mark_step(m1, 0);
		m2 = mark_dup(m1);
		home_call(ci->home->parent, "doc:step-part", ci->focus,
			  1, m2);
		call("view:changed", ci->focus, 0, m1, NULL, 0, m2);
		call("Notify:clip", ci->focus, 0, m1, NULL, 0, m2);
		mark_free(m1);
		mark_free(m2);

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

	m = vmark_new(ci->focus, MARK_UNGROUPED, NULL);
	if (!m)
		return Efail;
	call("doc:set-ref", ci->focus, 0, m);
	n = get_part(ci->focus, m);
	mark_free(m);
	if (n <= 0 || n > 1000 )
		return Einval;

	alloc(evi, pane);
	evi->parts = n;
	evi->invis = calloc(n+1, sizeof(char));
	memset(evi->invis, 'v', n);
	p = pane_register(ci->focus, 0, &email_view_handle.c, evi);
	if (!p) {
		free(evi);
		return Efail;
	}
	attr_set_str(&p->attrs, "render-hide-CR", "yes");
	return comm_call(ci->comm2, "callback:attach", p);
}

static void email_init_map(void)
{
	email_view_map = key_alloc();
	key_add(email_view_map, "Free", &email_view_free);
	key_add(email_view_map, "doc:step", &email_step);
	key_add(email_view_map, "doc:set-ref", &email_set_ref);
	key_add(email_view_map, "doc:set-attr", &email_view_set_attr);
	key_add(email_view_map, "doc:get-attr", &email_view_get_attr);
	key_add(email_view_map, "doc:email:render-spacer", &email_spacer);
	key_add(email_view_map, "doc:email:select", &email_select);
}

void edlib_init(struct pane *ed safe)
{
	email_init_map();
	call_comm("global-set-command", ed, &open_email, 0, NULL,
		  "open-doc-email");
	call_comm("global-set-command", ed, &attach_email_view, 0, NULL,
		  "attach-email-view");

	call("global-load-module", ed, 0, NULL, "lib-html-to-text");
	call("global-load-module", ed, 0, NULL, "lib-pdf-to-text");
}
