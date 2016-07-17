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
			if (p == list_last_entry(&doc->doc.home->children,
						 struct pane, siblings))
				m->ref.p = NULL;
			else
				m->ref.p = list_next_entry(p, siblings);
			doc_notify_change(&doc->doc, m, NULL);
		}
}

static void docs_enmark(struct docs *doc, struct pane *p)
{
	/* This document has just been added to the list.
	 * any mark pointing just past it is moved back.
	 */
	struct mark *m;
	struct pane *next;

	if (p == list_last_entry(&doc->doc.home->children, struct pane, siblings))
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

static void doc_save(struct pane *p, struct pane *focus)
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

/*
 * Interactive saving of files, particularly as happens when the editor
 * is exiting, pops up a document-list window which only display
 * documents which need saving.  The can be saved or killed, both of which
 * actions removes them from the list.  When the list is empty an event can be
 * sent back to the pane that requested the popup.
 */

static int mark_is_modified(struct pane *p, struct mark *m)
{
	char *fn, *mod;

	mod = pane_mark_attr(p, m, 1, "doc-modified");
	if (!mod || strcmp(mod, "yes") != 0)
		return 0;
	fn = pane_mark_attr(p, m, 1, "filename");
	return fn && *fn;
}

static void mark_to_modified(struct pane *p, struct mark *m)
{
	/* If 'm' isn't just before a savable document, move it forward */
	while (!mark_is_modified(p, m))
		if (mark_next_pane(p, m) == WEOF)
			break;
}

static wchar_t prev_modified(struct pane *p, struct mark *m)
{
	if (mark_prev_pane(p, m) == WEOF)
		return WEOF;
	while (mark_is_modified(p, m))
		if (mark_prev_pane(p, m) == WEOF)
			return WEOF;

	return doc_following_pane(p, m);
}

DEF_CMD(docs_modified_handle)
{
	struct mark *m;

	if (strncmp(ci->key, "Chr-", 4) == 0) {
		if (strlen(ci->key) == 5 &&
		    strchr("sk%", ci->key[4]) != NULL)
			return 0;
		/* Suppress all others */
		return 1;
	}

	if (strcmp(ci->key, "render-line") == 0 &&
	    ci->mark2) {
		/* mark2 is point - now is a good time to ensure it is
		 * on a safe place
		 */
		mark_to_modified(ci->home->parent, ci->mark2);
		return 0;
	}

	if (strcmp(ci->key, "Notify:Replace") == 0) {
		int all_gone;
		m = vmark_new(ci->home->parent, MARK_UNGROUPED);
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
	    ci->mark) {
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
		list_for_each_entry_from(p1, &doc->doc.home->children, siblings) {
			char *fn = pane_attr_get(p1, "filename");
			char *mod = pane_attr_get(p1, "doc-modified");
			if (fn && *fn && mod && strcmp(mod, "yes") == 0)
				break;
		}
		list_for_each_entry_from(p2, &doc->doc.home->children, siblings) {
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
		/* Choose a documents with no notifiees or no pointer,
		 * but ignore 'deleting' */
		struct pane *choice = NULL, *last = NULL;

		list_for_each_entry(p, &doc->doc.home->children, siblings) {
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
		list_for_each_entry(p, &doc->doc.home->children, siblings)
			doc_save(p, NULL);
		return 1;
	}

	if (strcmp(ci->key, "docs:show-modified") == 0) {
		p = doc_attach_view(ci->focus, doc->doc.home, NULL);
		p = pane_register(p, 0, &docs_modified_handle, doc, NULL);
		call3("Request:Notify:Replace", p, 0, NULL);
		/* And trigger Notify:Replace handling immediately...*/
		call3("Notify:Replace", p, 0, NULL);
		return 1;
	}

	if (strcmp(ci->key, "doc:appeared-docs-register") == 0) {
		/* Always return 0 so other handlers get a chance */
		p = ci->focus;
		if (!p)
			return 0;
		if (p == doc->doc.home)
			/* The docs doc is implicitly attached */
			return 0;
		if (p->parent != doc->doc.home)
			check_name(doc, p);
		p->parent = doc->doc.home;
		docs_demark(doc, p);
		if (ci->numeric >= 0)
			list_move(&p->siblings, &doc->doc.home->children);
		else
			list_move_tail(&p->siblings, &doc->doc.home->children);
		docs_enmark(doc, p);
		return 0;
	}
	return 0;
}

DEF_CMD(doc_damage)
{
	struct pane *p = ci->home;
	struct mark *m = doc_new_mark(p->data, MARK_UNGROUPED);
	do {
		if (m->ref.p == ci->focus) {
			doc_notify_change(p->data, m, NULL);
			break;
		}
	} while (mark_next(p->data, m) != WEOF);
	mark_free(m);
	return 1;
}

DEF_CMD(doc_revisit)
{
	struct pane *p = ci->focus;
	struct docs *docs = container_of(ci->home->data, struct docs, doc);
	if (!p)
		return -1;
	if (p->parent != ci->home)
		return 0;
	if (p == docs->doc.home)
		return 1;
	docs_demark(docs, p);
	if (ci->numeric >= 0)
		list_move(&p->siblings, &docs->doc.home->children);
	else
		list_move_tail(&p->siblings, &docs->doc.home->children);
	docs_enmark(docs, p);
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
	struct pane *p = m->ref.p, *next;

	if (forward) {
		/* report on d */
		if (p == NULL || p == list_last_entry(&doc->home->children,
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
		if (list_empty(&doc->home->children))
			p = NULL;
		else if (!p)
			p = list_last_entry(&doc->home->children,
					    struct pane, siblings);
		else if (p != list_first_entry(&doc->home->children,
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
	/* return value must be +ve, so use high bits to ensure this. */
	return (ret & 0xFFFFF) | 0x100000;
}

DEF_CMD(docs_set_ref)
{
	struct doc *dc = ci->home->data;
	struct docs *d = container_of(dc, struct docs, doc);
	struct mark *m = ci->mark;

	if (ci->numeric == 1 && !list_empty(&d->doc.home->children))
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
		if (list_empty(&doc->home->children))
			p = NULL;
		else if (!p)
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
	char *attr = ci->str;
	char *val;
	struct doc *d = ci->home->data;

	if ((val = attr_find(d->home->attrs, attr)) != NULL)
		;
	else if (strcmp(attr, "heading") == 0)
			val = "<bold,underline> Mod Document             File</>";
	else if (strcmp(attr, "line-format") == 0)
			val = " %doc-modified:3 %+name:20 %filename";
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

DEF_CMD(docs_open)
{
	struct pane *p;
	struct pane *dp = ci->mark->ref.p;
	struct pane *par;

	/* close this pane, open the given document. */
	if (dp == NULL)
		return 0;

	if (strcmp(ci->key, "Chr-o") == 0)
		par = call_pane("OtherPane", ci->focus, 0, NULL, 0);
	else
		par = call_pane("ThisPane", ci->focus, 0, NULL, 0);
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

DEF_CMD(docs_open_alt)
{
	struct pane *p;
	struct pane *dp = ci->mark->ref.p;
	char *renderer = NULL;
	struct pane *par;
	char buf[100];

	/* close this pane, open the given document. */
	if (dp == NULL)
		return 0;

	snprintf(buf, sizeof(buf), "render-%s", ci->key);
	renderer = pane_attr_get(dp, buf);
	if (!renderer)
		return -1;

	par = call_pane("ThisPane", ci->focus, 0, NULL, 0);
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

DEF_CMD(docs_bury)
{
	/* If the docs list is in a tile, put something else there. */
	/* FIXME should this be a function of the pane manager? */
	struct pane *tile, *doc;
	tile = call_pane("ThisPane", ci->focus, 0, NULL, 0);
	if (!tile)
		return 1;
	/* Discourage this doc from being chosen again */
	call3("doc:revisit", ci->focus, -1, NULL);
	doc = call_pane("docs:choose", ci->focus, 0, NULL, 0);
	if (doc)
		doc_attach_view(tile, doc, NULL);
	return 1;
}

DEF_CMD(docs_save)
{
	struct pane *dp = ci->mark->ref.p;

	if (!dp)
		return 0;
	doc_save(dp, ci->focus);
	return 1;
}

DEF_CMD(docs_kill)
{
	struct pane *dp = ci->mark->ref.p;
	char *mod;

	if (!dp)
		return 0;
	mod = pane_attr_get(dp, "doc-modified");
	if (mod && strcmp(mod, "yes") == 0 &&
	    ci->numeric == NO_NUMERIC) {
		call5("Message", ci->focus, 0, NULL,
		      "File modified, cannot kill.", 0);
		return 1;
	}
	doc_destroy(dp);
	return 1;
}

DEF_CMD(docs_toggle)
{
	struct pane *dp = ci->mark->ref.p;
	return call3("doc:modified", dp, 0, NULL);
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
	key_add(docs_map, "doc:revisit", &doc_revisit);
	key_add(docs_map, "doc:status-changed", &doc_damage);

	key_add(docs_map, "Chr-f", &docs_open);
	key_add(docs_map, "Return", &docs_open);
	key_add(docs_map, "Chr-o", &docs_open);
	key_add(docs_map, "Chr-q", &docs_bury);
	key_add(docs_map, "Chr-s", &docs_save);
	key_add(docs_map, "Chr-k", &docs_kill);
	key_add(docs_map, "Chr-%", &docs_toggle);
	key_add_range(docs_map, "Chr-A", "Chr-Z", &docs_open_alt);

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
	p = pane_register(ci->home, 0, &docs_handle.c, &doc->doc, NULL);
	if (!p) {
		free(doc->doc.name);
		free(doc);
		return -1;
	}
	doc->doc.home = p;

	doc->callback = docs_callback;
	call_comm7("global-set-command", ci->home, 0, NULL, "docs:", 0, "docs;",
		   &doc->callback);
	call_comm("global-set-command", ci->home, 0, NULL,
		  "doc:appeared-docs-register", 0, &doc->callback);

	return comm_call(ci->comm2, "callback:doc", p, 0, NULL, NULL, 0);
}

void edlib_init(struct pane *ed)
{
	call_comm("global-set-command", ed, 0, NULL, "attach-doc-docs",
		  0, &attach_docs);
}
