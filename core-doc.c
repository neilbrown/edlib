/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
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

static struct pane *do_doc_assign(struct pane *p safe, struct pane *doc safe, int, char *);
static struct pane *doc_attach(struct pane *parent);

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
	struct mark		*mark;
};

static int do_doc_add_view(struct doc *d safe)
{
	struct docview *g;
	int ret;
	int i;

	for (ret = 0; d->views && ret < d->nviews; ret++)
		if (d->views[ret].state == 0)
			break;
	if (!d->views || ret == d->nviews) {
		/* Resize the view list */
		d->nviews += 4;
		g = malloc(sizeof(*g) * d->nviews);
		for (i = 0; d->views && i < ret; i++) {
			tlist_add(&g[i].head, GRP_HEAD, &d->views[i].head);
			tlist_del(&d->views[i].head);
			g[i].state = d->views[i].state;
		}
		for (; i < d->nviews; i++) {
			INIT_TLIST_HEAD(&g[i].head, GRP_HEAD);
			g[i].state = 0;
		}
		free(d->views);
		d->views = g;
		/* now resize all the points */
		points_resize(d);
	}
	if (d->views /* FIXME always true */) {
		points_attach(d, ret);
		d->views[ret].state = 1;
	}
	return ret;
}

static void do_doc_del_view(struct doc *d safe, int i)
{
	/* This view should only have points on the list, not typed
	 * marks.  Just delete everything and clear the 'notify' pointer
	 */
	if (i < 0 || i >= d->nviews || d->views == NULL)
		return;
	d->views[i].state = 0;
	while (!tlist_empty(&d->views[i].head)) {
		struct tlist_head *tl = d->views[i].head.next;
		if (TLIST_TYPE(tl) != GRP_LIST)
			abort();
		tlist_del_init(tl);
	}
}

void doc_init(struct doc *d safe)
{
	INIT_HLIST_HEAD(&d->marks);
	INIT_TLIST_HEAD(&d->points, 0);
	d->views = NULL;
	d->nviews = 0;
	d->name = NULL;
	memset(d->recent_points, 0, sizeof(d->recent_points));
	d->autoclose = 0;
	d->filter = 0;
	d->home = safe_cast NULL;
}

static void parse_sub_pos(char *attr, int *home safe, int *max safe)
{
	char *c;
	if (!attr) {
		*home = 0;
		*max = 1;
		return;
	}
	c = strchr(attr, ':');
	if (c) {
		*home = atoi(attr);
		*max = atoi(c+1);
	} else {
		*home = 0;
		*max = atoi(attr);
	}
	if (*max <= 0)
		*max = 1;
	if (*home < 0 || *home > *max)
		*home = 0;
}

static wint_t doc_move_horiz(struct pane *p safe, struct mark *m safe,
			     int forward,
			     int *field)
{
	/* If subfields are present, move one step, else move one
	 * char.  We we move a char and land in subfields, make
	 * sure rpos is correct.
	 */
	wint_t ret = '\n';

	if (field)
		*field = 0;
	if (m->rpos == NEVER_RPOS)
		return mark_step_pane(p, m, forward, 1);
	if (m->rpos != NO_RPOS) {
		char *a = pane_mark_attr(p, m, "renderline:fields");
		int home, cnt;
		parse_sub_pos(a, &home, &cnt);
		if (forward && m->rpos+1 < cnt)
			m->rpos += 1;
		else if (!forward && m->rpos > 0)
			m->rpos -= 1;
		else
			m->rpos = NO_RPOS;
	}
	if (m->rpos == NO_RPOS) {
		ret = mark_step_pane(p, m, forward, 1);
		if (is_eol(forward ? mark_step_pane(p, m, forward, 0) : ret)) {
			char *a = pane_mark_attr(p, m, "renderline:fields");
			int home, cnt;
			parse_sub_pos(a, &home, &cnt);
			if (a) {
				ret = '\n';
				if (field)
					*field = 1;
				if (forward)
					m->rpos = 0;
				else
					m->rpos = cnt-1;
			}
		}
	} else if (field)
		*field = 1;
	return ret;
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
		if (doc_move_horiz(f, m, 1, NULL) == WEOF)
			break;
		rpt -= 1;
	}
	while (rpt < 0) {
		if (doc_move_horiz(f, m, 0, NULL) == WEOF)
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
	/* Move-word should finish at a word boundary, which usually means
	 * an alphanum (possibly including '_' depending on doc attributes?).
	 * However it should never cross two different sets of spaces or
	 * punctuation.  So if we cross space and punct and don't find alphanum,
	 * then we treat end of punct as a word boundary.  We never stop immediately
	 * after a space.
	 * So skip spaces, then punct, then alphanum.
	 * Same in either direction.
	 */
	dir = rpt > 0 ? 1 : 0;
	while (rpt != 0) {
		int field = 0;
		wint_t wi;

		while (!field &&
		       iswspace(mark_step_pane(f, m, dir, 0)))
			doc_move_horiz(f, m, dir, &field);

		while (!field &&
		       (wi=mark_step_pane(f, m, dir, 0)) != WEOF &&
		       !iswspace(wi) && !iswalnum(wi))
			doc_move_horiz(f, m, dir, &field);

		if (m->rpos < NO_RPOS || iswalnum(mark_step_pane(f, m, dir, 0))) {
			while (!field && iswalnum(mark_step_pane(f, m, dir, 0)))
				doc_move_horiz(f, m, dir, &field);
		}

		rpt -= dir * 2 - 1;
	}

	return 1;
}

DEF_CMD(doc_expr)
{
	/* doc_expr skips an 'expression' which is the same as a word
	 * unless we see open '({[' or close ')}]' or quote ('").
	 * If we see close going forward, or open going backward, we stop.
	 * If we see open going forward or close going backward, or quote,
	 * we skip to matching close/open/quote, allowing for nested
	 * open/close etc. Inside quotes, we stop at EOL.
	 * If num2 is 1, then if we reach a true 'open' we continue
	 * one more character to enter (going forward) or leave (backward)
	 * the expression.
	 */
	struct pane *f = ci->focus;
	struct doc_data *dd = ci->home->data;
	struct mark *m = ci->mark;
	int rpt = RPT_NUM(ci);
	int enter_leave = ci->num2;
	int dir;
	char *open;
	char *close;
	const char *special = "[](){}'\"";

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
		       (wi > 255 || strchr(special, wi) == NULL))
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
			else while (depth > 0 && (wi = mark_step_pane(f, m, dir, 1)) != WEOF) {
				if (q) {
					if (wi == q || is_eol(wi))
						q = 0;
				} else if (strchr(open, wi))
					depth += 1;
				else if (strchr(close, wi))
					depth -= 1;
				else if (wi == '"' || wi == '\'')
					q = wi;
			}
		} else if (wi == '"' || wi == '\'') {
			/* skip quoted or to EOL */
			wint_t q = wi;
			mark_step_pane(f, m, dir, 1);
			while ((wi = mark_step_pane(f, m, dir, 0)) != WEOF &&
			       !is_eol(wi) && wi != q)
				mark_step_pane(f, m, dir, 1);
			if (wi == q)
				mark_step_pane(f, m, dir, 1);
		} else while (iswalnum(mark_step_pane(f, m, dir, 0)))
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
		int field = 0;
		while (!field &&
		       iswspace(doc_following_pane(f, m)))
			doc_move_horiz(f, m, 1, &field);

		while (!field &&
		       (wi=doc_following_pane(f, m)) != WEOF &&
		       !iswspace(wi))
			doc_move_horiz(f, m, 1, &field);
		rpt -= 1;
	}
	while (rpt < 0) {
		wint_t wi;
		int  field = 0;
		while (!field &&
		       iswspace(doc_prior_pane(f, m)))
			doc_move_horiz(f, m, 0, &field);
		while (!field &&
		       (wi=doc_prior_pane(f, m)) != WEOF &&
		       !iswspace(wi))
			doc_move_horiz(f, m, 0, &field);
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

DEF_CMD(doc_attr_set)
{
	struct doc *d = ci->home->data;

	/* if a non-filter doesn't support attr_set, don't let it fall through */
	return d->filter ? 0 : 1;
}

DEF_CMD(doc_set)
{
	struct doc *d = ci->home->data;
	char *val = ci->key + 8;

	if (strcmp(val, "autoclose") == 0) {
		d->autoclose = ci->num;
		return 1;
	}
	if (strcmp(val, "filter") == 0) {
		d->filter = ci->num;
		return 1;
	}
	if (ci->str)
		attr_set_str(&d->home->attrs, val, ci->str);

	return d->filter ? 0 : 1;
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
	else if (strcmp(ci->str, "dirname") == 0) {
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
	/* Once a get-attr request reaches a document, it needs to stop there,
	 * as parents might have a different idea about attributes, and about marks
	 */
	return 1;
}

DEF_CMD(doc_set_name)
{
	struct doc *d = ci->home->data;

	if (!ci->str)
		return Enoarg;
	free(d->name);
	d->name = strdup(ci->str);
	return call("doc:revisit", d->home);
}

DEF_CMD(doc_set_parent)
{
	if (ci->focus != ci->home->parent) {
		list_move(&ci->home->siblings, &ci->focus->children);
		ci->home->parent = ci->focus;
	}

	return 1;
}

DEF_CMD(doc_request_notify)
{
	struct doc *d = ci->home->data;
	pane_add_notify(ci->focus, ci->home, ci->key+8);
	return d->filter ? 0 : 1;
}

DEF_CMD(doc_notify)
{
	int ret = pane_notify(ci->key, ci->home, ci->num, ci->mark, ci->str,
			      ci->num2, ci->mark2, ci->str2, ci->comm2);
	/* Mustn't return 0, else will fall through to next doc */
	return ret ?: Enotarget;
}

DEF_CMD(doc_delview)
{
	if (ci->num >= 0)
		do_doc_del_view(ci->home->data, ci->num);
	else
		return Einval;
	return 1;
}

DEF_CMD(doc_addview)
{
	return 1 + do_doc_add_view(ci->home->data);
}

DEF_CMD(doc_vmarkget)
{
	struct mark *m, *m2;
	m = do_vmark_first(ci->home->data, ci->num);
	m2 = do_vmark_last(ci->home->data, ci->num);
	if (ci->num2 == 1 && ci->mark)
		m2 = do_vmark_at_point(ci->home->data, ci->mark,
				       ci->num);
	if (ci->num2 == 2)
		m2 = doc_new_mark(ci->home->data, ci->num);
	if (ci->num2 == 3 && ci->mark)
		m2 = do_vmark_at_or_before(ci->home->data, ci->mark, ci->num);
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
	ret = pane_notify("Notify:doc:viewers", p);
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

DEF_CMD(doc_do_revisit)
{
	/* If the document doesn't handle doc:revisit directly,
	 * ask it's parent, which is probably a document manager.
	 * We need this indirection to pass the document as the
	 * focus, so the document handler knows which document
	 * to revisit.
	 */
	if (!ci->home->parent)
		return Einval;
	return home_call(ci->home->parent, ci->key, ci->home, ci->num, ci->mark);
}

DEF_CMD(doc_mymark)
{
	/* Check if ci->mark is a mark in this document.
	 * 1 for 'yes', -1 for 'no'
	 */
	struct doc *d = ci->home->data;
	struct mark *m;

	m = doc_first_mark_all(d);
	while (m && m != ci->mark)
		m = doc_next_mark_all(m);
	return m ? 1 : Efalse;
}

DEF_CMD(doc_get_point)
{
	struct doc_data *dd = ci->home->data;

	comm_call(ci->comm2, "callback", ci->focus, 0, dd->point, NULL,
		  0, dd->mark);
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
	       comm_call(ci->comm2, "consume", ci->home, nxt, m)) {
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
		m = vmark_new(ci->focus, MARK_UNGROUPED);
	if (!m)
		return Esys;
	call_comm("doc:content", ci->focus, &g.c, 0, m);
	mark_free(m);
	comm_call(ci->comm2, "callback:get-str", ci->focus, 0, NULL, buf_final(&g.b));
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
		m = vmark_new(ci->focus, MARK_UNGROUPED);

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
		ret = Esys;
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

	if (ci->mark && dd->point)
		mark_to_mark(ci->mark, dd->point);
	return 1;
}

DEF_CMD(doc_notify_close)
{
	/* This pane has to go away */

	pane_close(ci->home);
	return 1;
}

DEF_CMD(doc_clone)
{
	struct doc_data *dd = ci->home->data;
	struct pane *p = doc_attach(ci->focus);

	if (!p)
		return 0;
	do_doc_assign(p, dd->doc, 1, NULL);
	call("Move-to", p, 0, dd->point);
	pane_clone_children(ci->home, p);
	return 1;
}

DEF_CMD(doc_close)
{
	struct doc_data *dd = ci->home->data;
	call("doc:push-point", dd->doc, 0, dd->point);
	mark_free(dd->point);
	mark_free(dd->mark);
	call("doc:closed", dd->doc);
	free(dd);
	ci->home->data = safe_cast NULL;
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

DEF_CMD(doc_assign)
{
	struct doc_data *dd = ci->home->data;
	struct pane *p2;
	if ((void*) (dd->doc))
		return Einval;
	p2 = do_doc_assign(ci->home, ci->focus, ci->num, ci->str);
	if (!p2)
		return Esys;
	comm_call(ci->comm2, "callback:doc", p2);
	return 1;
}

DEF_CMD(doc_replace)
{
	struct doc_data *dd = ci->home->data;
	return home_call(dd->doc, "doc:replace", ci->focus, 1, ci->mark, ci->str,
			 ci->num2, dd->point);
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

	switch(ci->num) {
	case 1:
		if (!dd->mark) {
			dd->mark = mark_dup(dd->point);
			if (!dd->mark)
				return Esys;
			attr_set_str(&dd->mark->attrs, "render:interactive-mark", "yes");
		}
		m = ci->mark ?: dd->point;
		mark_to_mark(dd->mark, m);
		/* Make sure mark is *before* point so insertion leave mark alone */
		if (dd->mark->seq > m->seq)
			mark_to_mark(dd->mark, m);
		break;
	case 2:
		mark_free(dd->mark);
		dd->mark = NULL;
		break;
	case 0:
		if (ci->mark)
			mark_to_mark(dd->point, ci->mark);
		break;
	}
	return 1;
}

DEF_CMD(doc_clip)
{
	struct doc_data *dd = ci->home->data;

	mark_clip(dd->point, ci->mark, ci->mark2);
	if (dd->mark)
		mark_clip(dd->mark, ci->mark, ci->mark2);
	return 1;
}

DEF_CMD(doc_pass_on)
{
	struct doc_data *dd = ci->home->data;
	int ret = home_call(dd->doc, ci->key, ci->focus, ci->num,
			 ci->mark ?: dd->point, ci->str,
			 ci->num2, ci->mark2, ci->str2,
			 ci->x, ci->y, ci->comm2);
	if (!ret && (ci->mark || ci->mark2))
		/* Mark won't be meaningful to any other document */
		ret = Enotarget;
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

struct map *doc_default_cmd safe;
static struct map *doc_handle_cmd safe;

DEF_LOOKUP_CMD(doc_handle, doc_handle_cmd);

static void init_doc_cmds(void)
{
	doc_default_cmd = key_alloc();
	doc_handle_cmd = key_alloc();

	key_add_range(doc_handle_cmd, "doc:", "doc;", &doc_pass_on);
	key_add_range(doc_handle_cmd, "Request:Notify:doc:", "Request:Notify:doc;", &doc_pass_on);
	key_add_range(doc_handle_cmd, "Notify:doc:", "Notify:doc;", &doc_pass_on);

	key_add(doc_handle_cmd, "Move-Char", &doc_char);
	key_add(doc_handle_cmd, "Move-Word", &doc_word);
	key_add(doc_handle_cmd, "Move-Expr", &doc_expr);
	key_add(doc_handle_cmd, "Move-WORD", &doc_WORD);
	key_add(doc_handle_cmd, "Move-EOL", &doc_eol);
	key_add(doc_handle_cmd, "Move-File", &doc_file);
	key_add(doc_handle_cmd, "Move-Line", &doc_line);
	key_add(doc_handle_cmd, "Move-View-Large", &doc_page);
	key_add(doc_handle_cmd, "doc:point", &doc_get_point);

	key_add(doc_handle_cmd, "Notify:doc:viewers", &doc_notify_viewers);
	key_add(doc_handle_cmd,	"Notify:Close", &doc_notify_close);
	key_add(doc_handle_cmd,	"Clone", &doc_clone);
	key_add(doc_handle_cmd,	"Close", &doc_close);
	key_add(doc_handle_cmd, "doc:dup-point", &doc_dup_point);
	key_add(doc_handle_cmd, "doc:assign", &doc_assign);
	key_add(doc_handle_cmd, "Replace", &doc_replace);
	key_add(doc_handle_cmd, "get-attr", &doc_handle_get_attr);
	key_add(doc_handle_cmd, "Move-to", &doc_move_to);
	key_add(doc_handle_cmd, "Notify:clip", &doc_clip);

	key_add(doc_default_cmd, "doc:set-attr", &doc_attr_set);
	key_add(doc_default_cmd, "doc:add-view", &doc_addview);
	key_add(doc_default_cmd, "doc:del-view", &doc_delview);
	key_add(doc_default_cmd, "doc:vmark-get", &doc_vmarkget);
	key_add(doc_default_cmd, "get-attr", &doc_get_attr);
	key_add(doc_default_cmd, "doc:set-name", &doc_set_name);
	key_add(doc_default_cmd, "doc:set-parent", &doc_set_parent);
	key_add(doc_default_cmd, "doc:destroy", &doc_do_destroy);
	key_add(doc_default_cmd, "doc:revisit", &doc_do_revisit);
	key_add(doc_default_cmd, "doc:drop-cache", &doc_drop_cache);
	key_add(doc_default_cmd, "doc:closed", &doc_do_closed);
	key_add(doc_default_cmd, "doc:mymark", &doc_mymark);
	key_add(doc_default_cmd, "doc:get-str", &doc_get_str);
	key_add(doc_default_cmd, "doc:write-file", &doc_write_file);
	key_add(doc_default_cmd, "doc:content", &doc_default_content);
	key_add(doc_default_cmd, "doc:push-point", &doc_push_point);
	key_add(doc_default_cmd, "doc:pop-point", &doc_pop_point);

	key_add_range(doc_default_cmd, "Request:Notify:doc:", "Request:Notify:doc;",
		      &doc_request_notify);
	key_add_range(doc_default_cmd, "Notify:doc:", "Notify:doc;",
		      &doc_notify);
	key_add_range(doc_default_cmd, "doc:set:", "doc:set;",
		      &doc_set);
}

static struct pane *do_doc_assign(struct pane *p safe, struct pane *doc safe,
			       int num, char *str)
{
	struct doc_data *dd = p->data;
	struct pane *p2 = NULL;
	struct mark *m;

	m = vmark_new(doc, MARK_POINT);
	if (!m)
		return NULL;
	if (call("doc:pop-point", doc, 0, m) <= 0)
		pane_notify("Notify:doc:viewers", doc, 0, m);
	dd->doc = doc;
	dd->point = m;
	attr_set_str(&m->attrs, "render:interactive-point", "yes");

	pane_add_notify(p, doc, "Notify:Close");
	pane_add_notify(p, doc, "Notify:doc:viewers");
	call("doc:revisit", doc, num);
	if (str) {
		p2 = call_pane("attach-view", p);
		if (p2)
			p2 = render_attach(str[0] ? str : NULL, p2);
	}
	return p2;
}

static struct pane *doc_attach(struct pane *parent)
{
	struct doc_data *dd = calloc(1, sizeof(*dd));

	return  pane_register(parent, 0, &doc_handle.c, dd, NULL);
}

struct pane *doc_new(struct pane *p safe, char *type, struct pane *parent)
{
	char buf[100];
	struct pane *np;

	snprintf(buf, sizeof(buf), "attach-doc-%s", type);
	np = call_pane(buf, p);
	if (np && parent)
		home_call(np, "doc:set-parent", parent);
	return np;
}

/*
 * If you have a document and want to view it, you call "doc:attach"
 * passing the attachment point as the focus.
 * Then call "doc:assign" on the resulting pane to provide the document.
 */
DEF_CMD(doc_do_attach)
{
	struct pane *p = doc_attach(ci->focus);
	if (!p)
		return Esys;
	return comm_call(ci->comm2, "callback:doc", p);
}

DEF_CMD(doc_open)
{
	struct pane *ed = ci->home;
	int fd = ci->num;
	char *name = ci->str;
	struct stat stb;
	struct pane *p;
	int autoclose = ci->num2 & 1;
	int filter = ci->num2 & 2;
	char pathbuf[PATH_MAX], *rp = NULL;

	if (!name)
		return Enoarg;
	stb.st_mode = 0;
	if (fd >= -1) {
		/* Try to canonicalize directory part of path */
		char *sl;
		sl = strrchr(name, '/');
		if (!sl) {
			rp = realpath(".", pathbuf);
			sl = name;
		} else if (sl-name < PATH_MAX-4) {
			char nbuf[PATH_MAX];
			strncpy(nbuf, name, sl-name);
			nbuf[sl-name] = 0;
			rp = realpath(nbuf, pathbuf);
			sl += 1;
		}

		if (rp) {
			strcat(rp, "/");
			strcat(rp, sl);
			name = rp;
		}
	}

	if (fd == -1)
		/* No open yet */
		fd = open(name, O_RDONLY);

	if (fd >= 0)
		fstat(fd, &stb);
	else
		stb.st_mode = S_IFREG;

	p = call_pane("docs:byfd", ed, 0, NULL, rp, fd);

	if (!p) {
		p = call_pane("global-multicall-open-doc-", ed, fd, NULL, name,
			      stb.st_mode & S_IFMT);

		if (!p) {
			if (fd != ci->num)
				close(fd);
			return Esys;
		}
		if (autoclose)
			call("doc:set:autoclose", p, 1);
		if (filter)
			call("doc:set:filter", p, 1);
		call("doc:load-file", p, 0, NULL, name, fd);
		call("global-multicall-doc:appeared-", p, 1);
	}
	if (fd != ci->num)
		close(fd);
	return comm_call(ci->comm2, "callback", p);
}

struct pane *doc_attach_view(struct pane *parent safe, struct pane *doc safe, char *render, int raise)
{
	struct pane *p;

	p = doc_attach(parent);
	if (p) {
		do_doc_assign(p, doc, raise, NULL);
		p = call_pane("attach-view", p);
	}
	if (p)
		p = render_attach(render, p);
	return p;
}

DEF_CMD(doc_from_text)
{
	struct pane *parent = ci->focus;
	char *name = ci->str;
	char *text = ci->str2;
	struct pane *p;

	p = doc_new(parent, "text", NULL);
	if (!p)
		return Esys;
	if (name) {
		call("doc:set-name", p, 0, NULL, name);
		call("global-multicall-doc:appeared-", p, 1);
	}
	call("doc:replace", p, 1, NULL, text, 1);
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
		ASSERT(d->views && !d->views[i].state);
	free(d->views);
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
	call_comm("global-set-command", ed, &doc_from_text, 0, NULL, "doc:from-text");
	call_comm("global-set-command", ed, &doc_do_attach, 0, NULL, "doc:attach");
	if (!(void*)doc_default_cmd)
		init_doc_cmds();
}
