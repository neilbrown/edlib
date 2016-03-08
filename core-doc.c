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

#define PRIVATE_DOC_REF
struct doc_ref {
	struct pane	*p;
	int		ignore;
};

#include "core.h"

static int do_doc_destroy(struct doc *d);

static int do_doc_add_view(struct doc *d, struct command *c)
{
	struct docview *g;
	int ret;
	int i;

	for (ret = 0; ret < d->nviews; ret++)
		if (d->views[ret].state == 0)
			break;
	if (ret == d->nviews) {
		/* Resize the view list */
		d->nviews += 4;
		g = malloc(sizeof(*g) * d->nviews);
		for (i = 0; i < ret; i++) {
			tlist_add(&g[i].head, GRP_HEAD, &d->views[i].head);
			tlist_del(&d->views[i].head);
			g[i].notify = d->views[i].notify;
			g[i].state = d->views[i].state;
		}
		for (; i < d->nviews; i++) {
			INIT_TLIST_HEAD(&g[i].head, GRP_HEAD);
			g[i].notify = NULL;
			g[i].state = 0;
		}
		free(d->views);
		d->views = g;
		/* now resize all the points */
		points_resize(d);
	}
	points_attach(d, ret);
	d->views[ret].notify = c;
	d->views[ret].state = 1;
	return ret;
}

static void do_doc_del_view_notifier(struct doc *d, struct command *c)
{
	/* This view should only have points on the list, not typed
	 * marks.  Just delete everything and clear the 'notify' pointer
	 */
	int i;
	for (i = 0; i < d->nviews; i++)
		if (d->views[i].notify == c)
			break;
	if (i >= d->nviews)
		return;
	d->views[i].notify = NULL;
	d->views[i].state = 0;
	while (!tlist_empty(&d->views[i].head)) {
		struct tlist_head *tl = d->views[i].head.next;
		if (TLIST_TYPE(tl) != GRP_LIST)
			abort();
		tlist_del_init(tl);
	}
}

static void do_doc_del_view(struct doc *d, int i)
{
	/* This view should only have points on the list, not typed
	 * marks.  Just delete everything and clear the 'notify' pointer
	 */
	if (i < 0 || i >= d->nviews)
		return;
	d->views[i].notify = NULL;
	d->views[i].state = 0;
	while (!tlist_empty(&d->views[i].head)) {
		struct tlist_head *tl = d->views[i].head.next;
		if (TLIST_TYPE(tl) != GRP_LIST)
			abort();
		tlist_del_init(tl);
	}
}

static int do_doc_find_view(struct doc *d, struct command *c)
{
	int i;
	for (i = 0 ; i < d->nviews; i++)
		if (d->views[i].notify == c)
			return i;
	return -1;
}

static void doc_close_views(struct doc *d)
{
	struct cmd_info ci = {0};
	int i;

	for (i = 0; i < d->nviews; i++)
		if (d->views[i].state)
			/* mark as being deleted */
			d->views[i].state = 2;
	ci.key = "Release";
	for (i = 0; i < d->nviews; i++) {
		struct command *c;
		if (d->views[i].state != 2)
			/* Don't delete newly added views */
			continue;
		if (d->views[i].notify == NULL)
			continue;
		ci.focus = ci.home = d->home;
		c = d->views[i].notify;
		ci.comm = c;
		c->func(&ci);
	}
}

void doc_init(struct doc *d)
{
	INIT_HLIST_HEAD(&d->marks);
	INIT_TLIST_HEAD(&d->points, 0);
	d->attrs = NULL;
	d->views = NULL;
	d->nviews = 0;
	d->name = NULL;
	d->map = NULL;
	d->deleting = 0;
	d->home = NULL;
}

DEF_CMD(doc_char)
{
	struct doc_data *dd = ci->home->data;
	int rpt = RPT_NUM(ci);

	while (rpt > 0) {
		if (mark_next(dd->doc, ci->mark) == WEOF)
			break;
		rpt -= 1;
	}
	while (rpt < 0) {
		if (mark_prev(dd->doc, ci->mark) == WEOF)
			break;
		rpt += 1;
	}

	return 1;
}

DEF_CMD(doc_word)
{
	struct doc_data *dd = ci->home->data;
	struct doc *d = dd->doc;
	int rpt = RPT_NUM(ci);

	/* We skip spaces, then either alphanum or non-space/alphanum */
	while (rpt > 0) {
		while (iswspace(doc_following(d, ci->mark)))
			mark_next(d, ci->mark);
		if (iswalnum(doc_following(d, ci->mark))) {
			while (iswalnum(doc_following(d, ci->mark)))
				mark_next(d, ci->mark);
		} else {
			wint_t wi;
			while ((wi=doc_following(d, ci->mark)) != WEOF &&
			       !iswspace(wi) && !iswalnum(wi))
				mark_next(d, ci->mark);
		}
		rpt -= 1;
	}
	while (rpt < 0) {
		while (iswspace(doc_prior(d, ci->mark)))
			mark_prev(d, ci->mark);
		if (iswalnum(doc_prior(d, ci->mark))) {
			while (iswalnum(doc_prior(d, ci->mark)))
				mark_prev(d, ci->mark);
		} else {
			wint_t wi;
			while ((wi=doc_prior(d, ci->mark)) != WEOF &&
			       !iswspace(wi) && !iswalnum(wi))
				mark_prev(d, ci->mark);
		}
		rpt += 1;
	}

	return 1;
}

DEF_CMD(doc_WORD)
{
	struct doc_data *dd = ci->home->data;
	struct doc *d = dd->doc;
	int rpt = RPT_NUM(ci);

	/* We skip spaces, then non-spaces */
	while (rpt > 0) {
		wint_t wi;
		while (iswspace(doc_following(d, ci->mark)))
			mark_next(d, ci->mark);

		while ((wi=doc_following(d, ci->mark)) != WEOF &&
		       !iswspace(wi))
			mark_next(d, ci->mark);
		rpt -= 1;
	}
	while (rpt < 0) {
		wint_t wi;
		while (iswspace(doc_prior(d, ci->mark)))
			mark_prev(d, ci->mark);
		while ((wi=doc_prior(d, ci->mark)) != WEOF &&
		       !iswspace(wi))
			mark_prev(d, ci->mark);
		rpt += 1;
	}

	return 1;
}

DEF_CMD(doc_eol)
{
	struct doc_data *dd = ci->home->data;
	struct doc *d = dd->doc;
	wint_t ch = 1;
	int rpt = RPT_NUM(ci);

	while (rpt > 0 && ch != WEOF) {
		while ((ch = mark_next(d, ci->mark)) != WEOF &&
		       ch != '\n')
			;
		rpt -= 1;
	}
	while (rpt < 0 && ch != WEOF) {
		while ((ch = mark_prev(d, ci->mark)) != WEOF &&
		       ch != '\n')
			;
		rpt += 1;
	}
	if (ch == '\n') {
		if (RPT_NUM(ci) > 0)
			mark_prev(d, ci->mark);
		else if (RPT_NUM(ci) < 0)
			mark_next(d, ci->mark);
	}
	return 1;
}

DEF_CMD(doc_file)
{
	struct doc_data *dd = ci->home->data;
	struct doc *d = dd->doc;
	wint_t ch = 1;
	int rpt = RPT_NUM(ci);
	struct mark *m = ci->mark;

	if (!m)
		m = dd->point;
	while (rpt > 0 && ch != WEOF) {
		while ((ch = mark_next(d, m)) != WEOF)
			;
		rpt = 0;
	}
	if (rpt > 0)
		__mark_reset(d, m, 0, 1);
	if (rpt < 0)
		mark_reset(d, m);

	return 1;
}

DEF_CMD(doc_line)
{
	struct doc_data *dd = ci->home->data;
	struct doc *d = dd->doc;
	wint_t ch = 1;
	int rpt = RPT_NUM(ci);

	while (rpt > 0 && ch != WEOF) {
		while ((ch = mark_next(d, ci->mark)) != WEOF &&
		       ch != '\n')
			;
		rpt -= 1;
	}
	while (rpt < 0 && ch != WEOF) {
		while ((ch = mark_prev(d, ci->mark)) != WEOF &&
		       ch != '\n')
			;
		rpt += 1;
	}
	return 1;
}

DEF_CMD(doc_page)
{
	struct doc_data *dd = ci->home->data;
	struct doc *d = dd->doc;
	wint_t ch = 1;
	int rpt = RPT_NUM(ci);

	rpt *= ci->home->h-2;
	while (rpt > 0 && ch != WEOF) {
		while ((ch = mark_next(d, ci->mark)) != WEOF &&
		       ch != '\n')
			;
		rpt -= 1;
	}
	while (rpt < 0 && ch != WEOF) {
		while ((ch = mark_prev(d, ci->mark)) != WEOF &&
		       ch != '\n')
			;
		rpt += 1;
	}
	return 1;
}

DEF_CMD(doc_do_replace)
{
	bool first_change = (ci->extra == 0);

	doc_replace(ci->home, ci->mark, ci->str, &first_change);
	return 1;
}

DEF_CMD(doc_attr_set)
{
	struct doc_data *dd = ci->home->data;
	struct doc *d = dd->doc;

	if (ci->str2 == NULL && ci->extra == 1)
		attr_set_int(&d->attrs, ci->str, ci->numeric);
	else
		attr_set_str(&d->attrs, ci->str, ci->str2, -1);
	return 1;
}

static struct map *doc_default_cmd;

static void init_doc_defaults(void)
{

	if (doc_default_cmd)
		return;

	doc_default_cmd = key_alloc();

	key_add(doc_default_cmd, "Move-Char", &doc_char);
	key_add(doc_default_cmd, "Move-Word", &doc_word);
	key_add(doc_default_cmd, "Move-WORD", &doc_WORD);
	key_add(doc_default_cmd, "Move-EOL", &doc_eol);
	key_add(doc_default_cmd, "Move-File", &doc_file);
	key_add(doc_default_cmd, "Move-Line", &doc_line);
	key_add(doc_default_cmd, "Move-View-Large", &doc_page);
	key_add(doc_default_cmd, "Replace", &doc_do_replace);
	key_add(doc_default_cmd, "doc:attr-set", &doc_attr_set);
}

DEF_CMD(doc_handle)
{
	struct doc_data *dd = ci->home->data;
	int ret;

	if (strcmp(ci->key, "Notify:Close") == 0) {
		/* This pane has to go away */
		struct doc_data *dd = ci->home->data;
		struct pane *par = ci->home, *p;

		/* Point needs to go now else it will be delete when
		 * the doc is destroy, and we get a dangling pointer
		 */
		struct mark *pnt = dd->point;
		dd->point = NULL;
		mark_free(pnt);

		/* Need another document to fill this pane. */
		/* FIXME make this conditional */
		p = pane_child(par);
		if (p)
			pane_close(p);
		p = call_pane("docs:choose", ci->focus, 0, NULL, 0);
		if (!p)
			return 1;
		doc_attach_view(par, p, NULL);
		p = pane_child(par);
		if (p) {
			pane_subsume(p, par);
			pane_close(p);
		}
		return 1;
	}

	if (strcmp(ci->key, "Request:Notify:Replace") == 0) {
		pane_add_notify(ci->focus, dd->doc->home, "Notify:Replace");
		return 1;
	}

	if (strcmp(ci->key, "Clone") == 0) {
		struct pane *p = doc_attach(ci->focus, dd->doc);
		struct pane *c = pane_child(ci->home);
		struct doc_data *dd2 = p->data;

		dd2->point = point_dup(dd->point);
		p->pointer = dd2->point;
		if (c)
			pane_clone(c, p);
		return 1;
	}

	if (strcmp(ci->key, "Close") == 0) {
		if (dd->point)
			mark_free(dd->point);
		free(dd);
		ci->home->data = NULL;
		return 1;
	}

	if (strcmp(ci->key, "doc:dup-point") == 0) {
		struct mark *pt = dd->point;
		struct mark *m;
		if (ci->mark && ci->mark->viewnum == MARK_POINT)
			pt = ci->mark;

		if (!pt || !ci->comm2)
			return -1;

		if (ci->extra == MARK_POINT)
			m = point_dup(pt);
		else if (ci->extra == MARK_UNGROUPED)
			m = mark_dup(pt, 1);
		else
			m = do_mark_at_point(dd->doc, pt,
					     ci->extra);

		return comm_call(ci->comm2, "callback:dup-point", ci->focus,
				 0, m, NULL, 0);
	}

	if (strcmp(ci->key, "Move-to") == 0) {
		point_to_mark(dd->point, ci->mark);
		return 1;
	}

	if (strcmp(ci->key, "doc:set-name") == 0) {
		doc_set_name(dd->doc, ci->str);
		return 1;
	}

	if (strcmp(ci->key, "doc:get-attr") == 0 &&
	    strcmp(ci->str, "doc:name") == 0)
		return comm_call(ci->comm2, "callback:get_attr", ci->focus, 0,
				 NULL, dd->doc->name, 0);

	if (strcmp(ci->key, "doc:add-view") == 0) {
		return 1 + do_doc_add_view(dd->doc, ci->comm2);
	}

	if (strcmp(ci->key, "doc:del-view") == 0) {
		if (ci->numeric >= 0)
			do_doc_del_view(dd->doc, ci->numeric);
		else if (ci->comm2)
			do_doc_del_view_notifier(dd->doc, ci->comm2);
		else
			return -1;
		return 1;
	}

	if (strcmp(ci->key, "doc:find-view") == 0) {
		int ret;
		if (!ci->comm2)
			return -1;
		ret =  do_doc_find_view(dd->doc, ci->comm2);
		if (ret < 0)
			return ret;
		return ret + 1;
	}

	if (strcmp(ci->key, "doc:vmark-get") == 0) {
		struct mark *m, *m2;
		m = do_vmark_first(dd->doc, ci->numeric);
		m2 = do_vmark_last(dd->doc, ci->numeric);
		if (ci->extra == 1 && dd->point)
			m2 = do_vmark_at_point(dd->doc, dd->point,
					       ci->numeric);
		if (ci->extra == 2)
			m2 = doc_new_mark(dd->doc, ci->numeric);
		if (ci->extra == 3)
			m2 = do_vmark_at_or_before(dd->doc, ci->mark, ci->numeric);
		return comm_call7(ci->comm2, "callback:vmark", ci->focus,
				  0, m, NULL, 0, NULL, m2);
	}
	if (strcmp(ci->key, "doc:destroy") == 0)
		return do_doc_destroy(dd->doc);

	ret = key_lookup(dd->doc->map, ci);
	ret = ret ?: key_lookup(doc_default_cmd, ci);
	return ret;
}

struct pane *doc_attach(struct pane *parent, struct doc *d)
{
	struct pane *p;
	struct doc_data *dd = malloc(sizeof(*dd));

	dd->doc = d;

	p = pane_register(parent, 0, &doc_handle, dd, NULL);
	if (!d->home)
		d->home = p;
	else {
		/* non-home panes need to be notified so they can self-destruct */
		pane_add_notify(p, d->home, "Notify:Close");
	}
	dd->point = NULL;
	call5("docs:attach", d->home, 1, NULL, NULL, 0);
	return p;
}

DEF_CMD(take_pane)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);
	cr->p = ci->focus;
	return 1;
}

struct doc *doc_new(struct pane *p, char *type)
{
	char buf[100];
	struct cmd_info ci = {0};
	struct call_return cr;
	struct doc_data *dd;

	init_doc_defaults();

	sprintf(buf, "doc-%s", type);
	ci.key = buf;
	ci.focus = ci.home = p;
	cr.c = take_pane;
	cr.p = NULL;
	ci.comm2 = &cr.c;
	if (!key_handle(&ci)) {
		call5("global-load-module", p, 0, NULL, buf, 0);
		if (!key_handle(&ci))
			return NULL;
	}
	dd = cr.p->data;
	return dd->doc;
}

struct pane *doc_open(struct pane *ed, int fd, char *name)
{
	struct stat stb;
	struct pane *p;
	struct doc *d;
	char pathbuf[PATH_MAX], *rp;

	p = call_pane7("docs:byfd", ed, 0, NULL, fd, name);

	if (p)
		return p;

	if (fstat(fd, &stb) != 0)
		return NULL;

	rp = realpath(name, pathbuf);
	if ((stb.st_mode & S_IFMT) == S_IFREG) {
		d = doc_new(ed, "text");
	} else if ((stb.st_mode & S_IFMT) == S_IFDIR) {
		d = doc_new(ed, "dir");
	} else
		return NULL;
	if (!d)
		return NULL;
	doc_load_file(d->home, fd, rp);
	return d->home;
}

struct pane *doc_attach_view(struct pane *parent, struct pane *doc, char *render)
{
	struct pane *p;
	struct doc_data *dd = doc->data;
	p = doc_attach(parent, dd->doc);
	if (p) {
		dd = p->data;
		dd->point = point_new(dd->doc);
		p->pointer = dd->point;
		p = pane_attach(p, "view", NULL, NULL);
	}
	if (p)
		p = render_attach(render, p);
	return p;
}

struct pane *doc_from_text(struct pane *parent, char *name, char *text)
{
	bool first = 1;
	struct pane *p;
	struct doc *d;

	d = doc_new(parent, "text");
	if (!d)
		return NULL;
	doc_set_name(d, name);
	p = doc_attach_view(parent, d->home, NULL);
	if (!p) {
		do_doc_destroy(d);
		return p;
	}
	doc_replace(p, NULL, text, &first);
	call3("Move-File", p, -1, NULL);
	return p;
}

void doc_set_name(struct doc *d, char *name)
{
	free(d->name);
	d->name = strdup(name);
	call3("doc:check_name", d->home, -1, NULL);
}

DEF_CMD(doc_attr_callback)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);
	cr->s = ci->str;
	return 1;
}

char *doc_attr(struct pane *dp, struct mark *m, bool forward, char *attr)
{
	struct cmd_info ci = {0};
	struct call_return cr;

	ci.key = "doc:get-attr";
	ci.home = ci.focus = dp;
	ci.mark = m;
	ci.numeric = forward ? 1 : 0;
	ci.str = attr;
	ci.comm = dp->handle;
	cr.c = doc_attr_callback;
	cr.s = NULL;
	ci.comm2 = &cr.c;
	if (!dp->handle || dp->handle->func(&ci) == 0)
		return NULL;
	return cr.s;
}

DEF_CMD(doc_str_callback)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);
	cr->s = strdup(ci->str);
	return 1;
}

char *doc_getstr(struct pane *from, struct mark *to)
{
	struct cmd_info ci = {0};
	int ret;
	struct call_return cr;

	ci.key = "doc:get-str";
	ci.focus = from;
	ci.mark = to;
	cr.c = doc_str_callback;
	cr.s = NULL;
	ci.comm2 = &cr.c;
	ret = key_handle(&ci);
	if (!ret)
		return NULL;
	return cr.s;
}

static int do_doc_destroy(struct doc *d)
{
	/* If there are no views on the document, then unlink from
	 * the documents list and destroy it.
	 */
	int i;
	struct cmd_info ci = {0};

	d->deleting = 1;

	doc_close_views(d);
	pane_notify_close(d->home);
	d->deleting = 0;

	if (!list_empty(&d->home->notifiees))
		/* still being watched */
		return -1;
	for (i = 0; i < d->nviews; i++)
		if (d->views[i].state)
			/* still in use */
			return -1;

	ci.home = ci.focus = d->home;
	ci.key = "doc:destroy";
	if (key_lookup(d->map, &ci) < 0)
		return -1;

	pane_close(d->home);

	free(d->views);
	attr_free(&d->attrs);
	free(d->name);
	while (!hlist_empty(&d->marks)) {
		struct mark *m = hlist_first_entry(&d->marks, struct mark, all);
		if (m->viewnum == MARK_POINT || m->viewnum == MARK_UNGROUPED)
			mark_free(m);
		else
			/* vmarks should have gone already */
			ASSERT(0);
	}
	free(d);
	return 1;
}
