/*
 * Copyright Neil Brown <neil@brown.name> 2015
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
#include <stdio.h>

#define PRIVATE_DOC_REF
struct doc_ref {
	struct doc	*d;
	int		ignore;
};

#include "core.h"

int doc_add_view(struct doc *d, struct command *c)
{
	struct docview *g;
	int ret;
	int i;

	for (ret = 0; ret < d->nviews; ret++)
		if (d->views[ret].notify == NULL)
			break;
	if (ret == d->nviews) {
		/* Resize the view list */
		d->nviews += 4;
		g = malloc(sizeof(*g) * d->nviews);
		for (i = 0; i < ret; i++) {
			tlist_add(&g[i].head, GRP_HEAD, &d->views[i].head);
			tlist_del(&d->views[i].head);
			g[i].notify = d->views[i].notify;
		}
		for (; i < d->nviews; i++) {
			INIT_TLIST_HEAD(&g[i].head, GRP_HEAD);
			g[i].notify = NULL;
		}
		free(d->views);
		d->views = g;
		/* now resize all the points */
		points_resize(d);
	}
	points_attach(d, ret);
	d->views[ret].space = 0;
	d->views[ret].notify = c;
	return ret;
}

void doc_del_view(struct doc *d, struct command *c)
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
	while (!tlist_empty(&d->views[i].head)) {
		struct tlist_head *tl = d->views[i].head.next;
		if (TLIST_TYPE(tl) != GRP_LIST)
			abort();
		tlist_del_init(tl);
	}
}

int doc_find_view(struct doc *d, struct command *c)
{
	int i;
	for (i = 0 ; i < d->nviews; i++)
		if (d->views[i].notify == c)
			return i;
	return -1;
}

void doc_close_views(struct doc *d)
{
	struct cmd_info ci;
	int i;

	ci.key = "Release";
	for (i = 0; i < d->nviews; i++) {
		struct point pt, *ptp = &pt;
		struct command *c;
		if (d->views[i].notify == NULL)
			continue;
		ci.pointp = &ptp;
		pt.doc = d;
		c = d->views[i].notify;
		ci.comm = c;
		c->func(&ci);
	}
}


void doc_init(struct doc *d)
{
	INIT_HLIST_HEAD(&d->marks);
	INIT_TLIST_HEAD(&d->points, 0);
	INIT_LIST_HEAD(&d->list);
	d->attrs = NULL;
	d->views = NULL;
	d->nviews = 0;
	d->name = NULL;
	d->map = NULL;
}

struct point *doc_new(struct editor *ed, char *type)
{
	char buf[100];
	struct cmd_info ci = {0};
	struct point *pt;

	sprintf(buf, "doc-%s", type);
	ci.key = buf;
	ci.pointp = &pt;
	if (!key_lookup(ed->commands, &ci)) {
		editor_load_module(ed, buf);
		if (!key_lookup(ed->commands, &ci))
			return NULL;
	}
	pt->doc->ed = ed;
	return pt;
}

struct pane *doc_open(struct pane *parent, int fd, char *name, char *render)
{
	struct stat stb;
	struct doc *d;
	struct point *pt;
	struct pane *p;
	char pathbuf[PATH_MAX], *rp;

	fstat(fd, &stb);
	list_for_each_entry(d, &pane2ed(parent)->documents, list)
		if (d->ops->same_file(d, fd, &stb)) {
			point_new(d, &pt);
			goto found;
		}

	rp = realpath(name, pathbuf);
	if ((stb.st_mode & S_IFMT) == S_IFREG) {
		pt = doc_new(pane2ed(parent), "text");
	} else if ((stb.st_mode & S_IFMT) == S_IFDIR) {
		pt = doc_new(pane2ed(parent), "dir");
	} else
		return NULL;
	if (!pt)
		return NULL;
	doc_load_file(pt->doc, NULL, fd, rp);
	point_reset(pt);
found:
	p = pane_attach(parent, "view", pt, NULL);
	if (p) {
		render_attach(render, p);
	} else {
		d = pt->doc;
		point_free(pt);
		doc_destroy(d);
	}
	return p;
}

struct pane *doc_from_text(struct pane *parent, char *name, char *text)
{
	bool first = 1;
	struct pane *p;
	struct point *pt, **ptp;

	pt = doc_new(pane2ed(parent), "text");
	if (!pt)
		return NULL;
	p = pane_attach(parent, "view", pt, NULL);
	if (!p) {
		struct doc *d = pt->doc;
		point_free(pt);
		doc_destroy(d);
		return p;
	}
	ptp = pane_point(p);
	doc_set_name((*ptp)->doc, name);
	doc_replace(*ptp, NULL, text, &first);
	point_reset(*ptp);
	render_attach(NULL, p);
	return p;
}

void doc_set_name(struct doc *d, char *name)
{
	char *nname = malloc(strlen(name) + sizeof("<xxx>"));
	int unique = 1;
	int conflict = 1;

	while (conflict && unique < 1000) {
		struct doc *d2;
		conflict = 0;
		if (unique > 1)
			sprintf(nname, "%s<%d>", name, unique);
		else
			strcpy(nname, name);
		list_for_each_entry(d2, &d->ed->documents, list) {
			if (d != d2 && strcmp(nname, d2->name) == 0) {
				conflict = 1;
				unique += 1;
				break;
			}
		}
	}
	free(d->name);
	d->name = nname;
}

struct doc *doc_find(struct editor *ed, char *name)
{
	struct doc *d;

	list_for_each_entry(d, &ed->documents, list)
		if (strcmp(name, d->name) == 0)
			return d;
	return NULL;
}

/* the 'docs' document type is special in that there can only ever
 * be one instance - the list of documents.
 * So there is no 'doctype' registered, just a document which can never
 * be deleted.
 */

struct docs {
	struct doc	doc;
};

static void docs_replace(struct point *pos, struct mark *end,
			 char *str, bool *first)
{
}

static int docs_same_file(struct doc *d, int fd, struct stat *stb)
{
	return 0;
}

static int docs_reundo(struct point *p, bool redo)
{
	return 0;
}

static wint_t docs_step(struct doc *doc, struct mark *m, bool forward, bool move)
{
	struct doc *d = m->ref.d, *next;

	if (forward) {
		/* report on d */
		if (d == NULL || d == list_last_entry(&doc->ed->documents, struct doc, list))
			next = NULL;
		else
			next = list_next_entry(d, list);
	} else {
		next = d;
		if (d == NULL)
			d = list_last_entry(&doc->ed->documents, struct doc, list);
		else if (d == list_first_entry(&doc->ed->documents, struct doc, list))
			d = NULL;
		else
			d = list_prev_entry(d, list);
		if (d)
			next = d;
	}
	if (move)
		m->ref.d = next;
	if (d == NULL)
		return WEOF;
	else
		return ' ';
}

static char *docs_getstr(struct doc *d, struct mark *from, struct mark *to)
{
	return NULL;
}

static void docs_setref(struct doc *doc, struct mark *m, bool start)
{

	if (start)
		m->ref.d = list_first_entry(&doc->ed->documents, struct doc, list);
	else
		m->ref.d = list_last_entry(&doc->ed->documents, struct doc, list);

	m->ref.ignore = 0;
}

static int docs_sameref(struct doc *d, struct mark *a, struct mark *b)
{
	return a->ref.d == b->ref.d;
}


static char *docs_get_attr(struct doc *doc, struct mark *m,
			  bool forward, char *attr)
{
	struct doc *d;

	if (!m) {
		char *a = attr_get_str(doc->attrs, attr, -1);
		if (a)
			return a;
		if (strcmp(attr, "heading") == 0)
			return "<bold,underline>  Document             File</>";
		if (strcmp(attr, "line-format") == 0)
			return "  %+name:20 %filename";
		return NULL;
	}
	d = m->ref.d;
	if (!forward) {
		if (!d)
			d = list_last_entry(&doc->ed->documents, struct doc, list);
		else if (d != list_first_entry(&doc->ed->documents, struct doc, list))
			d = list_prev_entry(d, list);
		else
			d = NULL;
	}
	if (!d)
		return NULL;
	if (strcmp(attr, "name") == 0)
		return d->name;
	return doc_attr(d, NULL, 0, attr);
}

static int docs_set_attr(struct point *p, char *attr, char *val)
{
	return 0;
}

static struct doc_operations docs_ops = {
	.replace   = docs_replace,
	.same_file = docs_same_file,
	.reundo    = docs_reundo,
	.step      = docs_step,
	.get_str   = docs_getstr,
	.set_ref   = docs_setref,
	.same_ref  = docs_sameref,
	.get_attr  = docs_get_attr,
	.set_attr  = docs_set_attr,
};

DEF_CMD(docs_open)
{
	struct pane *p = ci->home;
	struct point *pt;
	struct doc *dc = p->point->m.ref.d;
	struct pane *par = p->parent;
	char *renderer = NULL;

	/* close this pane, open the given document. */
	if (dc == NULL)
		return 0;

	if (strcmp(ci->key, "Chr-h") == 0)
		renderer = "hex";

	point_new(dc, &pt);
	pane_close(p);
	p = pane_attach(par, "view", pt, NULL);
	if (p) {
		render_attach(renderer, p);
		pane_focus(p);
		return 1;
	} else {
		point_free(pt);
		return 0;
	}
}

DEF_CMD(docs_bury)
{
	struct doc *d = (*ci->pointp)->doc;

	doc_close_views(d);
	return 1;
}

void doc_make_docs(struct editor *ed)
{
	struct docs *ds = malloc(sizeof(*ds));
	struct map *docs_map = key_alloc();

	doc_init(&ds->doc);
	ds->doc.ed = ed;
	ds->doc.ops = &docs_ops;
	ds->doc.default_render = "format";
	doc_set_name(&ds->doc, "*Documents*");
	ed->docs = &ds->doc;

	key_add(docs_map, "Chr-f", &docs_open);
	key_add(docs_map, "Chr-h", &docs_open);
	key_add(docs_map, "Return", &docs_open);
	key_add(docs_map, "Chr-q", &docs_bury);
	ds->doc.map = docs_map;

	doc_promote(&ds->doc);
}

static void docs_release(struct doc *d)
{
	/* This document is about to be moved in the list.
	 * Any mark pointing at it is moved forward
	 */
	struct editor *ed = d->ed;
	struct mark *m;

	for (m = doc_first_mark_all(ed->docs);
	     m;
	     m = doc_next_mark_all(ed->docs, m))
		if (m->ref.d == d) {
			docs_step(ed->docs, m, 1, 1);
			doc_notify_change(ed->docs, m);
		}
}

static void docs_attach(struct doc *d)
{
	/* This document has just been added to the list.
	 * any mark pointing just past it is moved back.
	 */
	struct editor *ed = d->ed;
	struct mark *m;

	if (d->list.next == &ed->documents)
		/* At the end, nothing to do */
		return;
	for (m = doc_first_mark_all(ed->docs);
	     m;
	     m = doc_next_mark_all(ed->docs, m))
		if (d->list.next == &m->ref.d->list) {
			docs_step(ed->docs, m, 0, 1);
			doc_notify_change(ed->docs, m);
		}
}

void doc_promote(struct doc *d)
{
	docs_release(d);
	list_move(&d->list, &d->ed->documents);
	docs_attach(d);
}

int  doc_destroy(struct doc *d)
{
	/* If there are no views on the document, then unlink from
	 * the documents list and destroy it.
	 */
	int i;

	for (i = 0; i < d->nviews; i++)
		if (d->views[i].notify)
			/* still in used */
			return 0;
	if (d->ops == &docs_ops)
		return 0;

	docs_release(d);
	list_del(&d->list);

	free(d->views);
	attr_free(&d->attrs);
	free(d->name);
	while (d->marks.first) {
		struct mark *m = hlist_first_entry(&d->marks, struct mark, all);
		mark_free(m);
	}
	d->ops->destroy(d);
	return 1;
}
