/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
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

#define PANE_DATA_TYPE struct doc_data
#define DOC_DATA_TYPE struct doc
#include "core.h"
#include "misc.h"
#include "internal.h"

/* this is ->data for a document reference pane.
 */
struct doc_data {
	struct pane		*doc safe;
	struct mark		*point safe;
	struct mark		*old_point; /* location at last refresh */
	struct mark		*marks[4];
};
#include "core-pane.h"

static struct pane *doc_attach_assign(struct pane *parent safe, struct pane *doc safe);


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
	d->refcnt = NULL;
}

struct pane *do_doc_register(struct pane *parent safe,
			     struct command *handle safe,
			     unsigned short data_size)
{
	struct pane *p;

	if (data_size < sizeof(struct doc))
		/* Not enough room for the doc ! */
		return NULL;

	/* Documents are always registered against the root */
	parent = pane_root(parent);
	p = do_pane_register(parent, 0, handle, NULL, data_size);
	if (!p)
		return p;
	doc_init(&p->doc);
	return p;
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
		if (doc_next(f,m) == WEOF)
			break;
		rpt -= 1;
	}
	while (rpt < 0) {
		if (doc_prev(f,m) == WEOF)
			break;
		rpt += 1;
	}

	return 1;
}

DEF_CMD(doc_word)
{
	/* Step to the Nth word boundary in appropriate
	 * direction.  If N is 0, don't move.
	 * Return 1 if succeeded before EOF, else Efalse.
	 */
	struct pane *f = ci->focus;
	struct mark *m = ci->mark;
	int rpt = RPT_NUM(ci);
	wint_t wi = 0;

	if (!m)
		return Enoarg;
	/* doc:word should finish at a word boundary, which usually
	 * means an alphanum (possibly including '_' depending on doc
	 * attributes?).  However it should never cross two different
	 * sets of spaces or punctuation.  So if we cross space and
	 * punct and don't find alphanum, then we treat end of punct as
	 * a word boundary.  We never stop immediately after a space.
	 * So skip spaces, then punct, then alphanum.
	 * Same in either direction.
	 */

	while (rpt > 0 && wi != WEOF) {
		while ((wi = doc_following(f, m)) != WEOF &&
		       iswspace(wi))
			doc_next(f, m);
		while ((wi = doc_following(f, m)) != WEOF &&
		       !iswspace(wi) && !iswalnum(wi))
			doc_next(f, m);
		while ((wi = doc_following(f, m)) != WEOF &&
		       iswalnum(wi))
			doc_next(f, m);
		rpt -= 1;
	}
	while (rpt < 0 && wi != WEOF) {
		while ((wi = doc_prior(f, m)) != WEOF &&
		       iswspace(wi))
			doc_prev(f, m);
		while ((wi = doc_prior(f, m)) != WEOF &&
		       !iswspace(wi) && !iswalnum(wi))
			doc_prev(f, m);
		while ((wi = doc_prior(f, m)) != WEOF &&
		       iswalnum(wi))
			doc_prev(f, m);
		rpt += 1;
	}
	return rpt == 0 ? 1 : Efalse;
}

static bool check_slosh(struct pane *p safe, struct mark *m safe)
{
	wint_t ch;
	/* Check is preceded by exactly 1 '\' */
	if (doc_prior(p, m) != '\\')
		return False;
	doc_prev(p,m);
	ch = doc_prior(p, m);
	doc_next(p,m);
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
	struct mark *m = ci->mark;
	int rpt = RPT_NUM(ci);
	int enter_leave = ci->num2;
	int dir = rpt > 0 ? 1 : -1;
	char *open;
	char *close;
	const char *wordchars = ci->str ?: "";
	const char *special safe = "[](){}'\"";

	if (!m)
		return Enoarg;
	if (dir > 0) {
		open = "([{"; close = ")]}";
	} else {
		open = ")]}"; close = "([{";
	}
	while (rpt != 0) {
		wint_t wi;

		while ((wi = doc_pending(f, m, dir)) != WEOF
		       && iswspace(wi))
			doc_move(f, m, dir);

		while ((wi = doc_pending(f, m, dir)) != WEOF &&
		       !iswspace(wi) && !iswalnum(wi) &&
		       (wi > 255 || (strchr(special, wi) == NULL &&
				     strchr(wordchars, wi) == NULL)))
			doc_move(f, m, dir);

		if (strchr(close, wi)) {
			if (dir < 0 && enter_leave) {
				doc_prev(f, m);
				rpt += 1;
			} else
				/* hit a close */
				break;
		} else if (strchr(open, wi)) {
			/* skip bracketed expression */
			int depth = 1;
			wint_t q = 0;

			doc_move(f, m, dir);
			if (enter_leave && dir > 0)
				/* Just entered the expression */
				rpt -= 1;
			else while (depth > 0 &&
				    (wi = doc_move(f, m, dir)) != WEOF) {
					if (q) {
						if (dir > 0)
							doc_prev(f,m);
						if ((!check_slosh(f, m) && wi == q) ||
						    is_eol(wi))
							q = 0;
						if (dir > 0)
							doc_next(f,m);
					} else if (strchr(open, wi))
						depth += 1;
					else if (strchr(close, wi))
						depth -= 1;
					else if (wi == '"' || wi == '\'') {
						if (dir > 0)
							doc_prev(f,m);
						if (!check_slosh(f, m))
							q = wi;
						if (dir > 0)
							doc_next(f,m);
					}
				}
		} else if (wi == '"' || wi == '\'') {
			/* skip quoted or to EOL */
			wint_t q = wi;
			bool slosh = False;
			if (dir > 0) {
				slosh = check_slosh(f, m);
				doc_move(f, m, dir);
			} else {
				doc_move(f, m, dir);
				slosh = check_slosh(f, m);
			}
			if (!slosh) {
				while (((wi = doc_pending(f, m, dir))
					!= WEOF) &&
				       !is_eol(wi)) {
					if (dir > 0) {
						slosh = check_slosh(f, m);
						doc_next(f, m);
					} else {
						doc_prev(f, m);
						slosh = check_slosh(f, m);
					}
					if (wi == q && !slosh)
						break;
				}
			}
		} else while (((wi=doc_pending(f, m, dir)) != WEOF && iswalnum(wi)) ||
			      (wi > 0 && wi <= 255 &&
			       strchr(wordchars, wi) != NULL))
				doc_move(f, m, dir);

		if (!enter_leave)
			rpt -= dir;
		if (wi == WEOF)
			break;
	}
	return rpt ? 2 : 1;
}

DEF_CMD(doc_WORD)
{
	/* Step to the Nth word boundary in appropriate
	 * direction.  For this function, puctuation is treated the
	 * same as alphanum.  Only space separates words.
	 * If N is 0, don't move.
	 * Return 1 if succeeded before EOF, else Efalse.
	 */
	struct pane *f = ci->focus;
	struct mark *m = ci->mark;
	int rpt = RPT_NUM(ci);

	if (!m)
		return Enoarg;

	/* We skip spaces, then non-spaces */
	while (rpt > 0) {
		wint_t wi;

		while ((wi = doc_following(f, m)) != WEOF &&
		       iswspace(wi))
			doc_next(f,m);

		while ((wi = doc_following(f, m)) != WEOF &&
		       !iswspace(wi))
			doc_next(f,m);
		rpt -= 1;
	}
	while (rpt < 0) {
		wint_t wi;

		while ((wi = doc_prior(f, m)) != WEOF &&
		       iswspace(wi))
			doc_prev(f,m);
		while ((wi = doc_prior(f, m)) != WEOF &&
		       !iswspace(wi))
			doc_prev(f,m);
		rpt += 1;
	}

	return rpt == 0 ? 1 : Efalse;
}

DEF_CMD(doc_eol)
{
	struct pane *f = ci->focus;
	struct mark *m = ci->mark;
	wint_t ch = 1;
	int rpt = RPT_NUM(ci);
	bool one_more = ci->num2 > 0;

	if (!m)
		return Enoarg;

	while (rpt > 0 && ch != WEOF) {
		while ((ch = doc_next(f, m)) != WEOF &&
		       !is_eol(ch))
			;
		if (ch != WEOF)
			rpt -= 1;
	}
	while (rpt < 0 && ch != WEOF) {
		while ((ch = doc_prev(f, m)) != WEOF &&
		       !is_eol(ch))
			;
		if (ch != WEOF)
			rpt += 1;
	}
	if (!one_more) {
		if (is_eol(ch)) {
			if (RPT_NUM(ci) > 0)
				doc_prev(f, m);
			else if (RPT_NUM(ci) < 0)
				doc_next(f, m);
		}
		if (ch == WEOF) {
			if (RPT_NUM(ci) > 0)
				rpt -= 1;
			else if (RPT_NUM(ci) < 0)
				rpt += 1;
		}
	}
	return rpt == 0 ? 1 : Efalse;
}

DEF_CMD(doc_file)
{
	int rpt = RPT_NUM(ci);
	struct mark *m = ci->mark;

	if (!m)
		return Enoarg;

	call("doc:set-ref", ci->focus, (rpt < 0), m);

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
		while ((ch = doc_next(p, m)) != WEOF &&
		       !is_eol(ch))
			;
		rpt -= 1;
	}
	while (rpt < 0 && ch != WEOF) {
		while ((ch = doc_prev(p, m)) != WEOF &&
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
	struct mark *m = ci->mark;
	int rpt = RPT_NUM(ci);
	wint_t ch = 0;
	int nlcnt = 0;
	int dir = rpt > 0 ? 1 : -1;

	if (!m)
		return Enoarg;

	while (dir < 0 && is_eol(doc_prior(p, m)))
		doc_prev(p, m);

	while (rpt && ch != WEOF) {
		nlcnt = 0;
		while (ch != WEOF) {
			ch = doc_move(p, m, dir);
			if (is_eol(ch))
				nlcnt += 1;
			else if (nlcnt < 2)
				nlcnt = 0;
			else {
				doc_move(p, m, -dir);
				break;
			}
		}
		rpt += dir;
	}

	while (dir < 0 && nlcnt-- > 0)
		doc_next(p, m);
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
	/* repeat count is in 1000th of the pane */
	rpt /= 1000;
	while (rpt > 0 && ch != WEOF) {
		while ((ch = doc_next(p, m)) != WEOF &&
		       !is_eol(ch))
			;
		rpt -= 1;
	}
	while (rpt < 0 && ch != WEOF) {
		while ((ch = doc_prev(p, m)) != WEOF &&
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
	struct doc *d = &ci->home->doc;
	const char *val = ksuffix(ci, "doc:set:");

	if (!*val)
		val = ci->str2;
	if (!val)
		return Enoarg;

	if (strcmp(val, "autoclose") == 0) {
		d->autoclose = ci->num;
		return 1;
	}
	if (strcmp(val, "readonly") == 0) {
		d->readonly = ci->num;
		call("doc:notify:doc:status-changed", ci->home);
		return 1;
	}
	if (ci->str)
		attr_set_str(&ci->home->attrs, val, ci->str);

	return 1;
}

DEF_CMD(doc_append)
{
	struct pane *p = ci->home;
	const char *attr = ksuffix(ci, "doc:append:");
	const char *val = ci->str;
	const char *old;

	if (!*attr)
		attr = ci->str2;
	if (!attr)
		return Enoarg;

	if (!val || !val[0])
		return Enoarg;
	/* Append the string to the attr.  It attr doesn't
	 * exists, strip first char of val and use that.
	 */
	old = attr_find(p->attrs, attr);
	if (!old) {
		attr_set_str(&p->attrs, attr, val+1);
	} else {
		const char *pos = strstr(old, val+1);
		int len = strlen(val+1);
		if (pos &&
		    (pos == old || pos[-1] == val[0]) &&
		    (pos[len] == 0 || pos[len] == val[0]))
			; /* val already present */
		else
			attr_set_str(&p->attrs, attr, strconcat(p, old, val));
	}
	return 1;
}

DEF_CMD(doc_get_attr)
{
	struct doc *d = &ci->home->doc;
	char pathbuf[PATH_MAX];
	char *a;

	if (!ci->str)
		return Enoarg;

	if ((a = attr_find(ci->home->attrs, ci->str)) != NULL)
		;
	else if (strcmp(ci->str, "doc-name") == 0)
		a = d->name;
	else if (strcmp(ci->str, "doc-modified") == 0)
		a = "no";
	else if (strcmp(ci->str, "doc-readonly") == 0) {
		a = d->readonly ? "yes":"no";
	} else if (strcmp(ci->str, "dirname") == 0) {
		char *sl;
		a = pane_attr_get(ci->home, "filename");
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
		attr_set_str(&ci->home->attrs, "dirname", a);
	} else if (strcmp(ci->str, "realdir") == 0) {
		a = pane_attr_get(ci->home, "dirname");
		if (a) {
			strcpy(pathbuf,"/");
			a = realpath(a, pathbuf);
			if (a && a != pathbuf)
				strcpy(pathbuf, a);
			if (pathbuf[1])
				strcat(pathbuf, "/");
			a = pathbuf;
		}
		attr_set_str(&ci->home->attrs, "realdir", a);
	}
	if (a)
		return comm_call(ci->comm2, "callback:get_attr", ci->focus, 0,
				 NULL, a) ?: 1;
	return 1;
}

DEF_CMD(doc_doc_get_attr)
{
	/* If the document doesn't provide the attribute for
	 * this location, see if there is a pane-attribute for
	 * the document.
	 */
	char *a;

	if (!ci->str)
		return Enoarg;
	a = pane_attr_get(ci->home, ci->str);
	if (a)
		comm_call(ci->comm2, "cb", ci->focus, 0, NULL, a);
	return 1;
}

DEF_CMD(doc_set_name)
{
	struct doc *d = &ci->home->doc;

	if (!ci->str)
		return Enoarg;
	free(d->name);
	d->name = strdup(ci->str);
	return call("doc:notify:doc:revisit", ci->home, ci->num) ?: 1;
}

DEF_CMD(doc_request_notify)
{
	pane_add_notify(ci->focus, ci->home, ksuffix(ci, "doc:request:"));
	return 1;
}

DEF_CMD_CLOSED(doc_notify)
{
	/* Key is "doc:notify:..." */
	int ret = pane_notify(ksuffix(ci, "doc:notify:"),
			      ci->home,
			      ci->num, ci->mark, ci->str,
			      ci->num2, ci->mark2, ci->str2, ci->comm2);
	return ret;
}

static int do_del_view(struct doc *d safe, int v,
		       struct pane *owner)
{
	bool warned = False;
	/* This view should only have points on the list, not typed
	 * marks.  Just delete everything and clear the 'notify' pointer
	 */
	if (v < 0 || v >= d->nviews || d->views == NULL ||
	    !owner || d->views[v].owner != owner)
		return Einval;

	d->views[v].owner = NULL;
	while (!tlist_empty(&d->views[v].head)) {
		struct mark *m;
		struct tlist_head *tl = d->views[v].head.next;

		switch (TLIST_TYPE(tl)) {
		case GRP_LIST: /* A point */
			tlist_del_init(tl);
			break;
		case GRP_MARK: /* a vmark */
			m = container_of(tl, struct mark, view);
			if (m->mdata)
				pane_call(owner, "Close:mark", owner, 0, m);
			if (tl == d->views[v].head.next) {
				/* It hasn't been freed */
				if (m->mdata && !warned) {
					call("editor:notify:Message:broadcast",
					     owner, 0, NULL,
					     "WARNING mark not freed by Close:mark");
					LOG("WARNING Mark on %s not freed by Close:mark",
					    owner->name);
					warned = True;
				}
				m->mdata = NULL;
				mark_free(m);
			}
			break;
		default: /* impossible */
			abort();
		}
	}
	return 1;
}

DEF_CMD(doc_delview)
{
	struct doc *d = &ci->home->doc;

	return do_del_view(d, ci->num, ci->focus);
}

DEF_CMD(doc_addview)
{
	struct doc *d = &ci->home->doc;
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
		pane_add_notify(ci->home, ci->focus, "Notify:Close");
	}
	return 1 + ret;
}

DEF_CMD_CLOSED(doc_close_doc)
{
	struct doc *d = &ci->home->doc;
	doc_free(d, ci->home);
	return 1;
}

DEF_CMD_CLOSED(doc_view_close)
{
	/* A pane which once held a view is closing.  We must discard
	 * that view if it still exists.
	 */
	struct doc *d = &ci->home->doc;
	int v;

	for (v = 0 ; d->views && v < d->nviews; v++)
		do_del_view(d, v, ci->focus);
	return 1;
}

DEF_CMD(doc_vmarkget)
{
	struct mark *m, *m2;
	m = do_vmark_first(&ci->home->doc, ci->num, ci->focus);
	m2 = do_vmark_last(&ci->home->doc, ci->num, ci->focus);
	return comm_call(ci->comm2, "callback:vmark", ci->focus,
			 0, m, NULL, 0, m2) ?: 1;
}

DEF_CMD(doc_vmarkprev)
{
	struct mark *m = NULL;
	if (ci->mark)
		m = do_vmark_at_or_before(&ci->home->doc, ci->mark,
					   ci->num, ci->focus);
	comm_call(ci->comm2, "callback:vmark", ci->focus, 0, m);
	return 1;
}

DEF_CMD(doc_vmarknew)
{
	struct mark *m;

	m = doc_new_mark(ci->home, ci->num, ci->focus);
	comm_call(ci->comm2, "callback:vmark", ci->focus, 0, m);
	return 1;
}

DEF_CMD(doc_drop_cache)
{
	struct pane *p = ci->home;
	struct doc *d = &p->doc;

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

	call_comm("event:on-idle", p, &doc_delayed_close, 1);
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
	 * This is used for 'search' and 'copy'.
	 * This default version calls doc:char which is simple, but might
	 * be slow.
	 *
	 * If called as doc:content-bytes: return bytes, not chars
	 *
	 * .mark is 'location': to start.  This is not moved.
	 * .mark2, if set, is location to stop.
	 * .comm2 is 'consume': pass char mark and report if finished.
	 *
	 * comm2 is passed:
	 * .mark - the mark that was passed in and gets moved
	 * .num - char character just before .mark
	 * .str - num utf8 text after mark.  It may not be present
	 *       and if it is, at most .num2 bytes can be used
	 * .num2 - usable length of .str
	 *
	 * comm2 it typically embedded in another struct that can
	 * be accessed in the callback (using container_of in C code).
	 * If the caller need to know where the callback aborted, the
	 * callback need to record that somehow.
	 *
	 * comm2 should return 1 if the main char was consumed,
	 * 1+n if n bytes (not chars) from str were consumed
	 * -ve to abort.
	 *
	 * If the callback processes some of 'str', the mark will no longer
	 * be accurate.  If it needs an accurate mark, it can walk a copy
	 * forward, or return a suitable count and be called again with an
	 * accurate mark.
	 */
	struct mark *m = ci->mark;
	int nxt;
	char *cmd = "doc:char";

	if (!m || !ci->comm2)
		return Enoarg;
	m = mark_dup(m);
	if (strcmp(ci->key, "doc:content-bytes") == 0)
		cmd = "doc:byte";

	nxt = call(cmd, ci->home, 1, m);
	while (nxt > 0 && nxt != CHAR_RET(WEOF) &&
	       (!ci->mark2 || mark_ordered_or_same(m, ci->mark2)) &&
	       comm_call(ci->comm2, "consume", ci->home, (nxt & 0x1FFFF), m) > 0)
		nxt = call(cmd, ci->home, 1, m);

	mark_free(m);
	return nxt < 0 ? nxt : 1;
}

DEF_CMD(doc_insert_char)
{
	const char *str = ksuffix(ci, "doc:char-");

	return call("doc:replace", ci->focus, 1, NULL, str, ci->num2,
		    ci->mark);
}

struct getstr {
	struct buf b;
	struct mark *end;
	struct command c;
	int bytes;
};

DEF_CB(get_str_callback)
{
	/* First char will be in ci->num and ->mark will be *after* that char.
	 * Some more chars might be in ->str (for ->num2 bytes).
	 * If ->x, then expect that many bytes (approximately).
	 * Return Efalse to stop, 1 if char was consumed,
	 * 1+N (N <= ->num2) if N bytes from ->str were consumed.
	 */
	wint_t wch = ci->num & 0x1FFFFF;
	struct getstr *g = container_of(ci->comm, struct getstr, c);

	if (!ci->mark)
		return Enoarg;
	if (ci->x)
		buf_resize(&g->b, ci->x);
	if (g->bytes)
		buf_append_byte(&g->b, ci->num & 0xff);
	else
		buf_append(&g->b, wch);
	if (g->end && (ci->mark->seq >= g->end->seq ||
		       mark_same(ci->mark, g->end)))
		return Efalse;
	if (!ci->str || ci->num2 <= 0)
		return 1;

	/* This could over-run ->end, but we assume it doesn't */
	buf_concat_len(&g->b, ci->str, ci->num2);
	return 1 + ci->num2;
}

DEF_CMD(doc_get_str)
{
	/* doc:get-str
	 * uses doc:content to collect the content
	 * into a buf.
	 * If mark and mark2 are both set, they are end points.
	 * If only mark is set we ignore it.  It is likely
	 * 'point' provided by default.
	 */
	int bytes = strcmp(ci->key, "doc:get-bytes") == 0;
	struct getstr g;
	struct mark *from = NULL, *to = NULL;

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
	g.bytes = bytes;
	buf_init(&g.b);
	g.end = to;
	if (!from) {
		from = mark_new(ci->focus);
		if (from)
			call("doc:set-ref", ci->focus, 1, from);
	}
	if (!from)
		return Efail;
	if (!to) {
		to = mark_new(ci->focus);
		if (to)
			call("doc:set-ref", ci->focus, 0, to);
	}
	call_comm(bytes ? "doc:content-bytes" : "doc:content",
		  ci->focus, &g.c, 0, from, NULL, 0, to);
	if (from != ci->mark && from != ci->mark2)
		mark_free(from);
	if (to != ci->mark && to != ci->mark2)
		mark_free(to);
	comm_call(ci->comm2, "callback:get-str", ci->focus, g.b.len, NULL,
		  buf_final(&g.b));
	free(g.b.b);
	return 1;
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

DEF_CMD(doc_notify_moving)
{
	struct doc_data *dd = ci->home->data;

	if (ci->mark == dd->point)
		pane_damaged(ci->home, DAMAGED_VIEW);
	return Efallthrough;
}

DEF_CMD(doc_refresh_view)
{
	struct doc_data *dd = ci->home->data;
	int active = attr_find_int(dd->point->attrs, "selection:active");

	if (active > 0) {
		call("view:changed", ci->focus, 0, dd->point, NULL,
		     0, dd->old_point);
	} else {
		call("view:changed", ci->focus, 0, dd->point);
		if (dd->old_point)
			call("view:changed", ci->focus, 0, dd->old_point);
	}
	if (!dd->old_point)
		dd->old_point = mark_dup(dd->point);
	else
		mark_to_mark(dd->old_point, dd->point);
	mark_watch(dd->point);
	return 1;
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
	struct pane *p = doc_attach_assign(ci->focus, dd->doc);

	if (!p)
		return Efail;
	call("Move-to", p, 0, dd->point);
	pane_clone_children(ci->home, p);
	return 1;
}

DEF_CMD_CLOSED(doc_close)
{
	struct doc_data *dd = ci->home->data;
	int i;
	call("doc:push-point", dd->doc, 0, dd->point);
	mark_free(dd->point);
	mark_free(dd->old_point);
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
	return call("doc:replace", ci->focus,
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
	return comm_call(ci->comm2, "callback", ci->focus, 0, NULL, a) ?: 1;
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
		mark_step(dd->marks[mnum], 0);
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

	mark_clip(dd->point, ci->mark, ci->mark2, !!ci->num);
	if (dd->old_point)
		mark_clip(dd->old_point, ci->mark, ci->mark2, !!ci->num);
	for (mnum = 0; mnum < 4; mnum++)
		if (dd->marks[mnum])
			mark_clip(dd->marks[mnum], ci->mark, ci->mark2, !!ci->num);
	return 1;
}

DEF_CMD_CLOSED(doc_pass_on)
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
	struct doc *d = &ci->home->doc;
	int n = ARRAY_SIZE(d->recent_points);
	struct mark *m;
	if (!ci->mark)
		return Enoarg;
	mark_free(d->recent_points[n-1]);
	memmove(&d->recent_points[1],
		&d->recent_points[0],
		(n-1)*sizeof(d->recent_points[0]));
	m = mark_dup(ci->mark);
	m->attrs = attr_copy(ci->mark->attrs);
	d->recent_points[0] = m;
	return 1;
}

DEF_CMD(doc_pop_point)
{
	struct doc *d = &ci->home->doc;
	int n = ARRAY_SIZE(d->recent_points);

	if (!ci->mark)
		return Enoarg;
	if (!d->recent_points[0])
		return Efail;
	mark_to_mark(ci->mark, d->recent_points[0]);
	if (!ci->mark->attrs) {
		ci->mark->attrs = d->recent_points[0]->attrs;
		d->recent_points[0]->attrs = NULL;
	}
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

	if (strcmp(type, "invisible") != 0) {
		if (doc == focus)
			/* caller is confused. */
			return Einval;
		/* Double check the focus can display things */
		if (call("Draw:text", focus) == Efallthrough)
			return Einval;
	}

	p = doc_attach_assign(focus, doc);
	if (!p)
		return Efail;

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

DEF_CMD(doc_get_doc)
{
	if (!ci->comm2)
		return Enoarg;

	comm_call(ci->comm2, "attach", ci->home, ci->num, NULL, ci->str,
		  ci->num2, NULL, ci->str2);
	return 1;
}

DEF_CMD(doc_abort)
{
	struct doc_data *dd = ci->home->data;

	call("doc:notify:Abort", dd->doc);
	return Efallthrough;
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
	key_add(doc_handle_cmd, "Move-Line", &doc_line);
	key_add(doc_handle_cmd, "Move-View", &doc_page);
	key_add(doc_handle_cmd, "doc:point", &doc_get_point);

	key_add(doc_handle_cmd, "doc:notify-viewers", &doc_notify_viewers);
	key_add(doc_handle_cmd,	"Notify:Close", &doc_notify_close);
	key_add(doc_handle_cmd,	"mark:moving", &doc_notify_moving);
	key_add(doc_handle_cmd,	"Refresh:view", &doc_refresh_view);
	key_add(doc_handle_cmd,	"Clone", &doc_clone);
	key_add(doc_handle_cmd,	"Close", &doc_close);
	key_add(doc_handle_cmd, "doc:dup-point", &doc_dup_point);
	key_add(doc_handle_cmd, "Replace", &doc_replace);
	key_add(doc_handle_cmd, "get-attr", &doc_handle_get_attr);
	key_add(doc_handle_cmd, "Move-to", &doc_move_to);
	key_add(doc_handle_cmd, "Notify:clip", &doc_clip);
	key_add(doc_handle_cmd, "Abort", &doc_abort);

	key_add(doc_default_cmd, "doc:add-view", &doc_addview);
	key_add(doc_default_cmd, "doc:del-view", &doc_delview);
	key_add(doc_default_cmd, "Notify:Close", &doc_view_close);
	key_add(doc_default_cmd, "doc:vmark-get", &doc_vmarkget);
	key_add(doc_default_cmd, "doc:vmark-prev", &doc_vmarkprev);
	key_add(doc_default_cmd, "doc:vmark-new", &doc_vmarknew);
	key_add(doc_default_cmd, "get-attr", &doc_get_attr);
	key_add(doc_default_cmd, "doc:get-attr", &doc_doc_get_attr);
	key_add(doc_default_cmd, "doc:set-name", &doc_set_name);
	key_add(doc_default_cmd, "doc:destroy", &doc_do_destroy);
	key_add(doc_default_cmd, "doc:drop-cache", &doc_drop_cache);
	key_add(doc_default_cmd, "doc:closed", &doc_do_closed);
	key_add(doc_default_cmd, "doc:get-str", &doc_get_str);
	key_add(doc_default_cmd, "doc:get-bytes", &doc_get_str);
	key_add(doc_default_cmd, "doc:content", &doc_default_content);
	key_add(doc_default_cmd, "doc:content-bytes", &doc_default_content);
	key_add(doc_default_cmd, "doc:push-point", &doc_push_point);
	key_add(doc_default_cmd, "doc:pop-point", &doc_pop_point);
	key_add(doc_default_cmd, "doc:attach-view", &doc_attach_view);
	key_add(doc_default_cmd, "doc:get-doc", &doc_get_doc);
	key_add(doc_default_cmd, "Close", &doc_close_doc);

	key_add(doc_default_cmd, "doc:word", &doc_word);
	key_add(doc_default_cmd, "doc:WORD", &doc_WORD);
	key_add(doc_default_cmd, "doc:EOL", &doc_eol);
	key_add(doc_default_cmd, "doc:file", &doc_file);
	key_add(doc_default_cmd, "doc:expr", &doc_expr);
	key_add(doc_default_cmd, "doc:paragraph", &doc_para);

	key_add_prefix(doc_default_cmd, "doc:char-", &doc_insert_char);
	key_add_prefix(doc_default_cmd, "doc:request:",
		       &doc_request_notify);
	key_add_prefix(doc_default_cmd, "doc:notify:", &doc_notify);
	key_add_prefix(doc_default_cmd, "doc:set:", &doc_set);
	key_add_prefix(doc_default_cmd, "doc:append:", &doc_append);
}

static struct pane *doc_attach_assign(struct pane *parent safe, struct pane *doc safe)
{
	struct pane *p;
	struct doc_data *dd;
	struct mark *m;

	p = pane_register(parent, 0, &doc_handle.c);
	if (!p)
		return NULL;
	dd = p->data;
	pane_damaged(p, DAMAGED_VIEW);

	m = point_new(doc);
	if (!m) {
		pane_close(p);
		return NULL;
	}
	if (call("doc:pop-point", doc, 0, m) <= 0)
		pane_notify("doc:notify-viewers", doc, 0, m);
	dd->doc = doc;
	dd->point = m;
	attr_set_str(&m->attrs, "render:interactive-point", "yes");

	pane_add_notify(p, doc, "Notify:Close");
	pane_add_notify(p, doc, "doc:notify-viewers");
	pane_add_notify(p, doc, "mark:moving");
	call("doc:notify:doc:revisit", doc, 0);
	mark_watch(m);
	return p;
}

static void simplify_path(const char *path safe, char *buf safe)
{
	/* Like readpath, but doesn't process symlinks,
	 * so only "..", "." and extra '/' are handled
	 * Assumes that 'path' starts with a '/'.
	 */
	const char *p;
	const char *end;
	char *b = buf;

	for (p = path; *p; p = end) {
		int len;
		end = strchrnul(p+1, '/');
		len = end - p;

		if (len == 1)
			/* Extra '/' at end or in the middle, ignore */
			continue;
		if (len == 2 && strstarts(p, "/.") )
			/* Ignore the dot */
			continue;
		if (len == 3 && strstarts(p, "/..")) {
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
	char *name;
	char *realname = NULL;
	struct stat stb;
	struct pane *p;
	int autoclose = ci->num2 & 1;
	int create_ok = ci->num2 & 4;
	int reload = ci->num2 & 8;
	int force_reload = ci->num2 & 16;
	int quiet = ci->num2 & 32;
	int only_existing = ci->num2 & 64;
	char pathbuf[PATH_MAX];

	if (!ci->str)
		return Enoarg;

	stb.st_mode = 0;
	/* fd < -1 mean a non-filesystem name */
	if (fd >= -1) {
		char *sl = NULL, *rp, *restore = NULL;
		char *dir;
		/* First, make sure we have an absolute path, and
		 * simplify it.
		 */
		if (ci->str[0] != '/') {
			char *c = getcwd(pathbuf, sizeof(pathbuf));
			if (c) {
				name = strconcat(ed, c, "/", ci->str);
				simplify_path(name, pathbuf);
				name = strsave(ed, pathbuf);
			} else
				name = strsave(ed, ci->str);
		} else {
			simplify_path(ci->str, pathbuf);
			name = strsave(ed, pathbuf);
		}
		if (!name)
			return Efail;
		/* Now try to canonicalize directory part of name as realname */
		dir = name;
		if (dir)
			sl = strrchr(dir, '/');
		if (sl && sl[1] && strcmp(sl, "/.") != 0 && strcmp(sl, "/..") != 0) {
			/* Found a real basename */
			restore = sl;
			*sl++ = '\0';
		} else if (sl) {
			/* Cannot preserve basename in relative path */
			sl = "";
		} else {
			sl = name;
			dir = ".";
		}
		rp = realpath(dir, pathbuf);
		if (rp && sl)
			realname = strconcat(ed, rp, "/", sl);
		else if (rp)
			realname = rp;
		else
			realname = NULL;
		if (restore)
			*restore = '/';
	} else
		name = (char*)ci->str;

	if (fd == -1)
		/* No open yet */
		fd = open(name, O_RDONLY);

	if (fd >= 0)
		fstat(fd, &stb);
	else if (create_ok)
		stb.st_mode = S_IFREG;
	else
		stb.st_mode = 0;

	p = call_ret(pane, "docs:byfd", ed, 0, NULL, name, fd);

	if (!p) {
		if (only_existing) {
			if (fd != ci->num)
				close(fd);
			return Efalse;
		}
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
		if (n && strlen(n) > 1 && n[strlen(n)-1] == '/') {
			/* Make sure both end in '/' if either do */
			if (realname)
				realname = strconcat(ed, realname, "/");
			name = strconcat(ed, name, "/");
		}
		if (!quiet && n && realname && strcmp(n, realname) != 0)
			call("Message", ci->focus, 0, NULL,
			     strconcat(ci->focus, "File ", realname, " and ", n,
				       " are the same"));
		else if (!quiet && n && strcmp(n, name) != 0)
			call("Message", ci->focus, 0, NULL,
			     strconcat(ci->focus, "File ", name, " and ", n,
				       " are the same"));

		if (reload || force_reload)
			call("doc:load-file", p, force_reload?0:1, NULL, name,
			     fd);
	}
	if (fd != ci->num)
		close(fd);
	return comm_call(ci->comm2, "callback", p) ?: 1;
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
	return comm_call(ci->comm2, "callback", p) ?: 1;
}

void doc_free(struct doc *d safe, struct pane *root safe)
{
	/* NOTE: this must be idempotent as both it can be called
	 * twice, once from doc_default_cmd, once deliberately by the
	 * pane.
	 */
	unsigned int i;
	bool warned = False;

	for (i = 0; i < ARRAY_SIZE(d->recent_points); i++) {
		mark_free(d->recent_points[i]);
		d->recent_points[i] = NULL;
	}
	for (i = 0; i < (unsigned int)d->nviews; i++)
		if (d->views)
			do_del_view(d, i, d->views[i].owner);
	unalloc_buf(d->views, sizeof(d->views[0]) * d->nviews, pane);
	free(d->name);
	d->name = NULL;
	while (!hlist_empty(&d->marks)) {
		struct mark *m = hlist_first_entry(&d->marks, struct mark, all);
		if (m->viewnum == MARK_UNGROUPED && m->mdata) {
			/* we cannot free this, so warn and discard */
			if (!warned) {
				call("editor:notify:Message:broadcast",
				     root, 0, NULL,
				     "WARNING mark with data not freed");
				LOG("WARNING Mark with data no freed");
			}
			warned = True;
			m->mdata = NULL;
		}
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
