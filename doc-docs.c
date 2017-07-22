/*
 * Copyright Neil Brown ©2016-2017 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Document management is eased by having a well defined collection
 * of documents.  This module provides a pane and a document to manage
 * that collection.
 *
 * The document presents as a list of documents, called "*Documents*",
 * providing a "line-format" to guide display of each line.
 * The auxiliary pane becomes the parent of all attached documents, so
 * that the list of children is exactly the content of the document.
 * This pane responds to doc:revisit and doc:status-changed commands that
 * come down from the individual documents.
 *
 * Supported global operations include:
 * docs:byname - report pane with given (str)name
 * docs:byfd - find a document given a path and file-descriptor.  Each document is asked
 *    whether it matches the path and/or fd.
 * docs:choose - choose and return a document which is not currently displayed somewhere.
 * docs:save-all - each each document to save itself
 * docs:show-modified - display a pane, in given window, listing just the documents
 *   that are modified and might need saving.  Pane auto-closes when empty.
 *
 * After a document is created and bound to a pane "doc:appeared-*" is called
 * which adds that pane to the list if it isn't already attached somewhere else.
 * If docs sees two documents with the same name, it changes one to keep them
 * all unique.
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "safe.h"
#define PRIVATE_DOC_REF
struct doc_ref {
	struct pane	*p;
	int		ignore;
};
#include "core.h"

struct docs {
	struct doc		doc;
	struct command		callback;
	struct pane		*collection safe;
};

static void docs_demark(struct docs *doc safe, struct pane *p safe)
{
	/* This document is about to be moved in the list.
	 * Any mark pointing at it is moved forward
	 */
	struct mark *m;
	struct pane *col = doc->collection;

	for (m = doc_first_mark_all(&doc->doc);
	     m;
	     m = doc_next_mark_all(m))
		if (m->ref.p == p) {
			if (p == list_last_entry(&col->children,
						 struct pane, siblings))
				m->ref.p = NULL;
			else if (!p->parent || list_empty(&p->siblings))
				/* document is gone.  This shouldn't happen,
				 * but for safety, set doc to NULL.
				 */
				m->ref.p = NULL;
			else
				m->ref.p = list_next_entry(p, siblings);
			doc_notify_change(&doc->doc, m, NULL);
		}
}

static void docs_enmark(struct docs *doc safe, struct pane *p safe)
{
	/* This document has just been added to the list.
	 * any mark pointing just past it is moved back.
	 */
	struct mark *m;
	struct pane *next;
	struct pane *col = doc->collection;

	if (p == list_last_entry(&col->children, struct pane, siblings))
		next = NULL;
	else
		next = list_next_entry(p, siblings);

	for (m = doc_first_mark_all(&doc->doc);
	     m;
	     m = doc_next_mark_all(m))
		if (m->ref.p == next) {
			m->ref.p = p;
			doc_notify_change(&doc->doc, m, NULL);
		}
}

static void doc_save(struct pane *p safe, struct pane *focus safe)
{
	char *fn = pane_attr_get(p, "filename");
	char *mod = pane_attr_get(p, "doc-modified");
	if (!fn || !*fn)
		call5("Message", focus, 0, NULL,
		      "File has no filename - cannot be saved.", 0);
	else if (!mod || strcmp(mod, "yes") != 0)
		call5("Message", focus, 0, NULL,
		      "File not modified - no need to save.", 0);
	else
		call_home(p, "doc:save-file", focus, 0, NULL, NULL);
}

static void check_name(struct docs *docs safe, struct pane *pane safe)
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
		list_for_each_entry(p, &docs->collection->children, siblings) {
			struct doc *d2 = p->data;
			if (d != d2 && d2->name && strcmp(nname, d2->name) == 0) {
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

static void doc_checkname(struct pane *p safe, struct docs *ds safe, int n)
{
	ASSERT(p->parent->data == ds);
	check_name(ds, p);
	if (n) {
		docs_demark(ds, p);
		if (n > 0)
			list_move(&p->siblings, &ds->collection->children);
		else
			list_move_tail(&p->siblings, &ds->collection->children);
		docs_enmark(ds, p);
	}
}

/*
 * Interactive saving of files, particularly as happens when the editor
 * is exiting, pops up a document-list window which only display
 * documents which need saving.  The can be saved or killed, both of which
 * actions removes them from the list.  When the list is empty an event can be
 * sent back to the pane that requested the popup.
 */

static int mark_is_modified(struct pane *p safe, struct mark *m safe)
{
	char *fn, *mod;

	mod = pane_mark_attr(p, m, 1, "doc-modified");
	if (!mod || strcmp(mod, "yes") != 0)
		return 0;
	fn = pane_mark_attr(p, m, 1, "filename");
	return fn && *fn;
}

static void mark_to_modified(struct pane *p safe, struct mark *m safe)
{
	/* If 'm' isn't just before a savable document, move it forward */
	while (!mark_is_modified(p, m))
		if (mark_next_pane(p, m) == WEOF)
			break;
}

static wchar_t prev_modified(struct pane *p safe, struct mark *m safe)
{
	if (mark_prev_pane(p, m) == WEOF)
		return WEOF;
	while (!mark_is_modified(p, m))
		if (mark_prev_pane(p, m) == WEOF)
			return WEOF;

	return doc_following_pane(p, m);
}

DEF_CMD(docs_modified_handle)
{
	struct mark *m;

	/* This is a view showing the list of modified documents.
	 * home->parent is a view on the docs doc.
	 */
	if (!ci->home->parent)
		/* Should never happen */
		return -1;

	if (ci->mark)
		mark_to_modified(ci->home->parent, ci->mark);
	if (ci->mark2)
		mark_to_modified(ci->home->parent, ci->mark2);

	if (strcmp(ci->key, "doc:replace") == 0) {
		if (ci->str &&
		    strchr("sk%", ci->str[0]) != NULL)
			return 0;
		/* Suppress all others */
		return 1;
	}

	if (strcmp(ci->key, "Notify:doc:Replace") == 0) {
		int all_gone;
		m = vmark_new(ci->home->parent, MARK_UNGROUPED);
		if (!m)
			return -1;
		mark_to_modified(ci->home->parent, m);
		all_gone = (m->ref.p == NULL);
		mark_free(m);
		if (ci->mark)
			pane_damaged(ci->home, DAMAGED_VIEW);
		if (all_gone)
			call5("popup:close", ci->home, 0, NULL, NULL, 0);
		return 1;
	}

	if (strcmp(ci->key, "doc:step") == 0 &&
	    ci->mark) {
		/* Only permit stepping to a document that is modified and
		 * has a file name
		 */
		wint_t ch, ret;
		mark_to_modified(ci->home->parent, ci->mark);
		if (ci->numeric) {
			ret = doc_following_pane(ci->home->parent, ci->mark);
			if (ci->extra && ret != WEOF) {
				mark_next_pane(ci->home->parent, ci->mark);
				mark_to_modified(ci->home->parent, ci->mark);
			}
		} else {
			m = mark_dup(ci->mark, 1);
			ch = prev_modified(ci->home->parent, m);
			if (ch == WEOF)
				ret = ch;
			else {
				if (ci->extra)
					mark_to_mark(ci->mark, m);
				ret = mark_next_pane(ci->home->parent, m);
			}
			mark_free(m);
		}
		return ret;
	}
	if (strcmp(ci->key, "doc:get-attr") == 0 &&
	    ci->str && ci->mark) {
		char *attr;
		m = mark_dup(ci->mark, 1);
		mark_to_modified(ci->home->parent, m);
		if (!ci->numeric)
			prev_modified(ci->home->parent, m);
		attr = pane_mark_attr(ci->home->parent, m, 1, ci->str);
		mark_free(m);
		comm_call(ci->comm2, "callback:get_attr", ci->focus, 0, NULL, attr, 0);
		return 1;
	}
	if (strcmp(ci->key, "doc:mark-same") == 0 &&
	    ci->mark && ci->mark2) {
		struct docs *doc = ci->home->data;
		struct pane *p1 = ci->mark->ref.p;
		struct pane *p2 = ci->mark2->ref.p;
		list_for_each_entry_from(p1, &doc->collection->children, siblings) {
			char *fn = pane_attr_get(p1, "filename");
			char *mod = pane_attr_get(p1, "doc-modified");
			if (fn && *fn && mod && strcmp(mod, "yes") == 0)
				break;
		}
		list_for_each_entry_from(p2, &doc->collection->children, siblings) {
			char *fn = pane_attr_get(p2, "filename");
			char *mod = pane_attr_get(p2, "doc-modified");
			if (fn && *fn && mod && strcmp(mod, "yes") == 0)
				break;
		}
		if (p1 == p2)
			return 1;
		else
			return 2;
	}
	if (strcmp(ci->key, "get-attr") == 0 &&
	    ci->str &&
	    strcmp(ci->str, "doc-name") == 0)
		return comm_call(ci->comm2, "callback:get_attr", ci->focus,
				 0, NULL, "*Modified Documents*", 0);

	return 0;
}

DEF_CMD(docs_callback)
{
	struct docs *doc = container_of(ci->comm, struct docs, callback);
	struct pane *p;

	if (strcmp(ci->key, "docs:byname") == 0) {
		if (ci->str == NULL || strcmp(ci->str, "*Documents*") == 0)
			return comm_call(ci->comm2, "callback:doc", doc->doc.home,
					 0, NULL, NULL, 0);
		list_for_each_entry(p, &doc->collection->children, siblings) {
			struct doc *dc = p->data;
			char *n = dc->name;
			if (n && strcmp(ci->str, n) == 0)
				return comm_call(ci->comm2, "callback:doc", p, 0,
						 NULL, NULL, 0);
		}
		return -1;
	}
	if (strcmp(ci->key, "docs:byfd") == 0) {
		list_for_each_entry(p, &doc->collection->children, siblings) {
			if (call5("doc:same-file", p, 0, NULL, ci->str,
				    ci->extra) > 0)
				return comm_call(ci->comm2, "callback:doc", p, 0,
						 NULL, NULL, 0);
		}
		return -1;
	}
	if (strcmp(ci->key, "docs:choose") == 0) {
		/* Choose a documents with no notifiees or no pointer,
		 * but ignore 'deleting' */
		struct pane *choice = NULL, *last = NULL;

		list_for_each_entry(p, &doc->collection->children, siblings) {
			struct doc *d = p->data;
			if (p->damaged & DAMAGED_CLOSED)
				continue;
			last = p;
			if (list_empty(&p->notifiees)) {
				choice = p;
				break;
			}
			if (tlist_empty(&d->points)) {
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

	if (strcmp(ci->key, "docs:save-all") == 0) {
		list_for_each_entry(p, &doc->collection->children, siblings)
			doc_save(p, p);
		return 1;
	}

	if (strcmp(ci->key, "docs:show-modified") == 0) {
		p = doc_attach_view(ci->focus, doc->doc.home, NULL);
		p = pane_register(p, 0, &docs_modified_handle, doc, NULL);
		call3("Request:Notify:doc:Replace", p, 0, NULL);
		/* And trigger Notify:doc:Replace handling immediately...*/
		call3("Notify:doc:Replace", p, 0, NULL);
		return 1;
	}

	if (strcmp(ci->key, "doc:appeared-docs-register") == 0) {
		/* Always return 0 so other handlers get a chance */
		p = ci->focus;
		if (!p)
			return 0;
		if (p->parent && p->parent->parent)
			/* This has a parent which is not the root,
			 * so we shouldn't interfere.
			 */
			return 0;
		if (p == doc->doc.home)
			/* The docs doc is attached separately */
			return 0;
		call_home(p, "doc:set-parent", doc->collection,
			  0, NULL, NULL);
		if (p->parent)
			doc_checkname(p, doc, ci->numeric);
		return 0;
	}
	return 0;
}

DEF_CMD(doc_damage)
{
	struct pane *p = ci->home;
	struct mark *m = doc_new_mark(p->data, MARK_UNGROUPED);
	struct pane *child = pane_my_child(p, ci->focus);

	if (!child || !m)
		return -1;
	do {
		if (m->ref.p == child) {
			doc_notify_change(p->data, m, NULL);
			break;
		}
	} while (mark_next(p->data, m) != WEOF);
	mark_free(m);
	return 1;
}

DEF_CMD(doc_revisit)
{
	struct pane *p = pane_my_child(ci->home, ci->focus);
	struct docs *docs = container_of(ci->home->data, struct docs, doc);
	if (!p)
		return -1;
	if (p->parent != docs->collection)
		return 0;
	if (p == ci->home)
		return 1;
	doc_checkname(p, docs, ci->numeric);
	return 1;
}

DEF_CMD(docs_step)
{
	struct doc *doc = ci->home->data;
	struct mark *m = ci->mark;
	struct mark *m2, *target = m;
	bool forward = ci->numeric;
	bool move = ci->extra;
	int ret;
	struct pane *p, *next;
	struct docs *d = container_of(doc, struct docs, doc);

	if (!m)
		return -1;
	if (call3("doc:mymark", ci->home, 0, ci->mark) != 1)
		return -1;

	p = m->ref.p;
	if (forward) {
		/* report on d */
		if (p == NULL || p == list_last_entry(&d->collection->children,
						      struct pane, siblings))
			next = NULL;
		else
			next = list_next_entry(p, siblings);
		if (move)
			for (m2 = doc_next_mark_all(m);
			     m2 && (m2->ref.p == next || m2->ref.p == m->ref.p);
			     m2 = doc_next_mark_all(m2))
				target = m2;
	} else {
		next = p;
		if (list_empty(&d->collection->children))
			p = NULL;
		else if (!p)
			p = list_last_entry(&d->collection->children,
					    struct pane, siblings);
		else if (p != list_first_entry(&d->collection->children,
					       struct pane, siblings))
			p = list_prev_entry(p, siblings);
		else
			p = NULL;
		if (p)
			next = p;
		if (move)
			for (m2 = doc_prev_mark_all(m);
			     m2 && (m2->ref.p == next || m2->ref.p == m->ref.p);
			     m2 = doc_prev_mark_all(m2))
				target = m2;
	}
	if (move) {
		mark_to_mark(m, target);
		m->ref.p = next;
	}

	if (p == NULL)
		ret = WEOF;
	else
		ret = ' ';

	return CHAR_RET(ret);
}

DEF_CMD(docs_set_ref)
{
	struct doc *dc = ci->home->data;
	struct docs *d = container_of(dc, struct docs, doc);
	struct mark *m = ci->mark;

	if (!m)
		return -1;
	if (call3("doc:mymark", ci->home, 0, m) != 1)
		return -1;

	if (ci->numeric == 1 && !list_empty(&d->collection->children))
		m->ref.p = list_first_entry(&d->collection->children,
					    struct pane, siblings);
	else
		m->ref.p = NULL;

	m->ref.ignore = 0;
	mark_to_end(dc, m, ci->numeric != 1);
	return 1;
}

DEF_CMD(docs_mark_same)
{
	if (call3("doc:mymark", ci->home, 0, ci->mark) != 1)
		return -1;
	if (call3("doc:mymark", ci->home, 0, ci->mark2) != 1)
		return -1;
	if (!ci->mark || !ci->mark2)
		return -1;
	return ci->mark->ref.p == ci->mark2->ref.p ? 1 : 2;
}

static char *__docs_get_attr(struct doc *doc safe, struct mark *m safe,
			     bool forward, char *attr safe)
{
	struct pane *p;
	struct docs *d = container_of(doc, struct docs, doc);

	p = m->ref.p;
	if (!forward) {
		if (list_empty(&d->collection->children))
			p = NULL;
		else if (!p)
			p = list_last_entry(&d->collection->children,
					    struct pane, siblings);
		else if (p != list_first_entry(&d->collection->children,
					       struct pane, siblings))
			p = list_prev_entry(p, siblings);
		else
			p = NULL;
	}
	if (!p)
		return NULL;

	if (strcmp(attr, "name") == 0) {
		struct doc *d2 = p->data;
		return d2->name;
	}

	return pane_attr_get(p, attr);
}

DEF_CMD(docs_doc_get_attr)
{
	struct doc *d = ci->home->data;
	struct mark *m = ci->mark;
	bool forward = ci->numeric != 0;
	char *attr = ci->str;
	char *val;

	if (!m || !attr)
		return -1;
	if (call3("doc:mymark", ci->home, 0, ci->mark) != 1)
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
	char *attr = ci->str;
	char *val;
	struct doc *d = ci->home->data;

	if (!attr)
		return -1;

	if ((val = attr_find(d->home->attrs, attr)) != NULL)
		;
	else if (strcmp(attr, "heading") == 0)
		val = "<bold,underline> Mod Document             File</>";
	else if (strcmp(attr, "line-format") == 0)
		val = " %doc-modified:3 %+name:20 %.filename";
	else if (strcmp(attr, "render-default") == 0)
		val = "format";
	else if (strcmp(attr, "doc-type") == 0)
		val = "docs";
	else
		return 0;

	comm_call(ci->comm2, "callback:get_attr", ci->focus,
		  0, NULL, val, 0);
	return 1;
}

static int docs_open(struct pane *home safe, struct pane *focus safe,
		     struct mark *m, char cmd)
{
	struct pane *p;
	struct pane *dp;
	struct pane *par;

	if (!m)
		return -1;
	dp = m->ref.p;
	/* close this pane, open the given document. */
	if (dp == NULL)
		return 0;

	if (cmd == 'o')
		par = call_pane("OtherPane", focus, 0, NULL, 0);
	else
		par = call_pane("ThisPane", focus, 0, NULL, 0);
	if (!par)
		return -1;
	p = doc_attach_view(par, dp, NULL);
	if (p) {
		pane_focus(p);
		return 1;
	} else {
		return 0;
	}
}

static int docs_open_alt(struct pane *home safe, struct pane *focus safe,
			 struct mark *m, char cmd)
{
	struct pane *p;
	struct pane *dp;
	char *renderer = NULL;
	struct pane *par;
	char buf[100];

	if (!m)
		return -1;
	dp = m->ref.p;
	/* close this pane, open the given document. */
	if (dp == NULL)
		return 0;

	snprintf(buf, sizeof(buf), "render-Chr-%c", cmd);
	renderer = pane_attr_get(dp, buf);
	if (!renderer)
		return -1;

	par = call_pane("ThisPane", focus, 0, NULL, 0);
	if (!par)
		return -1;
	p = doc_attach_view(par, dp, renderer);
	if (p) {
		pane_focus(p);
		return 1;
	} else {
		return 0;
	}
}

static int docs_bury(struct pane *focus safe)
{
	/* If the docs list is in a tile, put something else there. */
	/* FIXME should this be a function of the pane manager? */
	struct pane *tile, *doc;
	tile = call_pane("ThisPane", focus, 0, NULL, 0);
	if (!tile)
		return 1;
	/* Discourage this doc from being chosen again */
	call3("doc:revisit", focus, -1, NULL);
	doc = call_pane("docs:choose", focus, 0, NULL, 0);
	if (doc)
		doc_attach_view(tile, doc, NULL);
	return 1;
}

static int docs_save(struct pane *focus safe, struct mark *m)
{
	struct pane *dp;

	if (!m)
		return -1;
	dp = m->ref.p;
	if (!dp)
		return 0;
	doc_save(dp, focus);
	return 1;
}

static int docs_kill(struct pane *focus safe, struct mark *m, int numeric)
{
	struct pane *dp;
	char *mod;

	if (!m)
		return -1;
	dp = m->ref.p;
	if (!dp)
		return 0;
	mod = pane_attr_get(dp, "doc-modified");
	if (mod && strcmp(mod, "yes") == 0 &&
	    numeric == NO_NUMERIC) {
		call5("Message", focus, 0, NULL,
		      "File modified, cannot kill.", 0);
		return 1;
	}
	call3("doc:destroy", dp, 0, NULL);
	return 1;
}

static int docs_toggle(struct pane *focus safe, struct mark *m)
{
	struct pane *dp;
	if (!m)
		return -1;
	dp = m->ref.p;
	if (dp)
		return call3("doc:modified", dp, 0, NULL);
	return 0;
}

DEF_CMD(docs_destroy)
{
	/* Not allowed to destroy this document
	 * So handle command here, so we don't get
	 * to the default handler
	 */
	return 1;
}

DEF_CMD(docs_child_closed)
{
	struct doc *d = ci->home->data;
	struct docs *docs = container_of(d, struct docs, doc);
	struct pane *child = pane_my_child(ci->home, ci->focus);
	if (!child)
		return -1;
	docs_demark(docs, child);
	return 1;
}

DEF_CMD(docs_cmd)
{
	char cmd;

	if (!ci->str)
		return -1;
	if (call3("doc:mymark", ci->home, 0, ci->mark) != 1)
		return -1;
	cmd = ci->str[0];
	switch(cmd) {
	case 'f':
	case '\n':
	case 'o':
		return docs_open(ci->home, ci->focus, ci->mark, cmd);
	case 'q':
		return docs_bury(ci->focus);
	case 's':
		return docs_save(ci->focus, ci->mark);
	case 'k':
		return docs_kill(ci->focus, ci->mark, ci->numeric);
	case '%':
		return docs_toggle(ci->focus, ci->mark);
	default:
		if (cmd >= 'A' && cmd <= 'Z')
			return docs_open_alt(ci->home, ci->focus, ci->mark, cmd);
		return 1;
	}
}

static struct map *docs_map, *docs_aux_map;

static void docs_init_map(void)
{
	if (docs_map)
		return;
	docs_map = key_alloc();
	docs_aux_map = key_alloc();
	/* A "docs" document provides services to children and also behaves as
	 * a document which lists those children
	 */

	key_add(docs_map, "doc:set-ref", &docs_set_ref);
	key_add(docs_map, "doc:get-attr", &docs_doc_get_attr);
	key_add(docs_map, "doc:mark-same", &docs_mark_same);
	key_add(docs_map, "doc:step", &docs_step);
	key_add(docs_map, "doc:destroy", &docs_destroy);
	key_add(docs_map, "doc:replace", &docs_cmd);

	key_add(docs_map, "get-attr", &docs_get_attr);

	key_add(docs_aux_map, "doc:revisit", &doc_revisit);
	key_add(docs_aux_map, "doc:status-changed", &doc_damage);
	key_add(docs_aux_map, "ChildClosed", &docs_child_closed);
}

DEF_LOOKUP_CMD_DFLT(docs_handle, docs_map, doc_default_cmd);
DEF_LOOKUP_CMD(docs_aux, docs_aux_map);

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
	p = pane_register(ci->home, 0, &docs_handle.c, &doc->doc, NULL);
	if (!p) {
		free(doc->doc.name);
		free(doc);
		return -1;
	}
	doc->doc.home = p;
	p = pane_register(ci->home, 0, &docs_aux.c, doc, NULL);
	if (!p) {
		pane_close(doc->doc.home);
		return -1;
	}
	doc->collection = p;

	doc->callback = docs_callback;
	call_comm7("global-set-command", ci->home, 0, NULL, "docs:", 0, "docs;",
		   &doc->callback);
	call_comm("global-set-command", ci->home, 0, NULL,
		  "doc:appeared-docs-register", 0, &doc->callback);

	call_home(p, "doc:set-parent", doc->collection,
		  0, NULL, NULL);

	return comm_call(ci->comm2, "callback:doc", doc->doc.home, 0, NULL, NULL, 0);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, 0, NULL, "attach-doc-docs",
		  0, &attach_docs);
}
