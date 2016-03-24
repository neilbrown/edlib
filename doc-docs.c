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
	struct doc *d = pane->data;
	char *nname;
	int unique = 1;
	int conflict = 1;

	if (!d->name)
		d->name = strdup("*unknown*");

	nname = malloc(strlen(d->name) + sizeof("<xxx>"));
	while (conflict && unique < 1000) {
		struct pane *p;
		conflict = 0;
		if (unique > 1)
			sprintf(nname, "%s<%d>", d->name, unique);
		else
			strcpy(nname, d->name);
		list_for_each_entry(p, &docs->doc.home->children, siblings) {
			struct doc *d2 = p->data;
			if (d != d2 && strcmp(nname, d2->name) == 0) {
				conflict = 1;
				unique += 1;
				break;
			}
		}
	}
	if (unique > 1) {
		free(d->name);
		d->name = nname;
	} else
		free(nname);
}

DEF_CMD(doc_checkname)
{
	struct doc *d = ci->home->data;
	struct docs *ds = container_of(d, struct docs, doc);

	check_name(ds, ci->focus);
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
			struct doc *dc = p->data;
			char *n = dc->name;
			if (n && strcmp(ci->str, n) == 0)
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
			struct doc *d = p->data;
			if (d->deleting)
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
	struct doc *doc = ci->home->data;
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
	struct doc *dc = ci->home->data;
	struct docs *d = container_of(dc, struct docs, doc);
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
		struct doc *d = p->data;
		return d->name;
	}
	return doc_attr(p, NULL, 0, attr);
}

DEF_CMD(docs_doc_get_attr)
{
	struct doc *d = ci->home->data;
	struct mark *m = ci->mark;
	bool forward = ci->numeric != 0;
	char *attr = ci->str;
	char *val;
	if (!m)
		return -1;

	val = __docs_get_attr(d, m, forward, attr);

	if (!val)
		return 0;
	comm_call(ci->comm2, "callback:get_attr", ci->focus,
		  0, NULL, val, 0);
	return 1;
}

DEF_CMD(docs_get_attr)
{
	struct doc *d = ci->home->data;
	char *attr = ci->str;
	char *val = attr_get_str(d->attrs, attr, -1);

	if (val)
		;
	else if (strcmp(attr, "heading") == 0)
			val = "<bold,underline>  Document             File</>";
	else if (strcmp(attr, "line-format") == 0)
			val = "  %+name:20 %filename";
	else if (strcmp(attr, "default-renderer") == 0)
			val = "format";
	else
		return 0;

	comm_call(ci->comm2, "callback:get_attr", ci->focus,
		  0, NULL, val, 0);
	return 1;
}

DEF_CMD(docs_open)
{
	struct pane *p;
	struct pane *dp = ci->mark->ref.p;
	char *renderer = NULL;
	struct pane *par;

	/* close this pane, open the given document. */
	if (dp == NULL)
		return 0;

	if (strcmp(ci->key, "Chr-h") == 0)
		renderer = "hex";

	if (strcmp(ci->key, "Chr-o") == 0)
		par = call_pane("OtherPane", ci->focus, 0, NULL, 0);
	else
		par = call_pane("ThisPane", ci->focus, 0, NULL, 0);
	if (!par)
		return -1;
	p = pane_child(par);
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
	doc_destroy(ci->home);
	return 1;
}

DEF_CMD(docs_destroy)
{
	/* Not allowed to destroy this document */
	return -1;
}

DEF_CMD(docs_child_closed)
{
	struct doc *d = ci->home->data;
	struct docs *docs = container_of(d, struct docs, doc);

	docs_demark(docs, ci->focus);
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
	key_add(docs_map, "doc:get-attr", &docs_doc_get_attr);
	key_add(docs_map, "get-attr", &docs_get_attr);
	key_add(docs_map, "doc:mark-same", &docs_mark_same);
	key_add(docs_map, "doc:step", &docs_step);
	key_add(docs_map, "doc:free", &docs_destroy);
	key_add(docs_map, "doc:check_name", &doc_checkname);

	key_add(docs_map, "Chr-f", &docs_open);
	key_add(docs_map, "Chr-h", &docs_open);
	key_add(docs_map, "Return", &docs_open);
	key_add(docs_map, "Chr-o", &docs_open);
	key_add(docs_map, "Chr-q", &docs_bury);

	key_add(docs_map, "ChildClosed", &docs_child_closed);
}

DEF_LOOKUP_CMD_DFLT(docs_handle, docs_map, doc_default_cmd);

DEF_CMD(attach_docs)
{
	/* Attach a docs handler.  We register some commands with the editor
	 * so we can be found
	 */
	struct docs *doc = malloc(sizeof(*doc));
	struct pane *p;

	docs_init_map();
	doc_init(&doc->doc);

	doc->doc.name = strdup("*Documents*");
	p = pane_register(ci->focus, 0, &docs_handle.c, &doc->doc, NULL);
	if (!p) {
		free(doc->doc.name);
		free(doc);
		return -1;
	}
	doc->doc.home = p;

	doc->callback = docs_callback;
	call_comm7("global-set-command", ci->home, 0, NULL, "docs:", 0, "docs;",
		   &doc->callback);

	return comm_call(ci->comm2, "callback:doc", p, 0, NULL, NULL, 0);
}


void edlib_init(struct pane *ed)
{
	call_comm("global-set-command", ed, 0, NULL, "attach-doc-docs",
		  0, &attach_docs);
}
