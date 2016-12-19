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

static struct pane *doc_assign(struct pane *p safe, int, char *);
struct pane *doc_attach(struct pane *parent, struct pane *d);

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
	d->autoclose = 0;
	d->home = safe_cast NULL;
}

/* For these 'default commands', home->data is struct doc */
DEF_CMD(doc_char)
{
	struct doc *d = ci->home->data;
	int rpt = RPT_NUM(ci);

	if (!ci->mark)
		return -1;

	while (rpt > 0) {
		if (mark_next(d, ci->mark) == WEOF)
			break;
		rpt -= 1;
	}
	while (rpt < 0) {
		if (mark_prev(d, ci->mark) == WEOF)
			break;
		rpt += 1;
	}

	return 1;
}

DEF_CMD(doc_word)
{
	struct doc *d = ci->home->data;
	int rpt = RPT_NUM(ci);

	if (!ci->mark)
		return -1;

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
	struct doc *d = ci->home->data;
	int rpt = RPT_NUM(ci);

	if (!ci->mark)
		return -1;

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
	struct doc *d = ci->home->data;
	wint_t ch = 1;
	int rpt = RPT_NUM(ci);

	if (!ci->mark)
		return -1;

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
	struct doc *d = ci->home->data;
	int rpt = RPT_NUM(ci);
	struct mark *m = ci->mark;

	if (!ci->mark)
		return -1;

	if (rpt > 0)
		mark_reset(d, m, 1);
	if (rpt < 0)
		mark_reset(d, m, 0);

	return 1;
}

DEF_CMD(doc_line)
{
	struct doc *d = ci->home->data;
	wint_t ch = 1;
	int rpt = RPT_NUM(ci);

	if (!ci->mark)
		return -1;

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
	struct doc *d = ci->home->data;
	wint_t ch = 1;
	int rpt = RPT_NUM(ci);

	if (!ci->mark)
		return -1;

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

DEF_CMD(doc_attr_set)
{
	struct doc *d = ci->home->data;

	if (!ci->str)
		return -1;

	if (ci->str2 == NULL && ci->extra == 1)
		attr_set_int(&d->home->attrs, ci->str, ci->numeric);
	else
		attr_set_str(&d->home->attrs, ci->str, ci->str2);
	return 1;
}

DEF_CMD(doc_get_attr)
{
	struct pane *p = ci->home; struct doc *d = p->data;
	char *a;

	if (!ci->str)
		return -1;

	if ((a = attr_find(d->home->attrs, ci->str)) != NULL)
		;
	else if (strcmp(ci->str, "doc-name") == 0)
		a = d->name;
	else if (strcmp(ci->str, "doc-modified") == 0)
		a = "no";
	if (a)
		return comm_call(ci->comm2, "callback:get_attr", ci->focus, 0,
				 NULL, a, 0);
	/* One a get-attr request reaches a document, it needs to stop there,
	 * as parents might have a different idea about attributes, and about marks
	 */
	return 1;
}

DEF_CMD(doc_set_name)
{
	struct doc *d = ci->home->data;

	if (!ci->str)
		return -1;
	free(d->name);
	d->name = strdup(ci->str);
	return call3("doc:check_name", d->home, -1, NULL);
}

DEF_CMD(doc_set_parent)
{
	if (ci->focus != ci->home->parent) {
		list_move(&ci->home->siblings, &ci->focus->children);
		ci->home->parent = ci->focus;
	}
	call3("doc:check_name", ci->home, ci->numeric, NULL);

	return 1;
}

DEF_CMD(doc_request_notify)
{
	pane_add_notify(ci->focus, ci->home, ci->key+8);
	return 1;
}

DEF_CMD(doc_notify)
{
	return pane_notify(ci->home, ci->key, ci->mark, ci->mark2,
			   ci->str, ci->numeric, ci->comm2);
}

DEF_CMD(doc_delview)
{
	if (ci->numeric >= 0)
		do_doc_del_view(ci->home->data, ci->numeric);
	else
		return -1;
	return 1;
}

DEF_CMD(doc_addview)
{
	return 1 + do_doc_add_view(ci->home->data);
}

DEF_CMD(doc_vmarkget)
{
	struct mark *m, *m2;
	m = do_vmark_first(ci->home->data, ci->numeric);
	m2 = do_vmark_last(ci->home->data, ci->numeric);
	if (ci->extra == 1 && ci->mark)
		m2 = do_vmark_at_point(ci->focus, ci->home->data, ci->mark,
				       ci->numeric);
	if (ci->extra == 2)
		m2 = doc_new_mark(ci->home->data, ci->numeric);
	if (ci->extra == 3 && ci->mark)
		m2 = do_vmark_at_or_before(ci->focus, ci->home->data, ci->mark, ci->numeric);
	return comm_call7(ci->comm2, "callback:vmark", ci->focus,
			  0, m, NULL, 0, NULL, m2);
}

DEF_CMD(doc_drop_cache)
{
	struct pane *p = ci->home;
	struct doc *d = p->data;

	if (d->autoclose)
		pane_close(p);
	return 1;
}

DEF_CMD(doc_do_closed)
{
	struct pane *p = ci->home;
	int ret;

	/* If there are any doc-displays open, then will return '1' and
	 * we will know not to destroy document yet.
	 */
	ret = pane_notify(p, "Notify:Close:request", NULL, NULL, NULL, 0, NULL);
	if (ret == 0)
		call3("doc:drop-cache", p, 0, NULL);
	return 1;
}

DEF_CMD(doc_set_autoclose)
{
	struct doc *d = ci->home->data;
	d->autoclose = ci->numeric;
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
	 * ask it's parent, which is probably a document manager
	 */
	if (!ci->home->parent)
		return -1;
	return call3(ci->key, ci->home->parent, ci->numeric, ci->mark);
}

struct map *doc_default_cmd safe;

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
	key_add(doc_default_cmd, "doc:attr-set", &doc_attr_set);
	key_add(doc_default_cmd, "doc:add-view", &doc_addview);
	key_add(doc_default_cmd, "doc:del-view", &doc_delview);
	key_add(doc_default_cmd, "doc:vmark-get", &doc_vmarkget);
	key_add(doc_default_cmd, "get-attr", &doc_get_attr);
	key_add(doc_default_cmd, "doc:set-name", &doc_set_name);
	key_add(doc_default_cmd, "doc:set-parent", &doc_set_parent);
	key_add(doc_default_cmd, "doc:autoclose", &doc_set_autoclose);
	key_add(doc_default_cmd, "doc:destroy", &doc_do_destroy);
	key_add(doc_default_cmd, "doc:revisit", &doc_do_revisit);
	key_add(doc_default_cmd, "doc:drop-cache", &doc_drop_cache);
	key_add(doc_default_cmd, "Closed", &doc_do_closed);
	key_add_range(doc_default_cmd, "Request:Notify:doc:", "Request:Notify:doc;",
		      &doc_request_notify);
	key_add_range(doc_default_cmd, "Notify:doc:", "Notify:doc;",
		      &doc_notify);
}

DEF_CMD(doc_handle)
{
	struct doc_data *dd = ci->home->data;
	struct cmd_info ci2;

	if (strcmp(ci->key, "Notify:Close:request") == 0) {
		/* The autoclose document wants to know if it should close.
		 * tell it "no" */
		return 1;
	}

	if (strcmp(ci->key, "Notify:Close") == 0) {
		/* This pane has to go away */

		pane_close(ci->home);
		return 1;
	}

	if (strcmp(ci->key, "Request:Notify:Replace") == 0) {
		pane_add_notify(ci->focus, dd->doc, "Notify:Replace");
		return 1;
	}

	if (strcmp(ci->key, "Clone") == 0) {
		struct pane *p = doc_attach(ci->focus, dd->doc);

		if (p && p->pointer && ci->home->pointer)
			point_to_mark(p->pointer, ci->home->pointer);
		pane_clone_children(ci->home, p);
		return 1;
	}

	if (strcmp(ci->key, "Close") == 0) {
		mark_free(dd->point);
		comm_call_pane(dd->doc, "Closed", ci->home, 0, NULL, NULL, 0,
			       NULL, NULL);
		free(dd);
		ci->home->data = safe_cast NULL;
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
			m = do_mark_at_point(pt, ci->extra);

		return comm_call(ci->comm2, "callback:dup-point", ci->focus,
				 0, m, NULL, 0);
	}

	if (strcmp(ci->key, "doc:assign") == 0) {
		struct pane *p2;
		if ((void*) (dd->doc))
			return -1;
		dd->doc = ci->focus;
		p2 = doc_assign(ci->home, ci->numeric, ci->str);
		if (p2)
			comm_call(ci->comm2, "callback:doc", p2, 0, NULL, NULL, 0);
		return 1;
	}

	if (strcmp(ci->key, "Replace") == 0) {
		return call_home7(dd->doc, "doc:replace", ci->focus, 1, ci->mark, ci->str,
				  ci->extra, NULL, dd->point, NULL);
	}

	if (strcmp(ci->key, "get-attr") == 0 && ci->mark == NULL) {
		char *a;
		if (!ci->str)
			return -1;
		a = pane_attr_get(dd->doc, ci->str);
		if (!a)
			return 0;
		return comm_call(ci->comm2, "callback", ci->focus, 0, NULL, a, 0);
	}

	if (strcmp(ci->key, "Move-to") == 0) {
		if (ci->mark)
			point_to_mark(dd->point, ci->mark);
		return 1;
	}

	if (strcmp(ci->key, "render:reposition") == 0)
		/* HACK: don't let any underlying pane see this as it won't
		 * mean anything, and lib-view grabs the mark which it shouldn't
		 */
		return 1;
	ci2 = *ci;
	ci2.home = dd->doc;
	ci2.comm = safe_cast NULL;

	if (ci2.mark == NULL)
		ci2.mark = dd->point;
	if (strncmp(ci->key, "doc:", 4) != 0)
		/* doesn't get sent to the doc */
		return key_lookup(doc_default_cmd, &ci2);
	return key_handle(&ci2);
}

static struct pane *doc_assign(struct pane *p safe, int numeric, char *str)
{
	struct doc_data *dd = p->data;
	struct pane *p2 = NULL;
	struct mark *m;

	m = vmark_new(dd->doc, MARK_POINT);
	if (!m)
		return NULL;
	dd->point = m;
	pane_add_notify(p, dd->doc, "Notify:Close");
	p->pointer = dd->point;
	call3("doc:revisit", dd->doc, 1, NULL);
	if (numeric) {
		p2 = call_pane("attach-view", p, 0, NULL, 0);
		if (p2)
			p2 = render_attach(str, p2);
	}
	return p2;
}

struct pane *doc_attach(struct pane *parent, struct pane *d)
{
	struct pane *p;
	struct doc_data *dd = calloc(1, sizeof(*dd));

	if (d)
		dd->doc = d;

	p = pane_register(parent, 0, &doc_handle, dd, NULL);
	/* non-home panes need to be notified so they can self-destruct */
	if (d)
		doc_assign(p, 0, NULL);
	return p;
}

struct pane *doc_new(struct pane *p safe, char *type)
{
	char buf[100];

	snprintf(buf, sizeof(buf), "attach-doc-%s", type);
	return call_pane(buf, p, 0, NULL, 0);
}

/*
 * If you have a document and want to view it, you call "doc:attach"
 * passing the attachment point as the focus.
 * Then call "doc:assign" on the resulting pane to provide the document.
 */
DEF_CMD(doc_do_attach)
{
	struct pane *p = doc_attach(ci->focus, NULL);
	if (!p)
		return -1;
	return comm_call(ci->comm2, "callback:doc", p, 0, NULL, NULL, 0);
}

DEF_CMD(doc_open)
{
	struct pane *ed = ci->home;
	int fd = ci->numeric;
	char *name = ci->str;
	struct stat stb;
	struct pane *p;
	char pathbuf[PATH_MAX], *rp = NULL;

	stb.st_mode = 0;
	if (fd < 0) {
		char *sl;
		sl = strrchr(name, '/');
		if (sl && sl-name < PATH_MAX-4 && sl[1]) {
			char nbuf[PATH_MAX];
			strncpy(nbuf, name, sl-name);
			nbuf[sl-name] = 0;
			rp = realpath(nbuf, pathbuf);
		} else if (!sl)
			rp = realpath(".", pathbuf);

		if (rp) {
			strcat(rp, "/");
			strcat(rp, sl+1);
		}
	} else if (fstat(fd, &stb) == 0)
		rp = realpath(name, pathbuf);

	if (!rp)
		rp = name;

	p = call_pane7("docs:byfd", ed, 0, NULL, fd, rp, NULL);

	if (!p) {
		p = call_pane7("global-multicall-open-doc-", ed, fd, NULL,
			       stb.st_mode & S_IFMT, rp, NULL);

		if (!p)
			return -1;
		call5("doc:load-file", p, 0, NULL, rp, fd);
		call5("global-multicall-doc:appeared-", p, 1, NULL, NULL, 0);
	}
	return comm_call(ci->comm2, "callback", p, 0, NULL,
			 NULL, 0);
}

struct pane *doc_attach_view(struct pane *parent safe, struct pane *doc safe, char *render)
{
	struct pane *p;

	p = doc_attach(parent, doc);
	if (p)
		p = call_pane("attach-view", p, 0, NULL, 0);
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

	p = doc_new(parent, "text");
	if (!p)
		return -1;
	call5("doc:set-name", p, 0, NULL, name, 0);
	call5("global-multicall-doc:appeared-", p, 1, NULL, NULL, 0);
	call7("doc:replace", p, 1, NULL, text, 1, NULL, NULL);
	return comm_call(ci->comm2, "callback", p, 0, NULL,
			 NULL, 0);
}

DEF_CMD(doc_attr_callback)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);
	cr->s = strsave(ci->focus, ci->str);
	return 1;
}

char *doc_attr(struct pane *dp safe, struct mark *m, bool forward, char *attr, int *done)
{
	struct cmd_info ci = {.key = "doc:get-attr", .home = dp, .focus = dp, .comm = safe_cast dp->handle};
	struct call_return cr;
	int ret;

	if (done)
		*done = 0;
	if (!m)
		ci.key = "get-attr";
	ci.mark = m;
	ci.numeric = forward ? 1 : 0;
	ci.str = attr;
	cr.c = doc_attr_callback;
	cr.s = NULL;
	ci.comm2 = &cr.c;
	if (!dp->handle || (ret = dp->handle->func(&ci)) == 0)
		return NULL;
	if (ret > 0 && done)
		*done = 1;
	return cr.s;
}

DEF_CMD(doc_str_callback)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);

	if (!ci->str)
		return -1;
	cr->s = strdup(ci->str);
	return 1;
}

char *doc_getstr(struct pane *from safe, struct mark *to, struct mark *m2)
{
	struct cmd_info ci = {.key = "doc:get-str", .focus = from, .home = from, .comm = safe_cast 0};
	int ret;
	struct call_return cr;

	ci.key = "doc:get-str";
	ci.focus = from;
	ci.mark = to;
	ci.mark2 = m2;
	cr.c = doc_str_callback;
	cr.s = NULL;
	ci.comm2 = &cr.c;
	ret = key_handle(&ci);
	if (!ret)
		return NULL;
	return cr.s;
}

void doc_free(struct doc *d safe)
{
	int i;

	for (i = 0; i < d->nviews; i++)
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
	call_comm("global-set-command", ed, 0, NULL, "doc:open", 0, &doc_open);
	call_comm("global-set-command", ed, 0, NULL, "doc:from-text", 0, &doc_from_text);
	call_comm("global-set-command", ed, 0, NULL, "doc:attach", 0, &doc_do_attach);
	if (!(void*)doc_default_cmd)
		init_doc_defaults();
}
