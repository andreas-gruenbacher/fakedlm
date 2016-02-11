#ifndef __LIST_H
#define __LIST_H

#include <stdbool.h>

struct list_head {
	struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }

#define LIST_HEAD(name) \
	struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list)
{
	list->next = list;
	list->prev = list;
}

static inline void
__list_add(struct list_head *new,
	   struct list_head *prev,
	   struct list_head *next)
{
	next->prev = new;
	new->next = next;
	new->prev = prev;
	prev->next = new;
}

static inline void
list_add(struct list_head *entry, struct list_head *head)
{
	__list_add(entry, head, head->next);
}

static inline void
list_add_tail(struct list_head *entry, struct list_head *head)
{
	__list_add(entry, head->prev, head);
}

static inline void
list_del(struct list_head *entry)
{
	struct list_head *next = entry->next;
	struct list_head *prev = entry->prev;
	next->prev = prev;
	prev->next = next;
}

static inline void
list_del_init(struct list_head *entry)
{
	list_del(entry);
	INIT_LIST_HEAD(entry);
}

static inline bool
list_empty(const struct list_head *head)
{
	return head->next == head;
}

#define list_entry(ptr, type, member) \
	(type *)( (char *)(ptr) - offsetof(type, member) )

#define list_first_entry(ptr, type, member) \
	list_entry((ptr)->next, type, member)

#define list_next_entry(pos, member) \
	list_entry((pos)->member.next, typeof(*(pos)), member)

#define list_for_each_entry(pos, head, member) \
	for (pos = list_first_entry(head, typeof(*pos), member); \
	     &pos->member != (head); \
	     pos = list_next_entry(pos, member))

#define list_for_each_entry_safe(pos, n, head, member) \
	for (pos = list_first_entry(head, typeof(*pos), member), \
	       n = list_next_entry(pos, member); \
	     &pos->member != (head); \
	     pos = n, n = list_next_entry(n, member))

#endif  /* __LIST_H */
