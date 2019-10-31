/*
 * Copyright Neil Brown ©2016-2017-2019 <neil@brown.name>
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
 * This pane receives Notify:doc:revisit notification  from the
 * individual documents, and also requests notification of
 * Notify:doc:status-changed.
 *
 * Supported global operations include:
 * docs:byname - report pane with given (str)name
 * docs:byfd - find a document given a path and file-descriptor.
 * Each document is asked whether it matches the path and/or fd.
 * docs:choose - choose and return a document which is not currently displayed
 * somewhere.
 * docs:save-all - each each document to save itself
 * docs:show-modified - display a pane, in given window, listing just the
 * documents that are modified and might need saving.
 * Pane auto-closes when empty.
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

static struct map *docs_map, *docs_aux_map, *docs_modified_map;
DEF_LOOKUP_CMD(docs_handle, docs_map);
DEF_LOOKUP_CMD(docs_aux, docs_aux_map);
DEF_LOOKUP_CMD(docs_modified_handle, docs_modified_map);

struct docs {
	struct doc		doc;
	struct command		callback;
	struct pane		*collection safe;
	struct pane		*rendering;
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
			pane_notify("Notify:doc:Replace", doc->doc.home, 0, m);
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
			pane_notify("Notify:doc:Replace", doc->doc.home, 0, m);
		}
}

static int doc_save(struct pane *p safe, struct pane *focus safe, int test)
{
	char *fn = pane_attr_get(p, "filename");
	char *mod = pane_attr_get(p, "doc-modified");
	if (!fn || !*fn)
		call("Message", focus, 0, NULL,
		     "File has no filename - cannot be saved.");
	else if (!mod || strcmp(mod, "yes") != 0)
		call("Message", focus, 0, NULL,
		     "File not modified - no need to save.");
	else if (test)
		return 1;
	else
		home_call(p, "doc:save-file", focus);
	return 0;
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
			if (d != d2 && d2->name &&
			    strcmp(nname, d2->name) == 0) {
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

	mod = pane_mark_attr(p, m, "doc-modified");
	if (!mod || strcmp(mod, "yes") != 0)
		return 0;
	fn = pane_mark_attr(p, m, "filename");
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


DEF_CMD(docs_modified_replace)
{
	if (ci->str &&
	    strchr("sk%", ci->str[0]) != NULL)
		return Efallthrough;
	/* Suppress all others */
	return 1;
}

DEF_CMD(docs_modified_notify_replace)
{
	int all_gone;
	struct mark *m;

	if (!ci->home->parent)
		return Efail;
	m = vmark_new(ci->home->parent, MARK_UNGROUPED, NULL);
	if (!m)
		return Efail;
	mark_to_modified(ci->home->parent, m);
	all_gone = (m->ref.p == NULL);
	if (!all_gone && ci->mark) {
		/* Need to send Notify:clip to ensure mark is
		 * no longer visible */
		struct mark *m2;
		m2 = vmark_new(ci->home->parent, MARK_UNGROUPED, NULL);
		while (m2 && m2->ref.p != NULL) {
			if (mark_ordered_or_same(m2, ci->mark) &&
			    mark_ordered_or_same(ci->mark, m))
				/* FIXME I really should wait for Refresh:view
				 * and then send Notify:clip to the focus,
				 * but that seems clumsy, and this works...
				 * for now.
				 */
				call("Notify:clip", ci->home
				     , 0, m2, NULL, 0, m);
			mark_to_mark(m2, m);
			if (mark_next_pane(ci->home->parent, m) == WEOF)
				break;
			mark_to_modified(ci->home->parent, m);
		}
		mark_free(m2);
	}
	mark_free(m);
	if (ci->mark)
		pane_damaged(ci->home, DAMAGED_VIEW);
	if (all_gone)
		call("popup:close", ci->home);
	return 1;
}

DEF_CMD(docs_modified_set_ref)
{
	struct doc *dc = ci->home->data;
	struct docs *d = container_of(dc, struct docs, doc);
	struct mark *m = ci->mark;

	if (!m)
		return Enoarg;

	if (ci->num == 1 && !list_empty(&d->collection->children)) {
		m->ref.p = list_first_entry(&d->collection->children,
					    struct pane, siblings);
		mark_to_modified(ci->home, m);
	} else
		m->ref.p = NULL;

	m->ref.ignore = 0;
	mark_to_end(dc, m, ci->num != 1);
	return 1;
}

DEF_CMD(docs_modified_step)
{
	/* Only permit stepping to a document that is modified and
	 * has a file name
	 */
	wint_t ch, ret;
	if (!ci->home->parent || !ci->mark)
		return Enoarg;

	if (ci->num) {
		ret = doc_following_pane(ci->home->parent, ci->mark);
		if (ci->num2 && ret != WEOF) {
			mark_next_pane(ci->home->parent, ci->mark);
			mark_to_modified(ci->home->parent, ci->mark);
		}
	} else {
		struct mark *m = mark_dup(ci->mark);
		ch = prev_modified(ci->home->parent, m);
		if (ch == WEOF)
			ret = ch;
		else {
			if (ci->num2)
				mark_to_mark(ci->mark, m);
			ret = mark_next_pane(ci->home->parent, m);
		}
		mark_free(m);
	}
	return ret;
}

DEF_CMD(docs_modified_doc_get_attr)
{
	char *attr;
	struct mark *m;

	if (!ci->str || !ci->mark || !ci->home->parent)
		return Enoarg;
	m = mark_dup(ci->mark);
	attr = pane_mark_attr(ci->home->parent, m, ci->str);
	mark_free(m);
	comm_call(ci->comm2, "callback:get_attr", ci->focus, 0, NULL, attr);
	return 1;
}

DEF_CMD(docs_modified_get_attr)
{
	if (ci->str &&  strcmp(ci->str, "doc-name") == 0)
		return comm_call(ci->comm2, "callback:get_attr", ci->focus,
				 0, NULL, "*Modified Documents*");

	return Efallthrough;
}

DEF_CMD(docs_callback)
{
	struct docs *doc = container_of(ci->comm, struct docs, callback);
	struct pane *p;

	if (strcmp(ci->key, "docs:byname") == 0) {
		if (ci->str == NULL || strcmp(ci->str, "*Documents*") == 0)
			return comm_call(ci->comm2, "callback:doc",
					 doc->doc.home);

		list_for_each_entry(p, &doc->collection->children, siblings) {
			struct doc *dc = p->data;
			char *n = dc->name;
			if (n && strcmp(ci->str, n) == 0)
				return comm_call(ci->comm2, "callback:doc", p);
		}
		return Efail;
	}
	if (strcmp(ci->key, "docs:byfd") == 0) {
		list_for_each_entry(p, &doc->collection->children, siblings) {
			if (call("doc:same-file", p, 0, NULL, ci->str,
				 ci->num2) > 0)
				return comm_call(ci->comm2, "callback:doc", p);
		}
		return Efail;
	}
	if (strcmp(ci->key, "docs:byeach") == 0) {
		list_for_each_entry(p, &doc->collection->children, siblings) {
			int r;
			r = comm_call(ci->comm2, "callback:doc", p);
			if (r)
				return r;
		}
		return 1;
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
		return comm_call(ci->comm2, "callback:doc", choice);
	}

	if (strcmp(ci->key, "docs:save-all") == 0) {
		list_for_each_entry(p, &doc->collection->children, siblings)
			if (doc_save(p, p, ci->num2) > 0)
				/* Something needs to be saved */
				return 2;
		return 1;
	}

	if (strcmp(ci->key, "docs:show-modified") == 0) {
		p = home_call_ret(pane, doc->doc.home, "doc:attach-view",
				  ci->focus,
		                  ci->num, NULL, "modified");
		if (!p)
			return Efail;
		return comm_call(ci->comm2, "callback:doc", p);
	}

	if (strcmp(ci->key, "doc:appeared-docs-register") == 0) {
		/* Always return Efallthrough so other handlers get a chance */
		p = ci->focus;
		if (!p)
			return Efallthrough;
		if (p->parent && p->parent->parent)
			/* This has a parent which is not the root,
			 * so we shouldn't interfere.
			 */
			return Efallthrough;
		if (p == doc->doc.home)
			/* The docs doc is attached separately */
			return Efallthrough;
		pane_reparent(p, doc->collection);
		home_call(p, "doc:Request:Notify:doc:revisit", doc->collection);
		home_call(p, "doc:Request:Notify:doc:status-changed",
			  doc->collection);
		if (p->parent)
			doc_checkname(p, doc, ci->num ?: -1);
		return Efallthrough;
	}
	return Efallthrough;
}

DEF_CMD(doc_damage)
{
	struct pane *p = ci->home;
	struct doc *d = p->data;
	struct mark *m = vmark_new(d->home, MARK_UNGROUPED, NULL);
	struct pane *child = ci->focus;

	if (!child || !m)
		return Enoarg;
	do {
		if (m->ref.p == child) {
			pane_notify("Notify:doc:Replace", d->home, 0, m);
			break;
		}
	} while (mark_next(d, m) != WEOF);
	mark_free(m);
	return 1;
}

DEF_CMD(doc_revisit)
{
	struct pane *p = ci->focus;
	struct docs *docs = container_of(ci->home->data, struct docs, doc);

	if (!p)
		return Einval;
	if (p->parent != docs->collection)
		return Efallthrough;
	if (p == ci->home)
		return 1;
	doc_checkname(p, docs, ci->num);
	return 1;
}

DEF_CMD(docs_step)
{
	struct doc *doc = ci->home->data;
	struct mark *m = ci->mark;
	struct mark *m2, *target = m;
	bool forward = ci->num;
	bool move = ci->num2;
	int ret;
	struct pane *p, *next;
	struct docs *d = container_of(doc, struct docs, doc);

	if (!m)
		return Enoarg;

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
		ret = '\n';

	return CHAR_RET(ret);
}

DEF_CMD(docs_set_ref)
{
	struct doc *dc = ci->home->data;
	struct docs *d = container_of(dc, struct docs, doc);
	struct mark *m = ci->mark;

	if (!m)
		return Enoarg;

	if (ci->num == 1 && !list_empty(&d->collection->children))
		m->ref.p = list_first_entry(&d->collection->children,
					    struct pane, siblings);
	else
		m->ref.p = NULL;

	m->ref.ignore = 0;
	mark_to_end(dc, m, ci->num != 1);
	return 1;
}

static char *__docs_get_attr(struct doc *doc safe, struct mark *m safe,
			     char *attr safe)
{
	struct pane *p;

	p = m->ref.p;
	if (!p)
		return NULL;

	return pane_attr_get(p, attr);
}

DEF_CMD(docs_doc_get_attr)
{
	struct doc *d = ci->home->data;
	struct mark *m = ci->mark;
	char *attr = ci->str;
	char *val;

	if (!m || !attr)
		return Enoarg;

	val = __docs_get_attr(d, m, attr);

	if (!val)
		return Efallthrough;
	comm_call(ci->comm2, "callback:get_attr", ci->focus, 0, NULL, val);
	return 1;
}

DEF_CMD(docs_get_attr)
{
	char *attr = ci->str;
	char *val;
	struct doc *d = ci->home->data;

	if (!attr)
		return Enoarg;

	if ((val = attr_find(d->home->attrs, attr)) != NULL)
		;
	else if (strcmp(attr, "heading") == 0)
		val = "<bold,underline> Mod Document             File</>";
	else if (strcmp(attr, "line-format") == 0)
		val = " %doc-modified:3 %+doc-name:20 %.filename";
	else if (strcmp(attr, "render-default") == 0)
		val = "format";
	else if (strcmp(attr, "doc-type") == 0)
		val = "docs";
	else
		return Efallthrough;

	comm_call(ci->comm2, "callback:get_attr", ci->focus,
		  0, NULL, val);
	return 1;
}

static int docs_open(struct pane *home safe, struct pane *focus safe,
		     struct mark *m, char cmd)
{
	struct pane *p;
	struct pane *dp;
	struct pane *par;

	if (!m)
		return Enoarg;
	dp = m->ref.p;
	/* close this pane, open the given document. */
	if (dp == NULL)
		return 0;

	if (cmd == 'o') {
		par = home_call_ret(pane, focus, "DocPane", dp);
		if (!par)
			par = call_ret(pane, "OtherPane", focus);
	} else
		par = call_ret(pane, "ThisPane", focus);
	if (!par)
		return Efail;
	p = home_call_ret(pane, dp, "doc:attach-view", par, 1);
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
	char *viewer = NULL;
	struct pane *par;
	char buf[100];

	if (!m)
		return Enoarg;
	dp = m->ref.p;
	/* close this pane, open the given document. */
	if (dp == NULL)
		return 0;

	snprintf(buf, sizeof(buf), "render-Chr-%c", cmd);
	renderer = pane_attr_get(dp, buf);
	snprintf(buf, sizeof(buf), "view-Chr-%c", cmd);
	viewer = pane_attr_get(dp, buf);
	if (!renderer && !viewer)
		return Efail;

	par = call_ret(pane, "ThisPane", focus);
	if (!par)
		return Efail;
	p = home_call_ret(pane, dp, "doc:attach-view", par, 1, NULL, buf+5);
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
	tile = call_ret(pane, "ThisPane", focus);
	if (!tile)
		return 1;
	/* Discourage this doc from being chosen again */
	call("doc:Notify:doc:revisit", focus, -1);
	doc = call_ret(pane, "docs:choose", focus);
	if (doc)
		home_call(doc, "doc:attach-view", tile);
	return 1;
}

static int docs_save(struct pane *focus safe, struct mark *m)
{
	struct pane *dp;

	if (!m)
		return Enoarg;
	dp = m->ref.p;
	if (!dp)
		return 0;
	doc_save(dp, focus, 0);
	return 1;
}

static int docs_kill(struct pane *focus safe, struct mark *m, int num)
{
	struct pane *dp;
	char *mod;

	if (!m)
		return Enoarg;
	dp = m->ref.p;
	if (!dp)
		return 0;
	mod = pane_attr_get(dp, "doc-modified");
	if (mod && strcmp(mod, "yes") == 0 &&
	    num == NO_NUMERIC) {
		call("Message", focus, 0, NULL,
		     "File modified, cannot kill.");
		return 1;
	}
	call("doc:destroy", dp);
	return 1;
}

static int docs_toggle(struct pane *focus safe, struct mark *m)
{
	struct pane *dp;
	if (!m)
		return Enoarg;
	dp = m->ref.p;
	if (dp)
		return call("doc:modified", dp);
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

	docs_demark(docs, ci->focus);
	return 1;
}

DEF_CMD(docs_cmd)
{
	char cmd;

	if (!ci->str)
		return Enoarg;
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
		return docs_kill(ci->focus, ci->mark, ci->num);
	case '%':
		return docs_toggle(ci->focus, ci->mark);
	default:
		if (cmd >= 'A' && cmd <= 'Z')
			return docs_open_alt(ci->home,
					     ci->focus, ci->mark, cmd);
		return 1;
	}
}

DEF_CMD(docs_attach)
{
	struct doc *d = ci->home->data;
	struct docs *docs = container_of(d, struct docs, doc);
	char *type = ci->str ?: "default";
	struct pane *p;

	if (strcmp(type, "invisible") == 0 ||
	    ci->num == (int)(unsigned long)docs_attach_func)
		/* use default core-doc implementation */
		return Efallthrough;

	if (strcmp(type, "complete") == 0) {
		p = home_call_ret(pane, ci->home, "doc:attach-view", ci->focus,
		                  0, NULL, "invisible");
		if (p)
			p = call_ret(pane, "attach-view", p);
		if (p)
			p = call_ret(pane, "attach-render-format", p);
		if (p) {
			attr_set_str(&p->attrs, "line-format", "%+doc-name");
			attr_set_str(&p->attrs, "heading", "");
			attr_set_str(&p->attrs, "done-key", "Replace");
			p = call_ret(pane, "attach-render-complete", p);
		}
		if (p)
			return comm_call(ci->comm2, "callback:doc", p);
		return Efail;
	}
	if (strcmp(type, "modified") == 0) {
		p = home_call_ret(pane, ci->home, "doc:attach-view", ci->focus,
		                  0, NULL, "invisible");
		if (p)
			p = call_ret(pane, "attach-view", p);
		if (p)
			p = call_ret(pane, "attach-render-format", p);
		if (p) {
			//attr_set_str(&p->attrs, "line-format", "%+doc-name");
			//attr_set_str(&p->attrs, "heading", "");
			//attr_set_str(&p->attrs, "done-key", "Replace");
			p = pane_register(p, 0, &docs_modified_handle.c, docs);

			call("doc:Request:Notify:doc:Replace", p);
			/* And trigger Notify:doc:Replace
			 * handling immediately...*/
			pane_call(p, "Notify:doc:Replace", p);
			/* Don't want to inherit position from some
			 * earlier instance, always move to the start.
			 */
			call("Move-File", p, -1);
		}
		if (p)
			return comm_call(ci->comm2, "callback:doc", p);
		return Efail;
	}
	/* any other type gets the default handling for the rendering */
	p = docs->rendering;
	if (!p) {
		/* now is a good time to create the rendering doc */
		p = call_ret(pane, "attach-render-format", docs->doc.home, 1);
		if (p)
			p = call_ret(pane, "attach-doc-rendering", p);
		if (p)
			pane_add_notify(docs->doc.home, p, "Notify:Close");
		docs->rendering = p;
	}

	if (!p || p->damaged & DAMAGED_CLOSED)
		p = ci->home;
	return home_call(p, ci->key, ci->focus,
	                 (int)(unsigned long)docs_attach_func, NULL, ci->str,
	                 0, NULL, NULL,
	                 0, 0, ci->comm2);
}

DEF_CMD(docs_notify_close)
{
	struct doc *d = ci->home->data;
	struct docs *docs = container_of(d, struct docs, doc);

	if (ci->focus == docs->rendering)
		docs->rendering = NULL;

	return 1;
}

static void docs_init_map(void)
{
	if (docs_map)
		return;
	docs_map = key_alloc();
	docs_aux_map = key_alloc();
	docs_modified_map = key_alloc();
	/* A "docs" document provides services to children and also behaves as
	 * a document which lists those children
	 */
	key_add_chain(docs_map, doc_default_cmd);
	key_add(docs_map, "doc:set-ref", &docs_set_ref);
	key_add(docs_map, "doc:get-attr", &docs_doc_get_attr);
	key_add(docs_map, "doc:step", &docs_step);
	key_add(docs_map, "doc:destroy", &docs_destroy);
	key_add(docs_map, "doc:replace", &docs_cmd);
	key_add(docs_map, "doc:attach-view", &docs_attach);
	key_add(docs_map, "Notify:Close", &docs_notify_close);

	key_add(docs_map, "get-attr", &docs_get_attr);

	key_add(docs_aux_map, "Notify:doc:revisit", &doc_revisit);
	key_add(docs_aux_map, "Notify:doc:status-changed", &doc_damage);
	key_add(docs_aux_map, "ChildClosed", &docs_child_closed);

	key_add(docs_modified_map, "doc:replace", &docs_modified_replace);
	key_add(docs_modified_map, "Notify:doc:Replace",
		&docs_modified_notify_replace);
	key_add(docs_modified_map, "doc:step", &docs_modified_step);
	key_add(docs_modified_map, "doc:get-attr", &docs_modified_doc_get_attr);
	key_add(docs_modified_map, "doc:set-ref", &docs_modified_set_ref);
	key_add(docs_modified_map, "get-attr", &docs_modified_get_attr);
}

DEF_CMD(attach_docs)
{
	/* Attach a docs handler.  We register some commands with the editor
	 * so we can be found
	 */
	struct docs *doc = malloc(sizeof(*doc));
	struct pane *p;

	docs_init_map();

	p = doc_register(ci->home, 0, &docs_handle.c, &doc->doc);
	if (!p) {
		free(doc->doc.name);
		free(doc);
		return Efail;
	}
	doc->rendering = NULL;
	doc->doc.name = strdup("*Documents*");
	p = pane_register(ci->home, 0, &docs_aux.c, doc);
	if (!p) {
		pane_close(doc->doc.home);
		return Efail;
	}
	doc->collection = p;

	doc->callback = docs_callback;
	call_comm("global-set-command", ci->home, &doc->callback,
		  0, NULL, "docs:",
		  0, NULL, "docs;");
	call_comm("global-set-command", ci->home, &doc->callback,
		  0, NULL, "doc:appeared-docs-register");


	/* Now reparent and return the primary doc */
	p = doc->doc.home;
	pane_reparent(p, doc->collection);

	return comm_call(ci->comm2, "callback:doc", p);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &attach_docs, 0, NULL,
		  "attach-doc-docs");
}
