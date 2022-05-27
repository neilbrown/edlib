/*
 * Copyright Neil Brown ©2016-2021 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * doc-email: Present an email message as its intended content, with
 * part recognition and decoding etc.
 *
 * A multipart document is created from sets of three documents.
 * In each set:
 * - The first is the original email, overlayed with 'crop' to select
 *   one section, then overlayed with handlers for the transfer-encoding
 *   and (optionally) charset.  There will be one for the headers
 *   and either one for the body, or one for each part of the body
 *   is multipart.  All nested multiparts have their parts linearized.
 * - The second is a scratch text document which can contain a transformed
 *   copy of the content when the tranformation is too complex for an
 *   overlay layer.  This includes HTML and PDF which can be converted
 *   to text, but the conversion is complex, and the headers, which need to be
 *   re-ordered as well as filtered and decoded.  For images, this document has
 *   trivial content but specifier a rendering function that displays the image.
 * - The final section is a 'spacer' which has fixed content and displays
 *   as a summary line for the part (e.g. MIME-type, file name) and
 *   provides active buttons for acting on the content (save, external viewer
 *   etc).
 *
 * The middle part has attributes set on the document which can be
 * accessed form the spacer using "multipart-prev:"
 * - email:path identify the part in the nexted multipart struture
 *   e.g. "header", "body", "body,multipart/mixed:0,mulitpart/alternate:1"
 * - email:actions a ':' separated list of buttons. "hide:save:view"
 * - email:content-type the MIME type of the content. "image/png"
 *
 */

#define _GNU_SOURCE /* for asprintf */
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <wctype.h>
#include <ctype.h>
#include <stdio.h>
#include "core.h"
#include "misc.h"

static inline bool is_orig(int p)
{
	return p >= 0 && p % 3 == 0;
}

static inline bool is_transformed(int p)
{
	return p >= 0 && p % 3 == 1;
}

static inline bool is_spacer(int p)
{
	return p >= 0 && p % 3 == 2;
}

static inline int to_orig(int p)
{
	if (p < 0)
		return p;
	return p - p % 3;
}

static bool handle_content(struct pane *p safe,
			   char *type, char *xfer, char *disp,
			   struct mark *start safe, struct mark *end safe,
			   struct pane *mp safe, struct pane *spacer safe,
			   char *path safe);
static bool handle_rfc822(struct pane *email safe,
			  struct mark *start safe, struct mark *end safe,
			  struct pane *mp safe, struct pane *spacer safe,
			  char *path safe);

static bool cond_append(struct buf *b safe, char *txt safe, char *tag safe,
			int offset, int *cp safe)
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
		while (!mark_ordered_or_same(pm, m)) {
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
		char *a = strchr(attr, ':');
		if (a)
			*a = 0;
		if (is_attr("hide", attr))
			ok = cond_append(&b, visible ? "HIDE" : "SHOW", attr,
					 o, &cp);
		else
			ok = cond_append(&b, attr, attr, o, &cp);
		if (ok)
			doc_next(ci->focus, m);
		attr = a;
		if (attr)
			*attr++ = ':';
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
		attr = pane_mark_attr(ci->focus, m,
				      "multipart-prev:email:charset");
		if (attr) {
			buf_concat(&b, " ");
			buf_concat(&b, attr);
		}
		attr = pane_mark_attr(ci->focus, m,
				      "multipart-prev:email:filename");
		if (attr) {
			buf_concat(&b, " file=");
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

static int get_part(struct pane *p safe, struct mark *m safe)
{
	char *a = pane_mark_attr(p, m, "multipart:part-num");

	if (!a)
		return Efail;
	return atoi(a);
}

DEF_CMD(email_image)
{
	char *c = NULL;
	int p;
	int ret;

	if (!ci->mark)
		return Enoarg;
	p = get_part(ci->home, ci->mark);
	doc_next(ci->focus, ci->mark);
	asprintf(&c, "<image:comm:doc:multipart-%d-doc:get-bytes>\n", to_orig(p));
	ret = comm_call(ci->comm2, "callback:render", ci->focus,
			0, NULL, c);
	free(c);
	return ret;
}

DEF_CMD(email_select)
{
	/* If mark is on a button, press it... */
	struct mark *m = ci->mark;
	char *a, *e, *cmd = NULL;
	wint_t ch;
	int p;
	int b;

	if (!m)
		return Enoarg;
	p = get_part(ci->home, m);
	if (!is_spacer(p))
		return Efallthrough;
	ch = doc_following(ci->home, m);
	if (ch == WEOF || !isdigit(ch))
		return 1;
	b = ch - '0';
	a = pane_mark_attr(ci->focus, m, "multipart-prev:email:actions");
	if (!a)
		a = "hide";
	while (b > 0 && a) {
		a = strchr(a, ':');
		if (a)
			a += 1;
		b -= 1;
	}
	if (!a)
		return 1;
	e = strchr(a, ':');
	if (!e)
		e = a + strlen(a);
	asprintf(&cmd, "email:select:%.*s", (int)(e-a), a);
	if (!cmd)
		return Efail;
	return call(cmd, ci->focus, 0, m);
}

DEF_CMD(email_select_hide)
{
	int vis = 1;
	char *a;
	struct mark *m = ci->mark;

	if (!m)
		return Enoarg;

	a = pane_mark_attr(ci->focus, m, "email:visible");
	if (a && strcmp(a, "none") == 0)
		vis = 0;
	call("doc:set-attr", ci->focus, 1, m, "email:visible", 0, NULL,
	     vis ? "none" : "preferred");
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

static bool handle_text(struct pane *p safe, char *type, char *xfer, char *disp,
			struct mark *start safe, struct mark *end safe,
			struct pane *mp safe, struct pane *spacer safe,
			char *path)
{
	struct pane *h, *transformed = NULL;
	int need_charset = 0;
	char *charset = NULL;
	char *major, *minor = NULL;
	int majlen, minlen;
	char *ctype = NULL;
	char *fname = NULL;

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
		if (xfer && xlen == 4 &&
		    strncasecmp(xfer, "8bit", 6) == 0)
			need_charset = 2; // ony if not utf-8
	}
	if (type && need_charset &&
	    (charset = get_822_attr(type, "charset")) != NULL &&
	    !(need_charset == 2 && strcasecmp(charset, "utf-8") == 0)) {
		char *c = NULL, *cp;
		struct pane *hx = NULL;
		charset = strsave(h, charset);
		asprintf(&c, "attach-charset-%s", charset);
		for (cp = c; cp && *cp; cp++)
			if (isupper(*cp))
				*cp = tolower(*cp);
		if (c)
			hx = call_ret(pane, c, h);
		free(c);
		if (!hx)
			/* windows-1251 is safer than utf-8 as the latter
			 * rejects some byte sequences, and iso-8859-* has
			 * lots of control characters.
			 */
			hx = call_ret(pane, "attach-charset-windows-1251", h);
		if (hx)
			h = hx;
	}
	if (type && (fname = get_822_attr(type, "name")))
		fname = strsave(h, fname);
	if (disp && !fname &&
	    (fname = get_822_attr(disp, "filename")))
		fname = strsave(h, fname);
	major = get_822_token(&type, &majlen);
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
	if (ctype && strcasecmp(ctype, "text/html") == 0)
		transformed = call_ret(pane, "html-to-text", h);
	if (ctype && strcasecmp(ctype, "application/pdf") == 0)
		transformed = call_ret(pane, "pdf-to-text", h);
	if (ctype && strcasecmp(ctype, "application/octet-stream") == 0 &&
	    fname && strcasestr(fname, ".pdf") != NULL)
		transformed = call_ret(pane, "pdf-to-text", h);
	if (ctype && strncasecmp(ctype, "image/", 6) == 0) {
		struct mark *m;
		transformed = call_ret(pane, "doc:from-text", h,
				       0, NULL, NULL, 0, NULL, "\n");
		if (transformed) {
			m = vmark_new(transformed, MARK_UNGROUPED, NULL);
			call("doc:set-ref", transformed, 1, m);
			call("doc:set-attr", transformed, 1, m, "markup:func", 0,
			     NULL, "doc:email:render-image");
			mark_free(m);
		}
	}
	if (transformed) {
		attr_set_str(&transformed->attrs, "email:is_transformed", "yes");
		attr_set_str(&transformed->attrs, "email:preferred", "transformed");
	} else {
		transformed = call_ret(pane, "doc:from-text", h,
				       0, NULL, NULL, 0, NULL, "\n");
		if (transformed)
			attr_set_str(&transformed->attrs, "email:preferred",
				     "orig");
	}
	if (!transformed) {
		pane_close(h);
		return False;
	}
	call("doc:set:autoclose", transformed, 1);
	if (ctype) {
		int i;
		for (i = 0; ctype[i]; i++)
			if (isupper(ctype[i]))
				ctype[i] = tolower(ctype[i]);
		attr_set_str(&transformed->attrs, "email:content-type", ctype);
		free(ctype);
	} else
		attr_set_str(&h->attrs, "email:content-type", "text/plain");
	attr_set_str(&transformed->attrs, "email:actions", "hide:save");
	attr_set_str(&transformed->attrs, "email:path", path);
	attr_set_str(&transformed->attrs, "email:which", "transformed");
	if (charset)
		attr_set_str(&transformed->attrs, "email:charset", charset);
	if (fname)
		attr_set_str(&transformed->attrs, "email:filename", fname);
	attr_set_str(&h->attrs, "email:which", "orig");

	home_call(mp, "multipart-add", h);
	home_call(mp, "multipart-add", transformed);
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

	found_end = find_boundary(p, start, end, NULL, boundary);
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
		char *ptype, *pxfer, *pdisp;

		if (!hdr)
			break;
		call("get-header", hdr, 0, NULL, "content-type",
		     0, NULL, "cmd");
		call("get-header", hdr, 0, NULL, "content-transfer-encoding",
		     0, NULL, "cmd");
		call("get-header", hdr, 0, NULL, "content-disposition",
		     0, NULL, "cmd");
		ptype = attr_find(hdr->attrs, "rfc822-content-type");
		pxfer = attr_find(hdr->attrs,
				  "rfc822-content-transfer-encoding");
		pdisp = attr_find(hdr->attrs,
				  "rfc822-content-disposition");

		pane_close(hdr);

		newpath = NULL;
		asprintf(&newpath, "%s%s%1.*s:%d", path, path[0] ? ",":"",
			 len, tok, partnum);
		partnum++;

		if (part_end->seq < start->seq)
			mark_to_mark(part_end, start);
		handle_content(p, ptype, pxfer, pdisp, start, part_end, mp,
			       spacer, newpath ?:"");
		free(newpath);
		mark_to_mark(start, pos);
	}
	mark_to_mark(start, pos);
	mark_free(pos);
	mark_free(part_end);
	free(boundary);
	return True;
}

static bool handle_content(struct pane *p safe,
			   char *type, char *xfer, char *disp,
			   struct mark *start safe, struct mark *end safe,
			   struct pane *mp safe, struct pane *spacer safe,
			   char *path safe)
{
	char *hdr;
	char *major, *minor = NULL;
	int mjlen, mnlen;

	if (mark_ordered_or_same(end, start))
		return True;

	if (!type)
		type = "text/plain";
	hdr = type;

	major = get_822_token(&hdr, &mjlen);
	if (major) {
		minor = get_822_token(&hdr, &mnlen);
		if (minor && minor[0] == '/')
			minor = get_822_token(&hdr, &mnlen);
	}

	if (tok_matches(major, mjlen, "multipart"))
		return handle_multipart(p, type, start, end, mp, spacer, path);

	if (tok_matches(major, mjlen, "message") && tok_matches(minor, mnlen, "rfc822"))
		return handle_rfc822(p, start, end, mp, spacer, path);

	if (tok_matches(major, mjlen, "text") && tok_matches(minor, mnlen, "rfc822-headers"))
		return handle_rfc822(p, start, end, mp, spacer, path);

	if (major == NULL ||
	    tok_matches(major, mjlen, "text"))
		return handle_text(p, type, xfer, disp, start, end,
				   mp, spacer, path);

	/* default to plain text until we get a better default */
	return handle_text(p, type, xfer, disp, start, end, mp, spacer, path);
}

static bool handle_rfc822(struct pane *email safe,
			  struct mark *start safe, struct mark *end safe,
			  struct pane *mp safe, struct pane *spacer safe,
			  char *path safe)
{
	struct pane *h2;
	struct pane *hdrdoc = NULL;
	struct mark *point = NULL;
	char *xfer = NULL, *type = NULL, *disp = NULL;
	char *mime;
	char *newpath = NULL;

	h2 = call_ret(pane, "attach-rfc822header", email, 0, start, NULL, 0, end);
	if (!h2)
		goto out;
	attr_set_str(&h2->attrs, "email:which", "orig");

	hdrdoc = call_ret(pane, "doc:from-text", email, 0, NULL, NULL, 0, NULL, "");
	if (!hdrdoc)
		goto out;
	call("doc:set:autoclose", hdrdoc, 1);
	point = vmark_new(hdrdoc, MARK_POINT, NULL);
	if (!point)
		goto out;

	/* copy some headers to the header temp document */
	home_call(h2, "get-header", hdrdoc, 0, point, "From", 1);
	home_call(h2, "get-header", hdrdoc, 0, point, "Date", 1);
	home_call(h2, "get-header", hdrdoc, 0, point, "Subject", 1, NULL, "text");
	home_call(h2, "get-header", hdrdoc, 0, point, "To", 1, NULL, "list");
	home_call(h2, "get-header", hdrdoc, 0, point, "Cc", 1, NULL, "list");

	/* copy some headers into attributes for later analysis */
	call("get-header", h2, 0, NULL, "MIME-Version", 0, NULL, "cmd");
	call("get-header", h2, 0, NULL, "content-type", 0, NULL, "cmd");
	call("get-header", h2, 0, NULL, "content-transfer-encoding",
	     0, NULL, "cmd");
	call("get-header", h2, 0, NULL, "content-disposition",
	     0, NULL, "cmd");
	mime = attr_find(h2->attrs, "rfc822-mime-version");
	if (mime)
		mime = get_822_word(mime);
	/* Some email doesn't contain MIME-Type, but is still mime... */
	if (!mime || strcmp(mime, "1.0") == 0) {
		type = attr_find(h2->attrs, "rfc822-content-type");
		xfer = attr_find(h2->attrs, "rfc822-content-transfer-encoding");
		disp = attr_find(h2->attrs, "rfc822-content-disposition");
	}

	newpath = NULL;
	asprintf(&newpath, "%s%sheaders", path, path[0] ? ",":"");
	attr_set_str(&hdrdoc->attrs, "email:actions", "hide");
	attr_set_str(&hdrdoc->attrs, "email:which", "transformed");
	attr_set_str(&hdrdoc->attrs, "email:content-type", "text/rfc822-headers");
	attr_set_str(&hdrdoc->attrs, "email:path", newpath);
	attr_set_str(&hdrdoc->attrs, "email:is_transformed", "yes");
	home_call(mp, "multipart-add", h2);
	home_call(mp, "multipart-add", hdrdoc);
	home_call(mp, "multipart-add", spacer);
	free(newpath);

	newpath = NULL;
	asprintf(&newpath, "%s%sbody", path, path[0] ? ",":"");
	if (!handle_content(email, type, xfer, disp, start, end,
			    mp, spacer, newpath?:""))
		goto out;
	free(newpath);
	return True;
out:
	free(newpath);
	if (h2)
		pane_close(h2);
	if (point)
		mark_free(point);
	if (hdrdoc)
		pane_close(hdrdoc);
	return False;
}

DEF_CMD(open_email)
{
	int fd;
	struct pane *email = NULL;
	struct pane *spacer = NULL;
	struct mark *start, *end;
	struct pane *p;
	struct mark *point;

	if (ci->str == NULL ||
	    strncmp(ci->str, "email:", 6) != 0)
		return Efallthrough;
	fd = open(ci->str+6, O_RDONLY);
	if (fd < 0)
		return Efallthrough;
	p = call_ret(pane, "doc:open", ci->focus, fd, NULL, ci->str + 6, 1);
	close(fd);
	if (!p)
		return Efallthrough;
	start = vmark_new(p, MARK_UNGROUPED, NULL);
	if (!start) {
		pane_close(p);
		return Efallthrough;
	}
	end = mark_dup(start);
	call("doc:set-ref", p, 0, end);

	call("doc:set:autoclose", p, 1);
	email = p;

	/* create spacer doc to be attached between each part */
	p = call_ret(pane, "doc:from-text", p, 0, NULL, NULL, 0, NULL,
		     "\n0123456789\n");
	if (!p)
		goto out;

	attr_set_str(&p->attrs, "email:which", "spacer");
	call("doc:set:autoclose", p, 1);
	spacer = p;
	point = vmark_new(p, MARK_POINT, NULL);
	call("doc:set-ref", p, 1, point);
	doc_next(p, point);
	call("doc:set-attr", p, 1, point, "markup:func", 0,
	     NULL, "doc:email:render-spacer");
	mark_free(point);

	/* Create the multipart in which all the parts are assembled. */
	p = call_ret(pane, "attach-doc-multipart", ci->home);
	if (!p)
		goto out;
	call("doc:set:autoclose", p, 1);
	attr_set_str(&p->attrs, "render-default", "text");
	attr_set_str(&p->attrs, "filename", ci->str+6);
	attr_set_str(&p->attrs, "doc-type", "email");

	if (!handle_rfc822(email, start, end, p, spacer, ""))
		goto out;
	mark_free(start);
	mark_free(end);

	return comm_call(ci->comm2, "callback:attach", p);

out:
	mark_free(start);
	mark_free(end);
	if (spacer)
		pane_close(spacer);
	pane_close(email);
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

static int email_step(struct pane *home safe, struct mark *mark safe,
		      int forward, int move)
{
	struct pane *p = home;
	struct email_view *evi = p->data;
	wint_t ret;
	int n = -1;

	if (forward) {
		ret = home_call(p->parent, "doc:char", home,
				move ? 1 : 0,
				mark, evi->invis,
				move ? 0 : 1);
		n = get_part(p->parent, mark);
		if (move && is_spacer(n)) {
			/* Moving in a spacer, If after valid buttons,
			 * move to end
			 */
			wint_t c;
			unsigned int buttons;
			buttons = count_buttons(p, mark);
			while ((c = doc_following(p->parent, mark)) != WEOF
			       && iswdigit(c) && (c - '0') >= buttons)
					doc_next(p->parent, mark);
		}
	} else {
		ret = home_call(p->parent, "doc:char", home,
				move ? -1 : 0,
				mark, evi->invis,
				move ? 0 : -1);
		n = get_part(p->parent, mark);
		if (is_spacer(n) && move &&
		    ret != CHAR_RET(WEOF) && iswdigit(ret & 0x1fffff)) {
			/* Just stepped back over the 9 at the end of a spacer,
			 * Maybe step further if there aren't 10 buttons.
			 */
			unsigned int buttons = count_buttons(p, mark);
			wint_t c = ret & 0x1fffff;

			while (c != WEOF && iswdigit(c) && c - '0' >= buttons)
				c = doc_prev(p->parent, mark);
			ret = CHAR_RET(c);
		}
	}
	return ret;
}

DEF_CMD(email_char)
{
	struct mark *m = ci->mark;
	struct mark *end = ci->mark2;
	int steps = ci->num;
	int forward = steps > 0;
	int ret = Einval;

	if (!m)
		return Enoarg;
	if (end && mark_same(m, end))
		return 1;
	if (end && (end->seq < m->seq) != (steps < 0))
		/* Can never cross 'end' */
		return Einval;
	while (steps && ret != CHAR_RET(WEOF) && (!end || mark_same(m, end))) {
		ret = email_step(ci->home, m, forward, 1);
		steps -= forward*2 - 1;
	}
	if (end)
		return 1 + (forward ? ci->num - steps : steps - ci->num);
	if (ret == CHAR_RET(WEOF) || ci->num2 == 0)
		return ret;
	if (ci->num &&(ci->num2 < 0) == forward)
		return ret;
	/* Want the 'next' char */
	return email_step(ci->home, m, ci->num2 > 0, 0);
}

DEF_CMD(email_content)
{
	/* Call the multipart doc:content telling in
	 * what is invisible, marking all spacers as invisible
	 */
	struct pane *p = ci->home;
	struct email_view *evi = p->data;
	char *invis2 = strsave(p, evi->invis);
	int i;

	for (i = 0; invis2 && invis2[i]; i++)
		if (is_spacer(i))
			invis2[i] = 'i';
	return home_call(p->parent, ci->key, p,
			 ci->num, ci->mark, invis2,
			 ci->num2, ci->mark2, ci->str2,
			 ci->x, ci->y, ci->comm2);
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
	int p;
	char *v;
	struct email_view *evi = ci->home->data;

	if (!ci->str || !ci->mark)
		return Enoarg;
	if (strcmp(ci->str, "email:visible") == 0) {
		p = get_part(ci->home->parent, ci->mark);
		/* only parts can be invisible, not separators */
		p = to_orig(p);
		if (p < 0 || p >= evi->parts)
			v = "none";
		else if (evi->invis[p] != 'i')
			v = "orig";
		else if (evi->invis[p+1] != 'i')
			v = "transformed";
		else
			v = "none";

		return comm_call(ci->comm2, "callback", ci->focus, 0, ci->mark,
				 v, 0, NULL, ci->str);
	}
	return Efallthrough;
}

DEF_CMD(email_view_set_attr)
{
	int p;
	struct email_view *evi = ci->home->data;

	if (!ci->str || !ci->mark)
		return Enoarg;
	if (strcmp(ci->str, "email:visible") == 0) {
		struct mark *m1, *m2;
		const char *w;

		p = get_part(ci->home->parent, ci->mark);
		/* only parts can be invisible, not separators */
		p = to_orig(p);
		if (p < 0 || p >= evi->parts)
			return Efail;

		m1 = mark_dup(ci->mark);
		while (get_part(ci->home->parent, m1) > p &&
		       home_call(ci->home->parent, "doc:step-part",
				 ci->focus, -1, m1) > 0)
			;

		w = ci->str2;
		if (w && strcmp(w, "preferred") == 0) {
			w = pane_mark_attr(ci->focus, m1,
					   "multipart-next:email:preferred");
			if (!w)
				w = "orig";
		} else if (w && (strcmp(w, "orig") == 0 ||
				 strcmp(w, "transformed") == 0)) {
			call("doc:set-attr", ci->focus, 1, m1,
			     "multipart-next:email:preferred", 0, NULL, w);
		}
		evi->invis[p] = 'i';
		evi->invis[p+1] = 'i';
		if (w && strcmp(w, "orig") == 0)
			evi->invis[p] = 'v';
		if (w && strcmp(w, "transformed") == 0)
			evi->invis[p+1] = 'v';

		/* Tell viewers that visibility has changed */
		mark_step(m1, 0);
		m2 = mark_dup(m1);
		home_call(ci->home->parent, "doc:step-part", ci->focus,
			  1, m2);
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
	key_add(email_view_map, "doc:char", &email_char);
	key_add(email_view_map, "doc:content", &email_content);
	key_add(email_view_map, "doc:content-bytes", &email_content);
	key_add(email_view_map, "doc:set-ref", &email_set_ref);
	key_add(email_view_map, "doc:set-attr", &email_view_set_attr);
	key_add(email_view_map, "doc:get-attr", &email_view_get_attr);
	key_add(email_view_map, "doc:email:render-spacer", &email_spacer);
	key_add(email_view_map, "doc:email:render-image", &email_image);
	key_add(email_view_map, "doc:email:select", &email_select);
	key_add(email_view_map, "email:select:hide", &email_select_hide);
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
