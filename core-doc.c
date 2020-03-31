/*
 * Copyright Neil Brown ©2015-2020 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * All content managed in edlib is stored in documents.
 * There can be multiple document handlers which export the
 * doc_operations interface to provide access to a particular
 * style of document storage.
 * A document has a list of marks and points (managed in core-mark.c)
 * and some attributes (managed in attr.c).
 * It has a list of 'views' which are notified when the document changes.
 * Those are managed here.
 *
 * Finally all documents are kept in a single list which itself is
 * used as the basis for a document: the document-list.  The list is
 * kept in most-recently-used order.  Each document has a unique name
 * in this list.
 */

#define _GNU_SOURCE for strchrnul
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <stdio.h>
#include <fcntl.h>

#define PRIVATE_DOC_REF
struct doc_ref {
	struct pane	*p;
	int		ignore;
};

#include "core.h"
#include "misc.h"
#include "internal.h"

static void do_doc_assign(struct pane *p safe, struct pane *doc safe);
static struct pane *safe doc_attach(struct pane *parent);

static inline wint_t doc_following(struct doc *d safe, struct mark *m safe)
{
	return mark_step2(d, m, 1, 0);
}
static inline wint_t doc_prior(struct doc *d safe, struct mark *m safe)
{
	return mark_step2(d, m, 0, 0);
}

/* this is ->data for a document reference pane.
 */
struct doc_data {
	struct pane		*doc safe;
	struct mark		*point safe;
	struct mark		*marks[4];
};

static void doc_init(struct doc *d safe)
{
	INIT_HLIST_HEAD(&d->marks);
	INIT_TLIST_HEAD(&d->points, 0);
	d->views = NULL;
	d->nviews = 0;
	d->name = NULL;
	memset(d->recent_points, 0, sizeof(d->recent_points));
	d->autoclose = 0;
	d->readonly = 0;
	d->home = safe_cast NULL;
	d->refcnt = NULL;
}

struct pane *safe __doc_register(struct pane *parent,
				 struct command *handle safe,
				 struct doc *doc safe,
				 void *data safe,
				 short data_size)
{
	ASSERT(data == (void*)doc);
	/* Documents are always registered against the root */
	if (parent)
		parent = pane_root(parent);
	doc_init(doc);
	doc->home = __pane_register(parent, 0, handle, doc, data_size);
	return doc->home;
}

/* For these 'default commands', home->data is struct doc */
DEF_CMD(doc_char)
{
	struct pane *f = ci->focus;
	struct doc_data *dd = ci->home->data;
	struct mark *m = ci->mark;
	int rpt = RPT_NUM(ci);

	if (!m)
		m = dd->point;

	while (rpt > 0) {
		if (mark_step_pane(f, m, 1, 1) == WEOF)
			break;
		rpt -= 1;
	}
	while (rpt < 0) {
		if (mark_step_pane(f, m, 0, 1) == WEOF)
			break;
		rpt += 1;
	}

	return 1;
}

DEF_CMD(doc_word)
{
	struct pane *f = ci->focus;
	struct doc_data *dd = ci->home->data;
	struct mark *m = ci->mark;
	int rpt = RPT_NUM(ci);
	int dir;

	if (!m)
		m = dd->point;
	/* Move-word should finish at a word boundary, which usually
	 * means an alphanum (possibly including '_' depending on doc
	 * attributes?).  However it should never cross two different
	 * sets of spaces or punctuation.  So if we cross space and
	 * punct and don't find alphanum, then we treat end of punct as
	 * a word boundary.  We never stop immediately after a space.
	 * So skip spaces, then punct, then alphanum.
	 * Same in either direction.
	 */
	dir = rpt > 0 ? 1 : 0;
	while (rpt != 0) {
		wint_t wi;

		while (iswspace(mark_step_pane(f, m, dir, 0)))
			mark_step_pane(f, m, dir, 1);

		while ((wi=mark_step_pane(f, m, dir, 0)) != WEOF &&
		       !iswspace(wi) && !iswalnum(wi))
			mark_step_pane(f, m, dir, 1);

		if (iswalnum(mark_step_pane(f, m, dir, 0))) {
			while (iswalnum(mark_step_pane(f, m, dir, 0)))
				mark_step_pane(f, m, dir, 1);
		}

		rpt -= dir * 2 - 1;
	}

	return 1;
}

static int check_slosh(struct pane *p safe, struct mark *m safe)
{
	wint_t ch;
	/* Check is preceded by exactly 1 '\' */
	if (doc_prior_pane(p, m) != '\\')
		return 0;
	mark_step_pane(p, m, 0, 1);
	ch = doc_prior_pane(p, m);
	mark_step_pane(p, m, 1, 1);
	return ch != '\\';
}

DEF_CMD(doc_expr)
{
	/* doc_expr skips an 'expression' which is the same as a word
	 * unless we see open '({[' or close ')}]' or quote (\'\").
	 * We ignore quotes when preceeded by a single '\'
	 * If we see close going forward, or open going backward, we stop.
	 * If we see open going forward or close going backward, or quote,
	 * we skip to matching close/open/quote, allowing for nested
	 * open/close etc. Inside quotes, we stop at EOL.
	 * If num2 is 1, then if we reach a true 'open' we continue
	 * one more character to enter (going forward) or leave (backward)
	 * the expression.
	 * 'str' can be set to extra chars that should be included in words.
	 */
	struct pane *f = ci->focus;
	struct doc_data *dd = ci->home->data;
	struct mark *m = ci->mark;
	int rpt = RPT_NUM(ci);
	int enter_leave = ci->num2;
	int dir;
	char *open;
	char *close;
	const char *wordchars = ci->str ?: "";
	const char *special safe = "[](){}'\"";

	if (!m)
		m = dd->point;
	dir = rpt > 0 ? 1 : 0;
	if (dir) {
		open = "([{"; close = ")]}";
	} else {
		open = ")]}"; close = "([{";
	}
	while (rpt != 0) {
		wint_t wi = ' ';

		while (iswspace(mark_step_pane(f, m, dir, 0)))
			mark_step_pane(f, m, dir, 1);

		while ((wi = mark_step_pane(f, m, dir, 0)) != WEOF &&
		       !iswspace(wi) && !iswalnum(wi) &&
		       (wi > 255 || (strchr(special, wi) == NULL &&
				     strchr(wordchars, wi) == NULL)))
			mark_step_pane(f, m, dir, 1);

		if (strchr(close, wi)) {
			if (!dir && enter_leave) {
				mark_step_pane(f, m, dir, 1);
				rpt += 1;
			} else
				/* hit a close */
				break;
		} else if (strchr(open, wi)) {
			/* skip bracketed expression */
			int depth = 1;
			wint_t q = 0;

			mark_step_pane(f, m, dir, 1);
			if (enter_leave && dir)
				/* Just entered the expression */
				rpt -= 1;
			else while (depth > 0 &&
				    (wi = mark_step_pane(f, m, dir, 1)) != WEOF) {
					if (q) {
						if (dir)
							mark_step_pane(f, m, 0, 1);
						if ((!check_slosh(f, m) && wi == q) ||
						    is_eol(wi))
							q = 0;
						if (dir)
							mark_step_pane(f, m, 1, 1);
					} else if (strchr(open, wi))
						depth += 1;
					else if (strchr(close, wi))
						depth -= 1;
					else if (wi == '"' || wi == '\'') {
						if (dir)
							mark_step_pane(f, m, 0, 1);
						if (!check_slosh(f, m))
							q = wi;
						if (dir)
							mark_step_pane(f, m, 1, 1);
					}
				}
		} else if (wi == '"' || wi == '\'') {
			/* skip quoted or to EOL */
			wint_t q = wi;
			int slosh = 0;
			if (dir) {
				slosh = check_slosh(f, m);
				mark_step_pane(f, m, dir, 1);
			} else {
				mark_step_pane(f, m, dir, 1);
				slosh = check_slosh(f, m);
			}
			if (!slosh) {
				while (((wi = mark_step_pane(f, m, dir, 0))
					!= WEOF) &&
				       !is_eol(wi)) {
					if (dir) {
						slosh = check_slosh(f, m);
						mark_step_pane(f, m, dir, 1);
					} else {
						mark_step_pane(f, m, dir, 1);
						slosh = check_slosh(f, m);
					}
					if (wi == q && !slosh)
						break;
				}
			}
		} else while (iswalnum((wi=mark_step_pane(f, m, dir, 0))) ||
			      (wi > 0 && wi <= 255 &&
			       strchr(wordchars, wi) != NULL))
				mark_step_pane(f, m, dir, 1);

		if (!enter_leave)
			rpt -= dir * 2 - 1;
		if (wi == WEOF)
			break;
	}
	return rpt ? 2 : 1;
}

DEF_CMD(doc_WORD)
{
	struct pane *f = ci->focus;
	struct doc_data *dd = ci->home->data;
	struct mark *m = ci->mark;
	int rpt = RPT_NUM(ci);

	if (!m)
		m = dd->point;

	/* We skip spaces, then non-spaces */
	while (rpt > 0) {
		wint_t wi;

		while (iswspace(doc_following_pane(f, m)))
			mark_step_pane(f, m, 1, 1);

		while ((wi=doc_following_pane(f, m)) != WEOF &&
		       !iswspace(wi))
			mark_step_pane(f, m, 1, 1);
		rpt -= 1;
	}
	while (rpt < 0) {
		wint_t wi;

		while (iswspace(doc_prior_pane(f, m)))
			mark_step_pane(f, m, 0, 1);
		while ((wi=doc_prior_pane(f, m)) != WEOF &&
		       !iswspace(wi))
			mark_step_pane(f, m, 0, 1);
		rpt += 1;
	}

	return 1;
}

DEF_CMD(doc_eol)
{
	struct pane *f = ci->focus;
	struct doc_data *dd = ci->home->data;
	struct mark *m = ci->mark;
	wint_t ch = 1;
	int rpt = RPT_NUM(ci);

	if (!m)
		m = dd->point;

	while (rpt > 0 && ch != WEOF) {
		while ((ch = mark_next_pane(f, m)) != WEOF &&
		       !is_eol(ch))
			;
		rpt -= 1;
	}
	while (rpt < 0 && ch != WEOF) {
		while ((ch = mark_prev_pane(f, m)) != WEOF &&
		       !is_eol(ch))
			;
		rpt += 1;
	}
	if (is_eol(ch)) {
		if (RPT_NUM(ci) > 0)
			mark_prev_pane(f, m);
		else if (RPT_NUM(ci) < 0)
			mark_next_pane(f, m);
	}
	return 1;
}

DEF_CMD(doc_file)
{
	struct doc_data *dd = ci->home->data;
	int rpt = RPT_NUM(ci);
	struct mark *m = ci->mark;

	if (!m)
		m = dd->point;

	call("doc:set-ref", ci->focus, (rpt <= 0), m);

	return 1;
}

DEF_CMD(doc_line)
{
	struct pane *p = ci->focus;
	struct doc_data *dd = ci->home->data;
	struct mark *m = ci->mark;
	wint_t ch = 1;
	int rpt = RPT_NUM(ci);

	if (!m)
		m = dd->point;

	while (rpt > 0 && ch != WEOF) {
		while ((ch = mark_next_pane(p, m)) != WEOF &&
		       !is_eol(ch))
			;
		rpt -= 1;
	}
	while (rpt < 0 && ch != WEOF) {
		while ((ch = mark_prev_pane(p, m)) != WEOF &&
		       !is_eol(ch))
			;
		rpt += 1;
	}
	return 1;
}

DEF_CMD(doc_para)
{
	/* Default paragraph move - find blank line - two or more
	 * is_eol() chars.
	 * If moving forward, skip over all those chars.
	 * If moving backward, stop before the first one.
	 */
	struct pane *p = ci->focus;
	struct doc_data *dd = ci->home->data;
	struct mark *m = ci->mark;
	int rpt = RPT_NUM(ci);
	wint_t ch = 0;
	int nlcnt = 0;
	int forwards = rpt > 0 ? 1 : 0;

	if (!m)
		m = dd->point;

	while (!forwards && is_eol(doc_prior_pane(p, m)))
		mark_prev_pane(p, m);

	while (rpt && ch != WEOF) {
		nlcnt = 0;
		while (ch != WEOF) {
			ch = mark_step_pane(p, m, forwards, 1);
			if (is_eol(ch))
				nlcnt += 1;
			else if (nlcnt < 2)
				nlcnt = 0;
			else {
				mark_step_pane(p, m, !forwards, 1);
				break;
			}
		}
		rpt += forwards ? -1 : 1;
	}

	while (!forwards && nlcnt-- > 0)
		mark_next_pane(p, m);
	return 1;
}

DEF_CMD(doc_page)
{
	struct pane *p = ci->focus;
	struct doc_data *dd = ci->home->data;
	struct mark *m = ci->mark, *old;
	wint_t ch = 1;
	int rpt = RPT_NUM(ci);

	if (!m)
		m = dd->point;
	old = mark_dup(m);

	rpt *= p->h-2;
	while (rpt > 0 && ch != WEOF) {
		while ((ch = mark_next_pane(p, m)) != WEOF &&
		       !is_eol(ch))
			;
		rpt -= 1;
	}
	while (rpt < 0 && ch != WEOF) {
		while ((ch = mark_prev_pane(p, m)) != WEOF &&
		       !is_eol(ch))
			;
		rpt += 1;
	}
	if (mark_same(m, old)) {
		mark_free(old);
		return 2;
	}
	mark_free(old);
	return 1;
}

DEF_CMD(doc_set)
{
	struct doc *d = ci->home->data;
	const char *val = ksuffix(ci, "doc:set:");

	if (strcmp(val, "autoclose") == 0) {
		d->autoclose = ci->num;
		return 1;
	}
	if (strcmp(val, "readonly") == 0) {
		d->readonly = ci->num;
		call("doc:notify:doc:status-changed", d->home);
		return 1;
	}
	if (ci->str)
		attr_set_str(&d->home->attrs, val, ci->str);

	return 1;
}

DEF_CMD(doc_get_attr)
{
	struct doc *d = ci->home->data;
	char *a;

	if (!ci->str)
		return Enoarg;

	if ((a = attr_find(d->home->attrs, ci->str)) != NULL)
		;
	else if (strcmp(ci->str, "doc-name") == 0)
		a = d->name;
	else if (strcmp(ci->str, "doc-modified") == 0)
		a = "no";
	else if (strcmp(ci->str, "doc-readonly") == 0) {
		a = d->readonly ? "yes":"no";
	} else if (strcmp(ci->str, "dirname") == 0) {
		char *sl;
		char pathbuf[PATH_MAX];
		a = pane_attr_get(d->home, "filename");
		if (!a) {
			a = realpath(".", pathbuf);
			if (a != pathbuf && a)
				strcpy(pathbuf, a);
			if (pathbuf[1])
				strcat(pathbuf, "/");
			a = strsave(ci->focus, pathbuf);
		} else {
			sl = strrchr(a, '/');
			if (!sl)
				sl = a = "/";
			a = strnsave(ci->focus, a, (sl-a)+1);
		}
	}
	if (a)
		return comm_call(ci->comm2, "callback:get_attr", ci->focus, 0,
				 NULL, a);
	return 1;
}

DEF_CMD(doc_set_name)
{
	struct doc *d = ci->home->data;

	if (!ci->str)
		return Enoarg;
	free(d->name);
	d->name = strdup(ci->str);
	return call("doc:notify:doc:revisit", d->home, ci->num);
}

DEF_CMD(doc_request_notify)
{
	pane_add_notify(ci->focus, ci->home, ksuffix(ci, "doc:request:"));
	return 1;
}

DEF_CMD(doc_notify)
{
	/* Key is "doc:notify:..." */
	int ret = home_pane_notify(ci->home, ksuffix(ci, "doc:notify:"),
				   ci->home,
				   ci->num, ci->mark, ci->str,
				   ci->num2, ci->mark2, ci->str2, ci->comm2);
	return ret;
}

DEF_CMD(doc_delview)
{
	struct doc *d = ci->home->data;
	int i = ci->num;

	/* This view should only have points on the list, not typed
	 * marks.  Just delete everything and clear the 'notify' pointer
	 */
	if (i < 0 || i >= d->nviews || d->views == NULL)
		return Einval;
	if (d->views[i].owner != ci->focus) abort();
	d->views[i].owner = NULL;
	while (!tlist_empty(&d->views[i].head)) {
		struct tlist_head *tl = d->views[i].head.next;
		if (TLIST_TYPE(tl) != GRP_LIST)
			abort();
		tlist_del_init(tl);
	}

	return 1;
}

DEF_CMD(doc_addview)
{
	struct doc *d = ci->home->data;
	struct docview *g;
	int ret;
	int i;

	for (ret = 0; d->views && ret < d->nviews; ret++)
		if (d->views[ret].owner == NULL)
			break;
	if (!d->views || ret == d->nviews) {
		/* Resize the view list */
		d->nviews += 4;
		g = alloc_buf(sizeof(*g) * d->nviews, pane);
		for (i = 0; d->views && i < ret; i++) {
			tlist_add(&g[i].head, GRP_HEAD, &d->views[i].head);
			tlist_del(&d->views[i].head);
			g[i].owner = d->views[i].owner;
		}
		for (; i < d->nviews; i++) {
			INIT_TLIST_HEAD(&g[i].head, GRP_HEAD);
			g[i].owner = NULL;
		}
		unalloc_buf(d->views, sizeof(*g)*(d->nviews - 4), pane);
		d->views = g;
		/* now resize all the points */
		points_resize(d);
	}
	if (d->views /* FIXME always true */) {
		points_attach(d, ret);
		d->views[ret].owner = ci->focus;
		// FIXME get close notificiation
	}
	return 1 + ret;
}

DEF_CMD(doc_vmarkget)
{
	struct mark *m, *m2;
	m = do_vmark_first(ci->home->data, ci->num, ci->focus);
	m2 = do_vmark_last(ci->home->data, ci->num, ci->focus);
	if (ci->num2 == 1 && ci->mark)
		m2 = do_vmark_at_point(ci->home->data, ci->mark,
				       ci->num, ci->focus);
	if (ci->num2 == 2)
		m2 = doc_new_mark(ci->home->data, ci->num, ci->focus);
	if (ci->num2 == 3 && ci->mark)
		m2 = do_vmark_at_or_before(ci->home->data, ci->mark,
					   ci->num, ci->focus);
	return comm_call(ci->comm2, "callback:vmark", ci->focus,
			 0, m, NULL, 0, m2);
}

DEF_CMD(doc_drop_cache)
{
	struct pane *p = ci->home;
	struct doc *d = p->data;

	if (d->autoclose)
		pane_close(p);
	return 1;
}

DEF_CMD(doc_delayed_close)
{
	struct pane *p = ci->home;
	int ret;

	/* If there are any doc-displays open, then will return '1' and
	 * we will know not to destroy document yet.
	 */
	ret = pane_notify("doc:notify-viewers", p);
	if (ret == 0)
		call("doc:drop-cache", p);
	return 1;
}

DEF_CMD(doc_do_closed)
{
	struct pane *p = ci->home;
	struct pane *child;

	/* Close the path of filters from doc to focus */
	child = pane_my_child(p, ci->focus);
	if (child)
		pane_close(child);

	call_comm("editor-on-idle", p, &doc_delayed_close);
	return 1;
}

DEF_CMD(doc_do_destroy)
{
	pane_close(ci->home);
	return 1;
}

DEF_CMD(doc_get_point)
{
	struct doc_data *dd = ci->home->data;
	int mnum = 0;

	if (ci->num >= 1 && ci->num <= 4)
		mnum = ci->num - 1;

	comm_call(ci->comm2, "callback", ci->focus, 0, dd->point, NULL,
		  0, dd->marks[mnum]);
	return 1;
}

DEF_CMD(doc_default_content)
{
	/* doc:content delivers one char at a time to a callback.
	 * The chars are the apparent content, rather than the actual
	 * content.  So for a directory listing, it is the listing, not
	 * one newline per file.
	 * This is used for 'search' and 'copy'.
	 * This default version calls doc:step and is used when the actual
	 * and apparent content are the same.
	 *
	 * .mark is 'location': to start.  This is moved forwards
	 * .comm2 is 'consume': pass char mark and report if finished.
	 *
	 */
	struct mark *m = ci->mark;
	struct commcache dstep = CCINIT;
	int nxt;

	if (!m || !ci->comm2)
		return Enoarg;

	nxt = ccall(&dstep, "doc:step", ci->home, 1, m);
	while (nxt != CHAR_RET(WEOF) &&
	       comm_call(ci->comm2, "consume", ci->home, nxt, m) > 0) {
		ccall(&dstep, "doc:step", ci->home, 1, m, NULL, 1);
		nxt = ccall(&dstep, "doc:step", ci->home, 1, m);
	}

	return 1;
}

struct getstr {
	struct buf b;
	struct mark *end;
	struct command c;
};

DEF_CMD(get_str_callback)
{
	wint_t wch = ci->num & 0xFFFFF;
	struct getstr *g = container_of(ci->comm, struct getstr, c);

	if (!ci->mark)
		return 0;
	if (g->end && ci->mark->seq >= g->end->seq)
		return 0;
	buf_append(&g->b, wch);
	return 1;
}

DEF_CMD(doc_get_str)
{
	/* Default doc_get_str
	 * uses doc:content to collect the content
	 * into a buf.
	 */
	struct getstr g;
	struct mark *from = NULL, *to = NULL, *m;

	if (ci->mark && ci->mark2) {
		if (ci->mark2->seq < ci->mark->seq) {
			from = ci->mark2;
			to = ci->mark;
		} else {
			from = ci->mark;
			to = ci->mark2;
		}
	}

	g.c = get_str_callback;
	buf_init(&g.b);
	g.end = to;
	if (from)
		m = mark_dup(from);
	else
		m = vmark_new(ci->focus, MARK_UNGROUPED, NULL);
	if (!m)
		return Efail;
	call_comm("doc:content", ci->focus, &g.c, 0, m);
	mark_free(m);
	comm_call(ci->comm2, "callback:get-str", ci->focus, 0, NULL,
		  buf_final(&g.b));
	free(g.b.b);
	return 1;
}

DEF_CMD(doc_write_file)
{
	/* Default write-file handler
	 * We just step through the file writing each character
	 * Requires that "doc:charset" attribute to be either "utf-8"
	 * or "8bit".
	 */
	struct mark *m;
	wint_t ch = 0;
	FILE *f = NULL;
	int ret = 1;
	int utf8 = 1;
	char *charset;

	charset = pane_attr_get(ci->focus, "doc:charset");
	if (charset && strcmp(charset, "8bit") == 0)
		utf8 = 0;
	else if (charset && strcmp(charset, "utf-8") == 0)
		utf8 = 1;
	else
		return Enosup;

	if (ci->str)
		f = fopen(ci->str, "w");
	else if (ci->num >= 0 && ci->num != NO_NUMERIC)
		f = fdopen(dup(ci->num), "w");
	else
		return Enoarg;
	if (!f)
		return Efail;

	if (ci->mark)
		m = mark_dup(ci->mark);
	else
		m = vmark_new(ci->focus, MARK_UNGROUPED, NULL);

	while(m) {
		ch = mark_next_pane(ci->focus, m);
		if (ch == WEOF)
			break;
		if (ci->mark2 && mark_ordered_not_same(ci->mark2, m))
			break;
		if (utf8) {
			if (ch <= 0x7f)
				fputc(ch, f);
			else if (ch < 0x7ff) {
				fputc(((ch>>6) & 0x1f) | 0xc0, f);
				fputc((ch & 0x3f) | 0x80, f);
			} else if (ch < 0xFFFF) {
				fputc(((ch>>12) & 0x0f) | 0xe0, f);
				fputc(((ch>>6) & 0x3f) | 0x80, f);
				fputc((ch & 0x3f) | 0x80, f);
			} else if (ch < 0x10FFFF) {
				fputc(((ch>>18) & 0x07) | 0xf0, f);
				fputc(((ch>>12) & 0x3f) | 0x80, f);
				fputc(((ch>>6) & 0x3f) | 0x80, f);
				fputc((ch & 0x3f) | 0x80, f);
			}
		} else
			fputc(ch, f);
	}
	if (fflush(f))
		ret = Efail;
	fclose(f);
	mark_free(m);
	return ret;
}

DEF_CMD(doc_notify_viewers)
{
	/* The autoclose document wants to know if it should close,
	 * or a new view wants to find an active point.
	 * If a mark was provided, move it to point, then
	 * report that there are active viewers by returning 1
	 */
	struct doc_data *dd = ci->home->data;

	if (ci->mark)
		mark_to_mark(ci->mark, dd->point);
	return 1;
}

DEF_CMD(doc_notify_moved)
{
	struct doc_data *dd = ci->home->data;

	if (ci->mark == dd->point)
		pane_damaged(ci->home, DAMAGED_CURSOR);
	return 0;
}

DEF_CMD(doc_refresh)
{
	struct doc_data *dd = ci->home->data;

	mark_ack(dd->point);
	return 0;
}

DEF_CMD(doc_notify_close)
{
	/* This pane has to go away */
	struct doc_data *dd = ci->home->data;

	mark_free(dd->point);
	dd->point = safe_cast NULL;
	pane_close(ci->home);
	return 1;
}

DEF_CMD(doc_clone)
{
	struct doc_data *dd = ci->home->data;
	struct pane *p = doc_attach(ci->focus);

	do_doc_assign(p, dd->doc);
	call("Move-to", p, 0, dd->point);
	pane_clone_children(ci->home, p);
	return 1;
}

DEF_CMD(doc_close)
{
	struct doc_data *dd = ci->home->data;
	int i;
	call("doc:push-point", dd->doc, 0, dd->point);
	mark_free(dd->point);
	for (i = 0; i < 4; i++)
		mark_free(dd->marks[i]);
	call("doc:closed", dd->doc);
	return 1;
}

DEF_CMD(doc_dup_point)
{
	struct doc_data *dd = ci->home->data;
	struct mark *pt = dd->point;
	struct mark *m;
	if (ci->mark && ci->mark->viewnum == MARK_POINT)
		pt = ci->mark;

	if (!pt || !ci->comm2)
		return Enoarg;

	if (ci->num2 == MARK_POINT)
		m = point_dup(pt);
	else if (ci->num2 == MARK_UNGROUPED)
		m = mark_dup(pt);
	else
		m = do_mark_at_point(pt, ci->num2);

	comm_call(ci->comm2, "callback:dup-point", ci->focus,
		  0, m);
	return 1;
}

DEF_CMD(doc_replace)
{
	struct doc_data *dd = ci->home->data;
	return home_call(dd->doc, "doc:replace", ci->focus,
			 1, ci->mark, ci->str,
			 ci->num2, dd->point, ci->str2);
}

DEF_CMD(doc_handle_get_attr)
{
	struct doc_data *dd = ci->home->data;
	char *a;
	if (!ci->str)
		return Enoarg;
	a = pane_attr_get(dd->doc, ci->str);
	if (!a)
		return Efallthrough;
	return comm_call(ci->comm2, "callback", ci->focus, 0, NULL, a);
}

DEF_CMD(doc_move_to)
{
	struct doc_data *dd = ci->home->data;
	struct mark *m;

	if (ci->num == 0) {
		if (ci->mark)
			mark_to_mark(dd->point, ci->mark);
	} else if (ci->num > 0 && ci->num <= 4) {
		int mnum = ci->num - 1;

		if (!dd->marks[mnum]) {
			dd->marks[mnum] = mark_dup(dd->point);
			if (!dd->marks[mnum])
				return Efail;
			if (mnum == 0)
				attr_set_str(&dd->marks[mnum]->attrs,
					     "render:interactive-mark", "yes");
		}
		m = ci->mark ?: dd->point;
		mark_to_mark(dd->marks[mnum], m);
		/* Make sure mark is *before* point so insertion
		 * leaves mark alone */
		mark_make_first(dd->marks[mnum]);
	} else if (ci->num < 0 && ci->num >= -4) {
		int mnum = -1 - ci->num;

		mark_free(dd->marks[mnum]);
		dd->marks[mnum] = NULL;
	} else
		return Efail;
	return 1;
}

DEF_CMD(doc_clip)
{
	struct doc_data *dd = ci->home->data;
	int mnum;

	mark_clip(dd->point, ci->mark, ci->mark2);
	for (mnum = 0; mnum < 4; mnum++)
		if (dd->marks[mnum])
			mark_clip(dd->marks[mnum], ci->mark, ci->mark2);
	return 1;
}

DEF_CMD(doc_pass_on)
{
	struct doc_data *dd = ci->home->data;
	int ret = home_call(dd->doc, ci->key, ci->focus, ci->num,
			    ci->mark ?: dd->point, ci->str,
			    ci->num2, ci->mark2, ci->str2,
			    ci->x, ci->y, ci->comm2);
	return ret;
}

DEF_CMD(doc_push_point)
{
	struct doc *d = ci->home->data;
	int n = ARRAY_SIZE(d->recent_points);
	if (!ci->mark)
		return Enoarg;
	mark_free(d->recent_points[n-1]);
	memmove(&d->recent_points[1],
		&d->recent_points[0],
		(n-1)*sizeof(d->recent_points[0]));
	d->recent_points[0] = mark_dup(ci->mark);
	return 1;
}

DEF_CMD(doc_pop_point)
{
	struct doc *d = ci->home->data;
	int n = ARRAY_SIZE(d->recent_points);

	if (!ci->mark)
		return Enoarg;
	if (!d->recent_points[0])
		return Efail;
	mark_to_mark(ci->mark, d->recent_points[0]);
	mark_free(d->recent_points[0]);
	memmove(&d->recent_points[0],
		&d->recent_points[1],
		(n-1) * sizeof(d->recent_points[0]));
	d->recent_points[n-1] = NULL;
	return 1;
}

DEF_CMD(doc_attach_view)
{
	struct pane *focus = ci->focus;
	struct pane *doc = ci->home;
	struct pane *p, *p2;
	char *s;
	const char *type = ci->str ?: "default";

	p = doc_attach(focus);
	if (!p)
		return Efail;
	do_doc_assign(p, doc);

	call("doc:notify:doc:revisit", p, ci->num);
	if (strcmp(type, "invisible") != 0) {
		/* Attach renderer */
		p2 = call_ret(pane, "attach-view", p);
		if (!p2)
			goto out;
		p = p2;

		s = strconcat(p, "render-", type);
		if (s)
			s = pane_attr_get(doc, s);
		if (!s)
			s = pane_attr_get(doc, "render-default");
		if (!s)
			goto out;
		s = strconcat(p, "attach-render-", s);
		p2 = call_ret(pane, s, p);
		if (!p2)
			goto out;
		p = p2;

		s = strconcat(p, "view-", type);
		if (s)
			s = pane_attr_get(doc, s);
		if (!s)
			s = pane_attr_get(doc, "view-default");
		if (s) {
			char *s2;
			while ((s2 = strchr(s, ',')) != NULL) {
				char *s3 = strndup(s, s2-s);
				p2 = call_ret(pane, strconcat(p, "attach-", s3)
					      , p);
				free(s3);
				if (p2)
					p = p2;
				s = s2+1;
			}
			s = strconcat(p, "attach-", s);
			p2 = call_ret(pane, s, p);
			if (p2)
				p = p2;
		}
	}
out:
	comm_call(ci->comm2, "callback:doc", p);
	return 1;
}

DEF_CMD(doc_abort)
{
	struct doc_data *dd = ci->home->data;

	call("doc:notify:Abort", dd->doc);
	return 0;
}


struct map *doc_default_cmd safe;
static struct map *doc_handle_cmd safe;

DEF_LOOKUP_CMD(doc_handle, doc_handle_cmd);

static void init_doc_cmds(void)
{
	doc_default_cmd = key_alloc();
	doc_handle_cmd = key_alloc();

	key_add_prefix(doc_handle_cmd, "doc:", &doc_pass_on);

	key_add(doc_handle_cmd, "Move-Char", &doc_char);
	key_add(doc_handle_cmd, "Move-Word", &doc_word);
	key_add(doc_handle_cmd, "Move-Expr", &doc_expr);
	key_add(doc_handle_cmd, "Move-WORD", &doc_WORD);
	key_add(doc_handle_cmd, "Move-EOL", &doc_eol);
	key_add(doc_handle_cmd, "Move-File", &doc_file);
	key_add(doc_handle_cmd, "Move-Line", &doc_line);
	key_add(doc_handle_cmd, "Move-Paragraph", &doc_para);
	key_add(doc_handle_cmd, "Move-View-Large", &doc_page);
	key_add(doc_handle_cmd, "doc:point", &doc_get_point);

	key_add(doc_handle_cmd, "doc:notify-viewers", &doc_notify_viewers);
	key_add(doc_handle_cmd,	"Notify:Close", &doc_notify_close);
	key_add(doc_handle_cmd,	"point:moved", &doc_notify_moved);
	key_add(doc_handle_cmd,	"Refresh", &doc_refresh);
	key_add(doc_handle_cmd,	"Clone", &doc_clone);
	key_add(doc_handle_cmd,	"Close", &doc_close);
	key_add(doc_handle_cmd,	"Free", &edlib_do_free);
	key_add(doc_handle_cmd, "doc:dup-point", &doc_dup_point);
	key_add(doc_handle_cmd, "Replace", &doc_replace);
	key_add(doc_handle_cmd, "get-attr", &doc_handle_get_attr);
	key_add(doc_handle_cmd, "Move-to", &doc_move_to);
	key_add(doc_handle_cmd, "Notify:clip", &doc_clip);
	key_add(doc_handle_cmd, "Abort", &doc_abort);

	key_add(doc_default_cmd, "doc:add-view", &doc_addview);
	key_add(doc_default_cmd, "doc:del-view", &doc_delview);
	key_add(doc_default_cmd, "doc:vmark-get", &doc_vmarkget);
	key_add(doc_default_cmd, "get-attr", &doc_get_attr);
	key_add(doc_default_cmd, "doc:set-name", &doc_set_name);
	key_add(doc_default_cmd, "doc:destroy", &doc_do_destroy);
	key_add(doc_default_cmd, "doc:drop-cache", &doc_drop_cache);
	key_add(doc_default_cmd, "doc:closed", &doc_do_closed);
	key_add(doc_default_cmd, "doc:get-str", &doc_get_str);
	key_add(doc_default_cmd, "doc:write-file", &doc_write_file);
	key_add(doc_default_cmd, "doc:content", &doc_default_content);
	key_add(doc_default_cmd, "doc:push-point", &doc_push_point);
	key_add(doc_default_cmd, "doc:pop-point", &doc_pop_point);
	key_add(doc_default_cmd, "doc:attach-view", &doc_attach_view);

	key_add_prefix(doc_default_cmd, "doc:request:",
		       &doc_request_notify);
	key_add_prefix(doc_default_cmd, "doc:notify:", &doc_notify);
	key_add_prefix(doc_default_cmd, "doc:set:", &doc_set);
}

static void do_doc_assign(struct pane *p safe, struct pane *doc safe)
{
	struct doc_data *dd = p->data;
	struct mark *m;

	m = vmark_new(doc, MARK_POINT, NULL);
	if (!m)
		return;
	if (call("doc:pop-point", doc, 0, m) <= 0)
		pane_notify("doc:notify-viewers", doc, 0, m);
	dd->doc = doc;
	dd->point = m;
	attr_set_str(&m->attrs, "render:interactive-point", "yes");

	pane_add_notify(p, doc, "Notify:Close");
	pane_add_notify(p, doc, "doc:notify-viewers");
	pane_add_notify(p, doc, "point:moved");
	call("doc:notify:doc:revisit", doc, 0);
}

static struct pane *safe doc_attach(struct pane *parent)
{
	struct doc_data *dd;

	alloc(dd, pane);
	return pane_register(parent, 0, &doc_handle.c, dd);
}

static void simplify_path(char *path safe, char *buf safe)
{
	/* Like readpath, but doesn't process symlinks,
	 * so only "..", "." and extra '/' are handled
	 * Assumes that 'path' starts with a '/'.
	 */
	char *p;
	char *end;
	char *b = buf;
	for (p = path; *p; p = end) {
		int len;
		end = strchrnul(p+1, '/');
		len = end - p;

		if (len == 1)
			/* Extra '/' at end or in the middle, ignore */
			continue;
		if (len == 2 && strncmp(p, "/.", 2) == 0 )
			/* Ignore the dot */
			continue;
		if (len == 3 && strncmp(p, "/..", 3) == 0) {
			/* strip last component of buf */
			while (b > buf && b[-1] != '/')
				b -= 1;
			if (b > buf)
				b -= 1;
			continue;
		}
		/* Append component to buf */
		strncpy(b, p, len);
		b += len;
	}
	if (b == buf)
		/* This is the only case where we allow a trailing '/' */
		*b++ = '/';
	*b = 0;
}

DEF_CMD(doc_open)
{
	struct pane *ed = ci->home;
	int fd = ci->num;
	const char *name = ci->str;
	struct stat stb;
	struct pane *p;
	int autoclose = ci->num2 & 1;
	int create_ok = ci->num2 & 4;
	int reload = ci->num2 & 8;
	int force_reload = ci->num2 & 16;
	int quiet = ci->num2 & 32;
	char pathbuf[PATH_MAX], *rp = NULL;

	if (!name)
		return Enoarg;
	stb.st_mode = 0;
	if (fd >= -1) {
		/* Try to canonicalize directory part of path */
		const char *sl;
		sl = strrchr(name, '/');
		if (!sl) {
			rp = realpath(".", pathbuf);
			sl = name;
		} else if (sl-name < PATH_MAX-4) {
			char nbuf[PATH_MAX];
			strncpy(nbuf, name, sl-name);
			nbuf[sl-name] = 0;
			simplify_path(nbuf, pathbuf);
			rp = pathbuf;
			sl += 1;
		}

		if (rp) {
			char nbuf[PATH_MAX];
			strcpy(nbuf, rp);
			if (rp[1])
				strcat(nbuf, "/");
			strcat(nbuf, sl);
			simplify_path(nbuf, pathbuf);
			name = pathbuf;
		}
	}

	if (fd == -1)
		/* No open yet */
		fd = open(name, O_RDONLY);

	if (fd >= 0)
		fstat(fd, &stb);
	else if (create_ok)
		stb.st_mode = S_IFREG;
	else
		stb.st_mode = 0;

	p = call_ret(pane, "docs:byfd", ed, 0, NULL, rp, fd);

	if (!p) {
		p = call_ret(pane, "global-multicall-open-doc-", ed,
			     fd, NULL, name,
			     stb.st_mode & S_IFMT);

		if (!p) {
			if (fd != ci->num)
				close(fd);
			return Efail;
		}
		if (autoclose)
			call("doc:set:autoclose", p, 1);
		call("doc:load-file", p, 0, NULL, name, fd);
		call("global-multicall-doc:appeared-", p);
	} else {
		char *n;
		n = pane_attr_get(p, "filename");
		if (n && strlen(n) > 1 && n[strlen(n)-1] == '/' && rp)
			/* Make sure both end in '/' if either do */
			strcat(rp, "/");
		if (!quiet && n && rp && strcmp(n, rp) != 0)
			call("Message", ci->focus, 0, NULL,
			     strconcat(ci->focus, "File ", rp, " and ", n,
				       " are the same"));
		if (reload || force_reload)
			call("doc:load-file", p, force_reload?0:1, NULL, name,
			     fd);
	}
	if (fd != ci->num)
		close(fd);
	return comm_call(ci->comm2, "callback", p);
}

DEF_CMD(doc_from_text)
{
	const char *name = ci->str;
	const char *text = ci->str2;
	struct pane *p;

	p = call_ret(pane, "attach-doc-text", ci->focus);
	if (!p)
		return Efail;
	if (name) {
		call("doc:set-name", p, 0, NULL, name);
		call("global-multicall-doc:appeared-", p);
	}
	call("doc:replace", p, 1, NULL, text);
	return comm_call(ci->comm2, "callback", p);
}

void doc_free(struct doc *d safe)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(d->recent_points); i++) {
		mark_free(d->recent_points[i]);
		d->recent_points[i] = NULL;
	}
	for (i = 0; i < (unsigned int)d->nviews; i++)
		ASSERT(d->views && !d->views[i].owner);
	unalloc_buf(d->views, sizeof(d->views[0]) * d->nviews, pane);
	free(d->name);
	while (!hlist_empty(&d->marks)) {
		struct mark *m = hlist_first_entry(&d->marks, struct mark, all);
		if (m->viewnum == MARK_POINT || m->viewnum == MARK_UNGROUPED)
			mark_free(m);
		else
			/* vmarks should have gone already */
			ASSERT(0);
	}
}

void doc_setup(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &doc_open, 0, NULL, "doc:open");
	call_comm("global-set-command", ed, &doc_from_text, 0, NULL,
		  "doc:from-text");
	if (!(void*)doc_default_cmd)
		init_doc_cmds();
}
