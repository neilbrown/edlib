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
	struct pane	*p;
	int		ignore;
};

#include "core.h"

static int do_doc_add_view(struct doc *d, struct command *c, int size)
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
			g[i].space = d->views[i].space;
			g[i].marked = d->views[i].marked;
		}
		for (; i < d->nviews; i++) {
			INIT_TLIST_HEAD(&g[i].head, GRP_HEAD);
			g[i].notify = NULL;
			g[i].space = 0;
			g[i].marked = 0;
		}
		free(d->views);
		d->views = g;
		/* now resize all the points */
		points_resize(d);
	}
	points_attach(d, ret);
	d->views[ret].space = 0;
	if (size > 0 && (unsigned)size > sizeof(struct mark))
		d->views[ret].space = size - sizeof(struct mark);
	d->views[ret].notify = c;
	d->views[ret].marked = 0;
	return ret;
}

static void do_doc_del_view(struct doc *d, struct command *c)
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
	struct cmd_info ci;
	int i;

	for (i = 0; i < d->nviews; i++)
		if (d->views[i].notify)
			d->views[i].marked = 1;
		else
			d->views[i].marked = 0;
	ci.key = "Release";
	for (i = 0; i < d->nviews; i++) {
		struct command *c;
		if (!d->views[i].marked)
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

DEF_CMD(doc_handle)
{
	struct doc *d = ci->home->data;

	/* This is a hack - I should use a watcher, but I don't have
	 * anywhere to store it.
	 * FIXME make this optional
	 */
	if (strcmp(ci->key, "Refresh") == 0) {
		struct pane *p = pane_child(ci->home);
		if (p)
			return 0; /* use default handling */
		p = editor_choose_doc(pane2ed(ci->home));
		doc_attach_view(ci->home, p, NULL);
		return 0;
	}
	if (strcmp(ci->key, "Clone") == 0) {
		struct pane *p = doc_attach(ci->focus, d);
		struct pane *c = pane_child(ci->home);

		point_dup(ci->home->point, &p->point);
		if (c)
			pane_clone(c, p);
		return 1;
	}

	if (strcmp(ci->key, "Close") == 0) {
		if (ci->home->point)
			point_free(ci->home->point);
		return 1;
	}

	if (strcmp(ci->key, "PointDup") == 0) {
		struct point *pt = NULL;
		if (ci->home->point)
			point_dup(ci->home->point, &pt);
		ci->mark = &pt->m;
		return 1;
	}

	if (strcmp(ci->key, "Move-to") == 0) {
		point_to_mark(ci->home->point, ci->mark);
		return 1;
	}

	if (strcmp(ci->key, "doc:set-name") == 0) {
		doc_set_name(d, ci->str);
		return 1;
	}

	if (strcmp(ci->key, "doc:add-view") == 0) {
		if (!ci->comm2)
			return -1;
		ci->extra = do_doc_add_view(d, ci->comm2, ci->extra);
		return 1;
	}

	if (strcmp(ci->key, "doc:del-view") == 0) {
		if (!ci->comm2)
			return -1;
		do_doc_del_view(d, ci->comm2);
		return 1;
	}

	if (strcmp(ci->key, "doc:find-view") == 0) {
		if (!ci->comm2)
			return -1;
		ci->extra = do_doc_find_view(d, ci->comm2);
		return 1;
	}

	if (strcmp(ci->key, "doc:vmark-get") == 0) {
		ci->mark = do_vmark_first(d, ci->numeric);
		ci->mark2 = do_vmark_last(d, ci->numeric);
		if (ci->extra && ci->home->point)
			ci->mark2 = do_vmark_at_point(ci->home->point, ci->numeric);
		return 1;
	}

	return key_lookup(d->map, ci);
}

struct pane *doc_attach(struct pane *parent, struct doc *d)
{
	struct pane *p;

	p = pane_register(parent, 0, &doc_handle, d, NULL);
	if (!d->home)
		d->home = p;
	d->ed = pane2ed(parent);
	doc_promote(d);
	return p;
}

struct pane *doc_new(struct editor *ed, char *type)
{
	char buf[100];
	struct cmd_info ci = {0};

	sprintf(buf, "doc-%s", type);
	ci.key = buf;
	ci.focus = ci.home = &ed->root;
	if (!key_lookup(ed->commands, &ci)) {
		editor_load_module(ed, buf);
		if (!key_lookup(ed->commands, &ci))
			return NULL;
	}
	return ci.focus;
}

struct pane *doc_open(struct editor *ed, int fd, char *name)
{
	struct stat stb;
	struct pane *p;
	char pathbuf[PATH_MAX], *rp;

	fstat(fd, &stb);
	list_for_each_entry(p, &ed->root.focus->children, siblings) {
		struct cmd_info ci2 = {0};
		ci2.key = "doc:same-file";
		ci2.focus = p;
		ci2.extra = -1;
		ci2.str2 = (void*)&stb;
		if (key_handle_focus(&ci2) > 0)
			return p;
	}

	rp = realpath(name, pathbuf);
	if ((stb.st_mode & S_IFMT) == S_IFREG) {
		p = doc_new(ed, "text");
	} else if ((stb.st_mode & S_IFMT) == S_IFDIR) {
		p = doc_new(ed, "dir");
	} else
		return NULL;
	if (!p)
		return NULL;
	doc_load_file(p, fd, rp);
	return p;
}

struct pane *doc_attach_view(struct pane *parent, struct pane *doc, char *render)
{
	struct pane *p;
	p = doc_attach(parent, doc->data);
	if (p) {
		point_new(doc->data, &p->point);
		p = pane_attach(p, "view", doc, NULL);
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
	struct cmd_info ci = {0};

	p = doc_new(pane2ed(parent), "text");
	if (!p)
		return NULL;
	d = p->data;
	doc_set_name(d, name);
	p = doc_attach_view(parent, p, NULL);
	if (!p) {
		doc_destroy(p->data);
		return p;
	}
	doc_replace(p, NULL, text, &first);
	ci.key = "Move-File";
	ci.numeric = -1;
	ci.focus = p;
	key_handle_focus(&ci);
	return p;
}

void doc_set_name(struct doc *d, char *name)
{
	char *nname = malloc(strlen(name) + sizeof("<xxx>"));
	int unique = 1;
	int conflict = 1;

	while (conflict && unique < 1000) {
		struct pane *p;
		conflict = 0;
		if (unique > 1)
			sprintf(nname, "%s<%d>", name, unique);
		else
			strcpy(nname, name);
		list_for_each_entry(p, &d->ed->root.focus->children, siblings) {
			struct doc *d2 = p->data;
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

struct pane *doc_find(struct editor *ed, char *name)
{
	struct pane *p;

	list_for_each_entry(p, &ed->root.focus->children, siblings) {
		struct doc *d = p->data;
		if (strcmp(name, d->name) == 0)
			return p;
	}
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

DEF_CMD(docs_step)
{
	struct doc *doc = ci->home->data;
	struct mark *m = ci->mark;
	bool forward = ci->numeric;
	bool move = ci->extra;

	struct pane *p = m->ref.p, *next;

	if (forward) {
		/* report on d */
		if (p == NULL || p == list_last_entry(&doc->ed->root.focus->children,
						      struct pane, siblings))
			next = NULL;
		else
			next = list_next_entry(p, siblings);
	} else {
		next = p;
		if (p == NULL)
			p = list_last_entry(&doc->ed->root.focus->children,
					    struct pane, siblings);
		else if (p == list_first_entry(&doc->ed->root.focus->children,
					       struct pane, siblings))
			p = NULL;
		else
			p = list_prev_entry(p, siblings);
		if (p)
			next = p;
	}
	if (move)
		m->ref.p = next;
	if (p == NULL)
		ci->extra = WEOF;
	else
		ci->extra = ' ';
	return 1;
}

DEF_CMD(docs_set_ref)
{
	struct editor *ed = pane2ed(ci->home);
	struct mark *m = ci->mark;

	if (ci->numeric == 1)
		m->ref.p = list_first_entry(&ed->root.focus->children,
					    struct pane, siblings);
	else
		m->ref.p = list_last_entry(&ed->root.focus->children,
					   struct pane, siblings);

	m->ref.ignore = 0;
	m->rpos = 0;
	return 1;
}

DEF_CMD(docs_mark_same)
{
	ci->extra = ci->mark->ref.p == ci->mark2->ref.p;
	return 1;
}

static char *__docs_get_attr(struct doc *doc, struct mark *m,
			     bool forward, char *attr)
{
	struct doc *d;
	struct pane *p;

	if (!m) {
		char *a = attr_get_str(doc->attrs, attr, -1);
		if (a)
			return a;
		if (strcmp(attr, "heading") == 0)
			return "<bold,underline>  Document             File</>";
		if (strcmp(attr, "line-format") == 0)
			return "  %+name:20 %filename";
		if (strcmp(attr, "default-renderer") == 0)
			return "format";
		return NULL;
	}
	p = m->ref.p;
	if (!forward) {
		if (!p)
			p = list_last_entry(&doc->ed->root.focus->children,
					    struct pane, siblings);
		else if (p != list_first_entry(&doc->ed->root.focus->children,
					       struct pane, siblings))
			p = list_prev_entry(p, siblings);
		else
			p = NULL;
	}
	if (!p)
		return NULL;
	d = p->data;
	if (strcmp(attr, "name") == 0)
		return d->name;
	return doc_attr(d, NULL, 0, attr);
}

DEF_CMD(docs_get_attr)
{
	struct doc *doc = ci->home->data;
	struct mark *m = ci->mark;
	bool forward = ci->numeric != 0;
	char *attr = ci->str;
	ci->str2 = __docs_get_attr(doc, m, forward, attr);
	return 1;
}

DEF_CMD(docs_open)
{
	struct pane *p = ci->home;
	struct pane *dp = p->point->m.ref.p;
	struct pane *par = p->parent;
	char *renderer = NULL;

	/* close this pane, open the given document. */
	if (dp == NULL)
		return 0;

	if (strcmp(ci->key, "Chr-h") == 0)
		renderer = "hex";

	if (strcmp(ci->key, "Chr-o") == 0) {
		struct cmd_info ci2 = {0};
		ci2.key = "OtherPane";
		ci2.focus = ci->focus;
		if (key_handle_focus(&ci2)) {
			par = ci2.focus;
			p = pane_child(par);
		}
	}
	if (p)
		pane_close(p);
	p = doc_attach_view(par, dp, renderer);
	if (p) {
		pane_focus(p);
		return 1;
	} else {
		return 0;
	}
}

DEF_CMD(docs_bury)
{
	struct doc *d = (*ci->pointp)->doc;

	doc_destroy(d);
	return 1;
}

void doc_make_docs(struct editor *ed)
{
	struct docs *ds = malloc(sizeof(*ds));
	struct map *docs_map = key_alloc();

	doc_init(&ds->doc);
	ds->doc.ed = ed;
	doc_set_name(&ds->doc, "*Documents*");
	ed->docs = &ds->doc;

	key_add(docs_map, "Chr-f", &docs_open);
	key_add(docs_map, "Chr-h", &docs_open);
	key_add(docs_map, "Return", &docs_open);
	key_add(docs_map, "Chr-o", &docs_open);
	key_add(docs_map, "Chr-q", &docs_bury);

	key_add(docs_map, "doc:set-ref", &docs_set_ref);
	key_add(docs_map, "doc:get-attr", &docs_get_attr);
	key_add(docs_map, "doc:mark-same", &docs_mark_same);
	key_add(docs_map, "doc:step", &docs_step);

	ds->doc.map = docs_map;
	doc_attach(ed->root.focus, &ds->doc);
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
		if (m->ref.p == d->home) {
			mark_step2(ed->docs, m, 1, 1);
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
	struct pane *p = d->home;

	if (p->siblings.next == &ed->root.focus->children)
		/* At the end, nothing to do */
		return;
	for (m = doc_first_mark_all(ed->docs);
	     m;
	     m = doc_next_mark_all(ed->docs, m))
		if (p->siblings.next == &m->ref.p->siblings) {
			mark_step2(ed->docs, m, 0, 1);
			doc_notify_change(ed->docs, m);
		}
}

void doc_promote(struct doc *d)
{
	struct pane *p = d->home;
	docs_release(d);
	list_move(&p->siblings, &d->ed->root.focus->children);
	docs_attach(d);
}

int  doc_destroy(struct doc *d)
{
	/* If there are no views on the document, then unlink from
	 * the documents list and destroy it.
	 */
	int i;
	struct cmd_info ci2 = {0};
	struct point p, *pt;

	d->deleting = 1;
	if (d == d->ed->docs)
		d->deleting = 2; /* tell editor choose doc that this
				  * is available if absolutely needed */
	doc_close_views(d);
	d->deleting = 0;

	for (i = 0; i < d->nviews; i++)
		if (d->views[i].notify)
			/* still in use */
			return 0;
	if (d == d->ed->docs)
		return 0;

	docs_release(d);
	pane_close(d->home);

	ci2.key = "doc:destroy";
	/* Hack ... will go */
	p.doc = d;
	pt = &p;
	ci2.pointp = &pt;
	key_lookup(d->map, &ci2);

	free(d->views);
	attr_free(&d->attrs);
	free(d->name);
	while (d->marks.first) {
		struct mark *m = hlist_first_entry(&d->marks, struct mark, all);
		if (m->viewnum == MARK_POINT)
			point_free(container_of(m, struct point, m));
		else if (m->viewnum == MARK_UNGROUPED)
			mark_free(m);
		else
			/* vmarks should have gone already */
			ASSERT(0);
	}
	free(d);
	return 1;
}
