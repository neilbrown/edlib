/*
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

#include "core.h"
#include "pane.h"
#include "view.h"

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

void doc_init(struct doc *d)
{
	INIT_HLIST_HEAD(&d->marks);
	INIT_TLIST_HEAD(&d->points, 0);
	INIT_LIST_HEAD(&d->list);
	d->attrs = NULL;
	d->views = NULL;
	d->nviews = 0;
	d->name = NULL;
}

struct doctype_list {
	struct doctype		*type;
	struct list_head	lst;
};

void doc_register_type(struct editor *ed, struct doctype *type)
{
	struct doctype_list *t = malloc(sizeof(*t));
	t->type = type;
	list_add(&t->lst, &ed->doctypes);
}

struct doc *doc_new(struct editor *ed, char *type)
{
	struct doctype_list *dt;
	list_for_each_entry(dt, &ed->doctypes, lst)
		if (strcmp(dt->type->name, type) == 0) {
			struct doc *d = dt->type->new(dt->type);
			if (d)
				d->ed = ed;
			return d;
		}
	return NULL;
}

struct pane *doc_open(struct pane *parent, int fd, char *name, char *render)
{
	struct stat stb;
	struct doc *d;
	struct pane *p;

	fstat(fd, &stb);
	list_for_each_entry(d, &pane2ed(parent)->documents, list)
		if (d->ops->same_file(d, fd, &stb))
			goto found;

	if ((stb.st_mode & S_IFMT) == S_IFREG) {
		d = doc_new(pane2ed(parent), "text");
	} else if ((stb.st_mode & S_IFMT) == S_IFDIR) {
		d = doc_new(pane2ed(parent), "dir");
	} else
		return NULL;
	doc_load_file(d, NULL, fd, name);
found:
	p = view_attach(parent, d, NULL, 1);
	if (!render)
		render = d->default_render;
	render_attach(render, p, p->parent->point);
	return p;
}

struct pane *doc_from_text(struct pane *parent, char *name, char *text)
{
	bool first = 1;
	struct doc *d = doc_new(pane2ed(parent), "text");
	struct pane *p = view_attach(parent, d, NULL, 1);
	struct point *pt = p->parent->point;
	doc_set_name(d, "Error");
	doc_replace(pt, NULL, text, &first);
	point_reset(pt);
	render_attach(d->default_render, p, pt);
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

void doc_promote(struct doc *d)
{
	list_move(&d->list, &d->ed->documents);
}
