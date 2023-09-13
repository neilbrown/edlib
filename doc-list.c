/*
 * Copyright Neil Brown ©2022-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * doc-list: present an arbitrary list of items as a document.
 * This was initially created to support menus.  A doc-list is somewhat
 * similar to doc-dir or doc-docs in each each element in the document
 * has the primary content in the attributes associated with the element.
 *
 * Elements can be added after a mark with doc:list-add.  The mark remains
 * before the element so doc:set-attr can add appropriate attributes.
 *
 */

#define PRIVATE_DOC_REF

struct doc_ref {
	struct elmnt *p;
	unsigned int i;
};

#define DOC_SHARESREF
#define DOC_DATA_TYPE struct list
#define DOC_NEXT(d,m,r,b) list_next(d,r,b)
#define DOC_PREV(d,m,r,b) list_prev(d,r,b)
#include "core.h"
#include "misc.h"

struct elmnt {
	struct list_head	list;
	struct attrset		*attrs;
};

struct list {
	struct doc		doc;
	struct list_head	content;
};
#include "core-pane.h"

static inline wint_t list_next(struct pane *p safe, struct doc_ref *r safe, bool bytes)
{
	struct list *l = p->doc_data;

	if (r->p == NULL)
		return WEOF;

	if (r->p == list_last_entry(&l->content, struct elmnt, list))
		r->p = NULL;
	else
		r->p = list_next_entry(r->p, list);
	return ' ';
}

static inline wint_t list_prev(struct pane *p safe, struct doc_ref *r safe, bool bytes)
{
	struct list *l = p->doc_data;

	if (r->p == list_first_entry_or_null(&l->content, struct elmnt, list))
		return WEOF;

	if (r->p == NULL)
		r->p = list_last_entry(&l->content, struct elmnt, list);
	else
		r->p = list_prev_entry(r->p, list);
	return ' ';
}

DEF_CMD(list_char)
{
	return do_char_byte(ci);
}

DEF_CMD(list_set_ref)
{
	struct list *l = ci->home->doc_data;
	struct mark *m = ci->mark;

	if (!m)
		return Enoarg;
	mark_to_end(ci->home, m, ci->num != 1);
	if (list_empty(&l->content) || ci->num != 1)
		m->ref.p = NULL;
	else
		m->ref.p = list_first_entry(&l->content, struct elmnt, list);
	m->ref.i = 0;
	return 1;
}

DEF_CMD(list_set_attr)
{
	struct mark *m = ci->mark;
	const char *attr = ci->str;
	const char *val = ci->str2;
	struct elmnt *e;

	if (!m || !attr)
		return Enoarg;
	e = m->ref.p;
	if (!e)
		return Efallthrough;
	attr_set_str(&e->attrs, attr, val);
	pane_notify("doc:replaced-attr", ci->home, 1, m);
	return 1;
}

DEF_CMD(list_get_attr)
{
	struct mark *m = ci->mark;
	const char *attr = ci->str;
	const char *val = NULL;
	struct elmnt *e;

	if (!m || !attr)
		return Enoarg;
	e = m->ref.p;
	if (e)
		val = attr_find(e->attrs, attr);
	if (!val)
		return Efallthrough;
	comm_call(ci->comm2, "callback:get_attr", ci->focus, 0, m, val,
		  0, NULL, attr);
	return 1;
}

DEF_CMD(list_shares_ref)
{
	return 1;
}

DEF_CMD(list_add_elmnt)
{
	struct list *l = ci->home->doc_data;
	struct mark *m = ci->mark;
	struct elmnt *e;

	if (!m)
		return Enoarg;
	alloc(e, pane);
	if (m->ref.p)
		list_add(&e->list, &m->ref.p->list);
	else
		list_add_tail(&e->list, &l->content);
	m->ref.p = e;
	return 1;
}
 
DEF_CMD(list_del_elmnt)
{
	struct list *l = ci->home->doc_data;
	struct mark *m = ci->mark;
	struct mark *m2;
	struct elmnt *e;

	if (!m)
		return Enoarg;
	if (!m->ref.p)
		return Efalse;
	e = m->ref.p;
	while ((m2 = mark_next(m)) && m2->ref.p == e)
		m = m2;
	if (e == list_last_entry(&l->content, struct elmnt, list))
		m->ref.p = NULL;
	else
		m->ref.p = list_next_entry(e, list);
	while ((m2 = mark_prev(m)) && m2->ref.p == e) {
		m2->ref.p = m->ref.p;
		m = m2;
	}
	list_del(&e->list);
	unalloc(e, pane);
	return 1;
}

static char *key(struct list_head *le, const void *data)
{
	const char *keyattr = data;
	struct elmnt *e;
	char *ret;
	if (le == NULL || keyattr == NULL)
		return NULL;
	e = container_of(le, struct elmnt, list);
	ret = attr_find(e->attrs, keyattr);
	return ret ?: "";
}

DEF_CMD(list_sort)
{
	struct list *l = ci->home->doc_data;
	struct mark *m;

	if (!ci->str)
		return Enoarg;
	/* First move all marks to end */
	for (m = mark_first(&l->doc); m ; m = mark_next(m)) {
		m->ref.p = NULL;
		m->ref.i = 0;
	}
	sort_list(&l->content, key, ci->str);
	return 1;
}

static struct map *list_map;
DEF_LOOKUP_CMD(list_handle, list_map);

DEF_CMD(list_new)
{
	struct list *l;
	struct pane *p;

	p = doc_register(ci->home, &list_handle.c);
	if (!p)
		return Efail;
	l = p->doc_data;
	INIT_LIST_HEAD(&l->content);

	return comm_call(ci->comm2, "callback:doc", p);
}

DEF_CMD_CLOSED(list_close)
{
	struct list *l = ci->home->doc_data;
	struct elmnt *e;

	while ((e = list_first_entry_or_null(&l->content,
					    struct elmnt, list)) != NULL) {
		attr_free(&e->attrs);
		list_del(&e->list);
		unalloc(e, pane);
	}
	return 1;
}

static void list_init_map(void)
{
	list_map = key_alloc();
	key_add_chain(list_map, doc_default_cmd);
	key_add(list_map, "doc:char", &list_char);
	key_add(list_map, "doc:set-ref", &list_set_ref);
	key_add(list_map, "doc:set-attr", &list_set_attr);
	key_add(list_map, "doc:get-attr", &list_get_attr);
	key_add(list_map, "doc:shares-ref", &list_shares_ref);
	key_add(list_map, "doc:list-add", &list_add_elmnt);
	key_add(list_map, "doc:list-del", &list_del_elmnt);
	key_add(list_map, "doc:list-sort", &list_sort);
	key_add(list_map, "Close", &list_close);
}

void edlib_init(struct pane *ed safe)
{
	list_init_map();
	call_comm("global-set-command", ed, &list_new, 0, NULL,
		  "attach-doc-list");
}
