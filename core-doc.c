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
#include <wchar.h>
#include <wctype.h>
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

	if (ci->mark == NULL)
		ci->mark = ci->home->point;
	while (rpt > 0 && ch != WEOF) {
		while ((ch = mark_next(d, ci->mark)) != WEOF)
			;
		rpt = 0;
	}
	if (rpt > 0)
		__mark_reset(d, ci->mark, 0, 1);
	if (rpt < 0)
		mark_reset(d, ci->mark);

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

static struct map *doc_default_cmd;

static void init_doc_defaults(void)
{
	doc_default_cmd = key_alloc();

	key_add(doc_default_cmd, "Move-Char", &doc_char);
	key_add(doc_default_cmd, "Move-Word", &doc_word);
	key_add(doc_default_cmd, "Move-WORD", &doc_WORD);
	key_add(doc_default_cmd, "Move-EOL", &doc_eol);
	key_add(doc_default_cmd, "Move-File", &doc_file);
	key_add(doc_default_cmd, "Move-Line", &doc_line);
	key_add(doc_default_cmd, "Move-View-Large", &doc_page);
	key_add(doc_default_cmd, "Replace", &doc_do_replace);
}

DEF_CMD(doc_handle)
{
	struct doc_data *dd = ci->home->data;
	int ret;

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
		struct pane *p = doc_attach(ci->focus, dd->doc);
		struct pane *c = pane_child(ci->home);

		p->point = point_dup(ci->home->point);
		if (c)
			pane_clone(c, p);
		return 1;
	}

	if (strcmp(ci->key, "Close") == 0) {
		if (ci->home->point)
			mark_free(ci->home->point);
		ci->home->point = NULL;
		return 1;
	}

	if (strcmp(ci->key, "doc:dup-point") == 0) {
		struct mark *pt = ci->home->point;
		if (ci->mark && ci->mark->viewnum == MARK_POINT)
			pt = ci->mark;
		ci->mark = NULL;
		if (ci->home->point) {
			if (ci->extra == MARK_POINT)
				ci->mark = point_dup(pt);
			else if (ci->extra == MARK_UNGROUPED)
				ci->mark = mark_dup(pt, 1);
			else
				ci->mark = do_mark_at_point(dd->doc, pt,
							    ci->extra);
		}
		return 1;
	}

	if (strcmp(ci->key, "Move-to") == 0) {
		point_to_mark(ci->home->point, ci->mark);
		return 1;
	}

	if (strcmp(ci->key, "doc:set-name") == 0) {
		doc_set_name(dd->doc, ci->str);
		return 1;
	}

	if (strcmp(ci->key, "doc:add-view") == 0) {
		if (!ci->comm2)
			return -1;
		ci->extra = do_doc_add_view(dd->doc, ci->comm2, ci->extra);
		return 1;
	}

	if (strcmp(ci->key, "doc:del-view") == 0) {
		if (!ci->comm2)
			return -1;
		do_doc_del_view(dd->doc, ci->comm2);
		return 1;
	}

	if (strcmp(ci->key, "doc:find-view") == 0) {
		if (!ci->comm2)
			return -1;
		ci->extra = do_doc_find_view(dd->doc, ci->comm2);
		return 1;
	}

	if (strcmp(ci->key, "doc:find") == 0) {
		ci->misc = dd->doc;
		return 1;
	}

	if (strcmp(ci->key, "doc:vmark-get") == 0) {
		ci->mark = do_vmark_first(dd->doc, ci->numeric);
		ci->mark2 = do_vmark_last(dd->doc, ci->numeric);
		if (ci->extra && ci->home->point)
			ci->mark2 = do_vmark_at_point(dd->doc, ci->home->point,
						      ci->numeric);
		return 1;
	}

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
	d->ed = pane2ed(parent);
	doc_promote(d);
	return p;
}

struct doc *doc_new(struct editor *ed, char *type)
{
	char buf[100];
	struct cmd_info ci = {0};
	struct doc_data *dd;

	if (!doc_default_cmd)
		init_doc_defaults();

	sprintf(buf, "doc-%s", type);
	ci.key = buf;
	ci.focus = ci.home = &ed->root;
	if (!key_lookup(ed->commands, &ci)) {
		editor_load_module(ed, buf);
		if (!key_lookup(ed->commands, &ci))
			return NULL;
	}
	dd = ci.focus->data;
	return dd->doc;
}

struct pane *doc_open(struct editor *ed, int fd, char *name)
{
	struct stat stb;
	struct pane *p;
	struct doc *d;
	char pathbuf[PATH_MAX], *rp;

	fstat(fd, &stb);
	list_for_each_entry(p, &ed->root.focus->children, siblings) {
		struct cmd_info ci2 = {0};
		ci2.key = "doc:same-file";
		ci2.focus = p;
		ci2.extra = -1;
		ci2.misc = &stb;
		if (key_handle_focus(&ci2) > 0)
			return p;
	}

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
		p->point = point_new(dd->doc);
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

	d = doc_new(pane2ed(parent), "text");
	if (!d)
		return NULL;
	doc_set_name(d, name);
	p = doc_attach_view(parent, d->home, NULL);
	if (!p) {
		doc_destroy(d);
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
			struct doc_data *d2 = p->data;
			if (d != d2->doc && strcmp(nname, d2->doc->name) == 0) {
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
		struct doc_data *dd = p->data;
		if (strcmp(name, dd->doc->name) == 0)
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
	struct doc_data *dd = ci->home->data;
	struct doc *doc = dd->doc;
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
	struct doc_data *dd;
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
	dd = p->data;
	if (strcmp(attr, "name") == 0)
		return dd->doc->name;
	return doc_attr(p, NULL, 0, attr);
}

DEF_CMD(docs_get_attr)
{
	struct doc_data *dd = ci->home->data;
	struct mark *m = ci->mark;
	bool forward = ci->numeric != 0;
	char *attr = ci->str;
	ci->str2 = __docs_get_attr(dd->doc, m, forward, attr);
	return 1;
}

DEF_CMD(docs_open)
{
	struct pane *p = ci->home;
	struct pane *dp = p->point->ref.p;
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
	struct doc *d = doc_from_pane(ci->home);

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
	     m = doc_next_mark_all(m))
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
	     m = doc_next_mark_all(m))
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
	ci2.focus = d->home;
	key_handle_focus(&ci2);

	free(d->views);
	attr_free(&d->attrs);
	free(d->name);
	while (d->marks.first) {
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
