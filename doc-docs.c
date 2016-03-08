/*
 * Copyright Neil Brown ©2016 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Document management is eased by having a well defined collection
 * of documents.  This module provides a pane to manage that collection.
 *
 * Collected documents are attached as children of this pane.  The pane
 * itself provides a "*Documents*" document which can be used to browse
 * the document list.
 *
 * Supported operations include:
 * docs:byname - report pane with given (str)name
 * docs:byid - find pane for doc with given (str)id - ID provided by handler and
 *             typically involves device/inode information
 * docs:attach - the (focus) document is added to the list if not already there,
 *               and placed at the top, or bottom if numeric < 0
 * docs:setname - assign the (str)name to the (focus)document, but ensure it is unique.
 *
 * After a document is created and bound to a pane "docs:attach" is called
 * which adds that pane to the list.
 *
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

struct docs {
	struct doc		doc;
	struct command		callback;
};

static void docs_demark(struct docs *doc, struct pane *p)
{
	/* This document is about to be moved in the list.
	 * Any mark pointing at it is moved forward
	 */
	struct mark *m;

	for (m = doc_first_mark_all(&doc->doc);
	     m;
	     m = doc_next_mark_all(m))
		if (m->ref.p == p) {
			mark_step2(&doc->doc, m, 1, 1);
			doc_notify_change(&doc->doc, m, NULL);
		}
}

static void docs_enmark(struct docs *doc, struct pane *p)
{
	/* This document has just been added to the list.
	 * any mark pointing just past it is moved back.
	 */
	struct mark *m;

	if (p->siblings.next == &doc->doc.home->children)
		/* At the end, nothing to do */
		return;
	for (m = doc_first_mark_all(&doc->doc);
	     m;
	     m = doc_next_mark_all(m))
		if (p->siblings.next == &m->ref.p->siblings) {
			mark_step2(&doc->doc, m, 0, 1);
			doc_notify_change(&doc->doc, m, NULL);
		}
}

static void check_name(struct docs *docs, struct pane *pane)
{
	struct doc_data *dd = pane->data;
	char *nname;
	int unique = 1;
	int conflict = 1;

	if (!dd->doc->name)
		dd->doc->name = strdup("*unknown*");

	nname = malloc(strlen(dd->doc->name) + sizeof("<xxx>"));
	while (conflict && unique < 1000) {
		struct pane *p;
		conflict = 0;
		if (unique > 1)
			sprintf(nname, "%s<%d>", dd->doc->name, unique);
		else
			strcpy(nname, dd->doc->name);
		list_for_each_entry(p, &docs->doc.home->children, siblings) {
			struct doc_data *d2 = p->data;
			if (dd->doc != d2->doc && strcmp(nname, d2->doc->name) == 0) {
				conflict = 1;
				unique += 1;
				break;
			}
		}
	}
	if (unique > 1) {
		free(dd->doc->name);
		dd->doc->name = nname;
	} else
		free(nname);
}

DEF_CMD(doc_checkname)
{
	struct doc_data *dd = ci->home->data;
	struct docs *d = container_of(dd->doc, struct docs, doc);

	check_name(d, ci->focus);
	return 1;
}

DEF_CMD(docs_callback)
{
	struct docs *doc = container_of(ci->comm, struct docs, callback);
	struct pane *p;

	if (strcmp(ci->key, "docs:byname") == 0) {
		if (ci->str == NULL || strcmp(ci->str, "*Documents*") == 0)
			return comm_call(ci->comm2, "callback:doc", doc->doc.home,
					 0, NULL, NULL, 0);
		list_for_each_entry(p, &doc->doc.home->children, siblings) {
			char *n = doc_attr(p, NULL, 0, "doc:name");
			if (strcmp(ci->str, n) == 0)
				return comm_call(ci->comm2, "callback:doc", p, 0,
						 NULL, NULL, 0);
		}
		return -1;
	}
	if (strcmp(ci->key, "docs:byfd") == 0) {
		list_for_each_entry(p, &doc->doc.home->children, siblings) {
			if (call5("doc:same-file", p, 0, NULL, ci->str,
				    ci->extra) > 0)
				return comm_call(ci->comm2, "callback:doc", p, 0,
						 NULL, NULL, 0);
		}
		return -1;
	}
	if (strcmp(ci->key, "docs:choose") == 0) {
		/* Choose a documents with no views notifiees, but ignore 'deleting' */
		struct pane *choice = NULL, *last = NULL;

		list_for_each_entry(p, &doc->doc.home->children, siblings) {
			struct doc_data *dd = p->data;
			if (dd->doc->deleting)
				continue;
			last = p;
			if (list_empty(&p->notifiees)) {
				choice = p;
				break;
			}
		}
		if (!choice)
			choice = last;
		if (!choice)
			choice = doc->doc.home;
		return comm_call(ci->comm2, "callback:doc", choice, 0,
				 NULL, NULL, 0);
	}

	if (strcmp(ci->key, "docs:attach") == 0) {
		struct pane *p = ci->focus;
		if (!p)
			return -1;
		if (p == doc->doc.home)
			/* The docs doc is implicitly attached */
			return 1;
		if (p->parent != doc->doc.home)
			check_name(doc, p);
		p->parent = doc->doc.home;
		docs_demark(doc, p);
		if (ci->numeric >= 0)
			list_move(&p->siblings, &doc->doc.home->children);
		else
			list_move_tail(&p->siblings, &doc->doc.home->children);
		docs_enmark(doc, p);
	}
	return 0;
}

DEF_CMD(docs_step)
{
	struct doc_data *dd = ci->home->data;
	struct doc *doc = dd->doc;
	struct mark *m = ci->mark;
	bool forward = ci->numeric;
	bool move = ci->extra;
	int ret;
	struct pane *p = m->ref.p, *next;

	if (forward) {
		/* report on d */
		if (p == NULL || p == list_last_entry(&doc->home->children,
						      struct pane, siblings))
			next = NULL;
		else
			next = list_next_entry(p, siblings);
	} else {
		next = p;
		if (p == NULL)
			p = list_last_entry(&doc->home->children,
					    struct pane, siblings);
		else if (p == list_first_entry(&doc->home->children,
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
		ret = WEOF;
	else
		ret = ' ';
	/* return value must be +ve, so use high bits to ensure this. */
	return (ret & 0xFFFFF) | 0x100000;
}

DEF_CMD(docs_set_ref)
{
	struct doc_data *dd = ci->home->data;
	struct docs *d = container_of(dd->doc, struct docs, doc);
	struct mark *m = ci->mark;

	if (ci->numeric == 1)
		m->ref.p = list_first_entry(&d->doc.home->children,
					    struct pane, siblings);
	else
		m->ref.p = NULL;

	m->ref.ignore = 0;
	m->rpos = 0;
	return 1;
}

DEF_CMD(docs_mark_same)
{
	return ci->mark->ref.p == ci->mark2->ref.p ? 1 : 2;
}

static char *__docs_get_attr(struct doc *doc, struct mark *m,
			     bool forward, char *attr)
{
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
			p = list_last_entry(&doc->home->children,
					    struct pane, siblings);
		else if (p != list_first_entry(&doc->home->children,
					       struct pane, siblings))
			p = list_prev_entry(p, siblings);
		else
			p = NULL;
	}
	if (!p)
		return NULL;

	if (strcmp(attr, "name") == 0) {
		struct doc_data *dd = p->data;
		return dd->doc->name;
	}
	return doc_attr(p, NULL, 0, attr);
}

DEF_CMD(docs_get_attr)
{
	struct doc_data *dd = ci->home->data;
	struct mark *m = ci->mark;
	bool forward = ci->numeric != 0;
	char *attr = ci->str;
	char *val = __docs_get_attr(dd->doc, m, forward, attr);

	comm_call(ci->comm2, "callback:get_attr", ci->focus,
		  0, NULL, val, 0);
	return 1;
}

DEF_CMD(docs_open)
{
	struct pane *p = ci->home;
	struct pane *dp = ci->mark->ref.p;
	struct pane *par = p->parent;
	char *renderer = NULL;

	/* close this pane, open the given document. */
	if (dp == NULL)
		return 0;

	if (strcmp(ci->key, "Chr-h") == 0)
		renderer = "hex";

	if (strcmp(ci->key, "Chr-o") == 0) {
		struct pane *p2 = call_pane("OtherPane", ci->focus, 0, NULL, 0);
		if (p2) {
			par = p2;
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
	return call3("doc:destroy", ci->home, 0, 0);
}

DEF_CMD(docs_destroy)
{
	/* Not allowed to destroy this document */
	return -1;
}

DEF_CMD(docs_child_closed)
{
	struct doc_data *dd = ci->home->data;
	struct docs *d = container_of(dd->doc, struct docs, doc);

	docs_demark(d, ci->focus);
	return 1;
}

static struct map *docs_map;

static void docs_init_map(void)
{
	if (docs_map)
		return;
	docs_map = key_alloc();
	/* A "docs" document provides services to children and also behaves as
	 * a document which lists those children
	 */

	key_add(docs_map, "doc:set-ref", &docs_set_ref);
	key_add(docs_map, "doc:get-attr", &docs_get_attr);
	key_add(docs_map, "doc:mark-same", &docs_mark_same);
	key_add(docs_map, "doc:step", &docs_step);
	key_add(docs_map, "doc:destroy", &docs_destroy);
	key_add(docs_map, "doc:check_name", &doc_checkname);

	key_add(docs_map, "Chr-f", &docs_open);
	key_add(docs_map, "Chr-h", &docs_open);
	key_add(docs_map, "Return", &docs_open);
	key_add(docs_map, "Chr-o", &docs_open);
	key_add(docs_map, "Chr-q", &docs_bury);

	key_add(docs_map, "ChildClosed", &docs_child_closed);
}

DEF_CMD(attach_docs)
{
	/* Attach a docs handler.  We register some commands with the editor
	 * so we can be found
	 */
	struct docs *doc = malloc(sizeof(*doc));
	struct pane *p;
	struct editor *ed = pane2ed(ci->focus);

	docs_init_map();
	doc_init(&doc->doc);

	doc->doc.map = docs_map;
	doc->doc.name = strdup("*Documents*");
	p = doc_attach(ci->focus, &doc->doc);
	if (!p) {
		free(doc->doc.name);
		free(doc);
		return -1;
	}

	doc->callback = docs_callback;
	key_add_range(ed->commands, "docs:","docs;", &doc->callback);

	return comm_call(ci->comm2, "callback:doc", p, 0, NULL, NULL, 0);
}


void edlib_init(struct editor *ed)
{
	key_add(ed->commands, "doc-docs", &attach_docs);
}
