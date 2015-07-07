#ifndef __LIST_H__
#define __LIST_H__

#define ASSERT(x) do { if (x) abort();} while (0)

/*Taken from various places in linux kernel */

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#endif
/**
 * container_of - cast a member of a structure out to the containing structure
 * @ptr:	the pointer to the member.
 * @type:	the type of the container struct this is embedded in.
 * @member:	the name of the member within the struct.
 *
 */
#define container_of(ptr, type, member) ({			\
	const typeof( ((type *)0)->member ) *__mptr = (ptr);	\
	(type *)( (char *)__mptr - offsetof(type,member) );})

/*
 * These are non-NULL pointers that will result in page faults
 * under normal circumstances, used to verify that nobody uses
 * non-initialized list entries.
 */
#define LIST_POISON1  ((void *) 0x00100100)
#define LIST_POISON2  ((void *) 0x00200200)

/*
 * Simple doubly linked list implementation.
 *
 * Some of the internal functions ("__xxx") are useful when
 * manipulating whole lists rather than single entries, as
 * sometimes we already know the next/prev entries and we can
 * generate better code by using them directly rather than
 * using the generic single-entry routines.
 */

struct list_head {
	struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }

#define LIST_HEAD(name) \
	struct list_head name = LIST_HEAD_INIT(name)

#define INIT_LIST_HEAD(ptr) do { \
	(ptr)->next = (ptr); (ptr)->prev = (ptr); \
} while (0)

/*
 * Insert a new entry between two known consecutive entries.
 *
 * This is only for internal list manipulation where we know
 * the prev/next entries already!
 */
static inline void __list_add(struct list_head *new,
			      struct list_head *prev,
			      struct list_head *next)
{
	next->prev = new;
	new->next = next;
	new->prev = prev;
	prev->next = new;
}

/**
 * list_add - add a new entry
 * @new: new entry to be added
 * @head: list head to add it after
 *
 * Insert a new entry after the specified head.
 * This is good for implementing stacks.
 */
static inline void list_add(struct list_head *new, struct list_head *head)
{
	__list_add(new, head, head->next);
}

/**
 * list_add_tail - add a new entry
 * @new: new entry to be added
 * @head: list head to add it before
 *
 * Insert a new entry before the specified head.
 * This is useful for implementing queues.
 */
static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
	__list_add(new, head->prev, head);
}

/*
 * Delete a list entry by making the prev/next entries
 * point to each other.
 *
 * This is only for internal list manipulation where we know
 * the prev/next entries already!
 */
static inline void __list_del(struct list_head * prev, struct list_head * next)
{
	next->prev = prev;
	prev->next = next;
}

/**
 * list_del - deletes entry from list.
 * @entry: the element to delete from the list.
 * Note: list_empty on entry does not return true after this, the entry is
 * in an undefined state.
 */
static inline void list_del(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
	entry->next = LIST_POISON1;
	entry->prev = LIST_POISON2;
}

/**
 * list_del_init - deletes entry from list and reinitialize it.
 * @entry: the element to delete from the list.
 */
static inline void list_del_init(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
	INIT_LIST_HEAD(entry);
}

/**
 * list_move - delete from one list and add as another's head
 * @list: the entry to move
 * @head: the head that will precede our entry
 */
static inline void list_move(struct list_head *list, struct list_head *head)
{
	__list_del(list->prev, list->next);
	list_add(list, head);
}

/**
 * list_move_tail - delete from one list and add as another's tail
 * @list: the entry to move
 * @head: the head that will follow our entry
 */
static inline void list_move_tail(struct list_head *list,
				  struct list_head *head)
{
	__list_del(list->prev, list->next);
	list_add_tail(list, head);
}

/**
 * list_empty - tests whether a list is empty
 * @head: the list to test.
 */
static inline int list_empty(struct list_head *head)
{
	return head->next == head;
}

/**
 * list_entry - get the struct for this entry
 * @ptr:	the &struct list_head pointer.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the list_struct within the struct.
 */
#define list_entry(ptr, type, member) \
	container_of(ptr, type, member)

/**
 * list_first_entry - get the first element from a list
 * @ptr:	the list head to take the element from.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the list_head within the struct.
 *
 * Note, that list is expected to be not empty.
 */
#define list_first_entry(ptr, type, member) \
	list_entry((ptr)->next, type, member)

/**
 * list_last_entry - get the last element from a list
 * @ptr:	the list head to take the element from.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the list_head within the struct.
 *
 * Note, that list is expected to be not empty.
 */
#define list_last_entry(ptr, type, member) \
	list_entry((ptr)->prev, type, member)

/**
 * list_first_entry_or_null - get the first element from a list
 * @ptr:	the list head to take the element from.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the list_head within the struct.
 *
 * Note that if the list is empty, it returns NULL.
 */
#define list_first_entry_or_null(ptr, type, member) \
	(!list_empty(ptr) ? list_first_entry(ptr, type, member) : NULL)

/**
 * list_next_entry - get the next element in list
 * @pos:	the type * to cursor
 * @member:	the name of the list_head within the struct.
 */
#define list_next_entry(pos, member) \
	list_entry((pos)->member.next, typeof(*(pos)), member)

/**
 * list_prev_entry - get the prev element in list
 * @pos:	the type * to cursor
 * @member:	the name of the list_head within the struct.
 */
#define list_prev_entry(pos, member) \
	list_entry((pos)->member.prev, typeof(*(pos)), member)

/**
 * list_for_each	-	iterate over a list
 * @pos:	the &struct list_head to use as a loop cursor.
 * @head:	the head for your list.
 */
#define list_for_each(pos, head) \
	for (pos = (head)->next; pos != (head); pos = pos->next)

/**
 * list_for_each_entry	-	iterate over list of given type
 * @pos:	the type * to use as a loop cursor.
 * @head:	the head for your list.
 * @member:	the name of the list_head within the struct.
 */
#define list_for_each_entry(pos, head, member)				\
	for (pos = list_first_entry(head, typeof(*pos), member);	\
	     &pos->member != (head);					\
	     pos = list_next_entry(pos, member))

/**
 * list_for_each_entry_continue - continue iteration over list of given type
 * @pos:	the type * to use as a loop cursor.
 * @head:	the head for your list.
 * @member:	the name of the list_head within the struct.
 *
 * Continue to iterate over list of given type, continuing after
 * the current position.
 */
#define list_for_each_entry_continue(pos, head, member)			\
	for (pos = list_next_entry(pos, member);			\
	     &pos->member != (head);					\
	     pos = list_next_entry(pos, member))

/**
 * list_for_each_entry_from - iterate over list of given type from the current point
 * @pos:	the type * to use as a loop cursor.
 * @head:	the head for your list.
 * @member:	the name of the list_head within the struct.
 *
 * Iterate over list of given type, continuing from current position.
 */
#define list_for_each_entry_from(pos, head, member)			\
	for (; &pos->member != (head);					\
	     pos = list_next_entry(pos, member))

struct hlist_head {
	struct hlist_node *first;
};

struct hlist_node {
	struct hlist_node *next, **pprev;
};

#define HLIST_HEAD_INIT { .first = NULL }
#define HLIST_HEAD(name) struct hlist_head name = {  .first = NULL }
#define INIT_HLIST_HEAD(ptr) ((ptr)->first = NULL)
static inline void INIT_HLIST_NODE(struct hlist_node *h)
{
	h->next = NULL;
	h->pprev = NULL;
}

static inline int hlist_unhashed(const struct hlist_node *h)
{
	return !h->pprev;
}

static inline int hlist_empty(const struct hlist_head *h)
{
	return !h->first;
}

static inline void __hlist_del(struct hlist_node *n)
{
	struct hlist_node *next = n->next;
	struct hlist_node **pprev = n->pprev;
	*pprev = next;
	if (next)
		next->pprev = pprev;
}

static inline void hlist_del(struct hlist_node *n)
{
	__hlist_del(n);
	n->next = LIST_POISON1;
	n->pprev = LIST_POISON2;
}

static inline void hlist_del_init(struct hlist_node *n)
{
	if (!hlist_unhashed(n)) {
		__hlist_del(n);
		INIT_HLIST_NODE(n);
	}
}

static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{
	struct hlist_node *first = h->first;
	n->next = first;
	if (first)
		first->pprev = &n->next;
	h->first = n;
	n->pprev = &h->first;
}

/* next must be != NULL */
static inline void hlist_add_before(struct hlist_node *n,
					struct hlist_node *next)
{
	n->pprev = next->pprev;
	n->next = next;
	next->pprev = &n->next;
	*(n->pprev) = n;
}

static inline void hlist_add_after(struct hlist_node *n,
					struct hlist_node *next)
{
	next->next = n->next;
	n->next = next;
	next->pprev = &n->next;

	if(next->next)
		next->next->pprev  = &next->next;
}

/* after that we'll appear to be on some hlist and hlist_del will work */
static inline void hlist_add_fake(struct hlist_node *n)
{
	n->pprev = &n->next;
}

/*
 * Move a list from one list head to another. Fixup the pprev
 * reference of the first entry if it exists.
 */
static inline void hlist_move_list(struct hlist_head *old,
				   struct hlist_head *new)
{
	new->first = old->first;
	if (new->first)
		new->first->pprev = &new->first;
	old->first = NULL;
}

#define hlist_entry(ptr, type, member) container_of(ptr,type,member)

#define hlist_next_entry(ptr, member) \
	hlist_entry((ptr)->member.next, typeof(*(ptr)), member)
#define hlist_first_entry(head, type, member)			\
	hlist_entry((head)->first, type, member)
#define hlist_prev(ptr) container_of((ptr)->pprev, struct hlist_node, next)
#define hlist_prev_entry(ptr, member) \
	hlist_entry(hlist_prev(&(ptr)->member), typeof(*(ptr)), member)

#define hlist_for_each(pos, head) \
	for (pos = (head)->first; pos ; pos = pos->next)

#define hlist_for_each_safe(pos, n, head) \
	for (pos = (head)->first; pos && ({ n = pos->next; 1; }); \
	     pos = n)

/**
 * hlist_for_each_entry	- iterate over list of given type
 * @pos:	the type * to use as a loop cursor.
 * @head:	the head for your list.
 * @member:	the name of the hlist_node within the struct.
 */
#define hlist_for_each_entry(pos, head, member)			\
	for (pos = (head)->first ? hlist_first_entry(head, typeof(*pos), member) : NULL; \
	     pos ;							\
	     pos = pos->member.next ? hlist_next_entry(pos, member) : NULL)
/**
 * hlist_for_each_entry_continue - iterate over a hlist continuing after current point
 * @pos:	the type * to use as a loop cursor.
 * @member:	the name of the hlist_node within the struct.
 */
#define hlist_for_each_entry_continue(pos, member)		 \
	for (pos = (pos)->member.next ? hlist_next_entry(pos, member) : NULL;	\
	     pos ;							\
	     pos = pos->member.next ? hlist_next_entry(pos, member) : NULL)

/**
 * hlist_for_each_entry_continue_reverse - iterate backwards over a hlist continuing after current point
 * @pos:	the type * to use as a loop cursor.
 * @member:	the name of the hlist_node within the struct.
 */
#define hlist_for_each_entry_continue_reverse(pos, head, member)		 \
	for (pos = &pos->member == (head)->first ? NULL : hlist_prev_entry(pos, member);\
	     pos ;						\
	     pos = &pos->member == (head)->first ? NULL : hlist_prev_entry(pos, member ))

/**
 * hlist_for_each_entry_from - iterate over a hlist continuing from current point
 * @tpos:	the type * to use as a loop cursor.
 * @pos:	the &struct hlist_node to use as a loop cursor.
 * @member:	the name of the hlist_node within the struct.
 */
#if 0
#define hlist_for_each_entry_from(tpos, pos, member)			\
	for (; pos &&							\
		({ tpos = hlist_entry(pos, typeof(*tpos), member); 1;}); \
	     pos = pos->next)
#endif
/**
 * hlist_for_each_entry_safe - iterate over list of given type safe against removal of list entry
 * @tpos:	the type * to use as a loop cursor.
 * @pos:	the &struct hlist_node to use as a loop cursor.
 * @n:		another &struct hlist_node to use as temporary storage
 * @head:	the head for your list.
 * @member:	the name of the hlist_node within the struct.
 */
#if 0
#define hlist_for_each_entry_safe(tpos, pos, n, head, member)		\
	for (pos = (head)->first;					\
	     pos && ({ n = pos->next; 1; }) &&				\
		({ tpos = hlist_entry(pos, typeof(*tpos), member); 1;}); \
	     pos = n)
#endif

/*
 * tlists are lists with types.
 * The low 2 bits of each of the pointers provide 4 bits of type
 * information.  This is useful when different fields in different
 * structures can all be on the same list
 */
struct tlist_head {
	struct tlist_head *next, *prev;
};

#define TLIST_PTR(p) ((struct tlist_head *)(((unsigned long)(p)) & ~3UL))
#define __TLIST_TYPE(p,n) ((((unsigned long)(p)) & 3)<<2 | (((unsigned long)(n)) & 3))
#define TLIST_TYPE(e) __TLIST_TYPE((e)->prev, (e)->next)
#define TLIST_PREV(p,t) ((struct tlist_head*)((((t)>>2)&3)|(unsigned long)(p)))
#define TLIST_NEXT(n,t) ((struct tlist_head*)(((t)&3)|(unsigned long)(n)))

#define TLIST_HEAD_INIT(name, type) { TLIST_NEXT(&(name),(type)),	\
				      TLIST_PREV(&(name),(type)) }
#define INIT_TLIST_HEAD(ptr, type) do {		\
	(ptr)->next = TLIST_NEXT((ptr),(type));	\
	(ptr)->prev = TLIST_PREV((ptr),(type));	\
} while (0)

/**
 * tlist_empty - tests whether a tlist is empty
 * @head: the list to test.
 */
static inline int tlist_empty(struct tlist_head *head)
{
	return TLIST_PTR(head->next) == head;
}
/*
 * Insert a new entry between two known consecutive entries.
 *
 * This is only for internal list manipulation where we know
 * the prev/next entries already!
 */
static inline void __tlist_add(struct tlist_head *new, int type,
			       struct tlist_head *prev,
			       struct tlist_head *next)
{
	next->prev = TLIST_PREV(new, __TLIST_TYPE(next->prev, NULL));
	new->next = TLIST_NEXT(next, type);
	new->prev = TLIST_PREV(prev, type);
	prev->next = TLIST_NEXT(new, __TLIST_TYPE(NULL, prev->next));
}

/**
 * list_entry - get the struct for this entry
 * @ptr:	the &struct list_head pointer.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the list_head within the struct.
 */
#define tlist_entry(ptr, type, member) \
	container_of(TLIST_PTR(ptr), type, member)

/**
 * tlist_add - add a new entry
 * @new: new entry to be added
 * @type: type of location
 * @head: list head to add it after
 *
 * Insert a new entry after the specified head.
 * This is good for implementing stacks.
 */
static inline void tlist_add(struct tlist_head *new, int type, struct tlist_head *head)
{
	__tlist_add(new, type, head, TLIST_PTR(head->next));
}

/**
 * tlist_add_tail - add a new entry
 * @new: new entry to be added
 * @type: type of location
 * @head: list head to add it before
 *
 * Insert a new entry before the specified head.
 * This is useful for implementing queues.
 */
static inline void tlist_add_tail(struct tlist_head *new, int type, struct tlist_head *head)
{
	__tlist_add(new, type, TLIST_PTR(head->prev), head);
}

/*
 * Delete a list entry by making the prev/next entries
 * point to each other.
 *
 * This is only for internal list manipulation where we know
 * the prev/next entries already!
 */
static inline void __tlist_del(struct tlist_head * prev, struct tlist_head * next)
{
	int nt = TLIST_TYPE(next);
	int pt = TLIST_TYPE(prev);
	next->prev = TLIST_PREV(TLIST_PTR(prev), nt);
	prev->next = TLIST_NEXT(TLIST_PTR(next), pt);
}

/**
 * tlist_del - deletes entry from list.
 * @entry: the element to delete from the list.
 * Note: list_empty on entry does not return true after this, the entry is
 * in an undefined state.
 */
static inline void tlist_del(struct tlist_head *entry)
{
	__tlist_del(TLIST_PTR(entry->prev), TLIST_PTR(entry->next));
	entry->next = LIST_POISON1;
	entry->prev = LIST_POISON2;
}

/**
 * tlist_del_init - deletes entry from list and reinitialize it.
 * @entry: the element to delete from the list.
 */
static inline void tlist_del_init(struct tlist_head *entry)
{
	int type = TLIST_TYPE(entry);
	__tlist_del(TLIST_PTR(entry->prev), TLIST_PTR(entry->next));
	INIT_TLIST_HEAD(entry, type);
}

/**
 * tlist_next_entry - get the next element in list
 * @pos:	the type * to cursor
 * @member:	the name of the tlist_head within the struct.
 */
#define tlist_next_entry(pos, member) \
	tlist_entry((pos)->member.next, typeof(*(pos)), member)

/**
 * tlist_prev_entry - get the prev element in list
 * @pos:	the type * to cursor
 * @member:	the name of the list_head within the struct.
 */
#define tlist_prev_entry(pos, member) \
	tlist_entry((pos)->member.prev, typeof(*(pos)), member)

/**
 * tlist_for_each_entry -  iterate over list of given type
 * @pos:	the type * to use as a loop cursor.
 * @head:	the head for your list.
 * @member:	the name of the tlist_head within the struct.
 *
 * Iterate over list of given type.
 */
#define tlist_for_each_entry(pos, head, member)		\
	for (pos = list_entry((head)->next, typeof(*(pos)), member);	\
	     &pos->member != (head);					\
	     pos = tlist_next_entry(pos, member))

/**
 * tlist_for_each_entry_continue - continue iteration over list of given type
 * @pos:	the type * to use as a loop cursor.
 * @head:	the head for your list.
 * @member:	the name of the tlist_head within the struct.
 *
 * Continue to iterate over list of given type, continuing after
 * the current position.
 */
#define tlist_for_each_entry_continue(pos, head, member)		\
	for (pos = tlist_next_entry(pos, member);			\
	     &pos->member != (head);					\
	     pos = tlist_next_entry(pos, member))

/**
 * list_for_each_entry_continue_reverse - iterate backwards from the given point
 * @pos:	the type * to use as a loop cursor.
 * @head:	the head for your list.
 * @member:	the name of the list_head within the struct.
 *
 * Start to iterate over list of given type backwards, continuing after
 * the current position.
 */
#define tlist_for_each_entry_continue_reverse(pos, head, member)		\
	for (pos = tlist_prev_entry(pos, member);			\
	     &pos->member != (head);					\
	     pos = tlist_prev_entry(pos, member))

#endif /* __LIST_H__ */
