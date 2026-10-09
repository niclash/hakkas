#include "graphs/deadlines.h"

#include <stdlib.h>

/* Initial size of the heap array; it doubles when more room is reserved. */
#define DEADLINES_INITIAL 8

void deadline_heap_init(DeadlineHeap *h)
{
	*h = (DeadlineHeap){0};
}

void deadline_heap_deinit(DeadlineHeap *h)
{
	free(h->items);
	*h = (DeadlineHeap){0};
}

bool deadline_heap_reserve(DeadlineHeap *h, uint32_t n)
{
	if (n <= h->cap)
		return true;
	if (n > UINT32_MAX / 2)
		return false;
	uint32_t cap = h->cap == 0 ? DEADLINES_INITIAL : h->cap;
	while (cap < n)
		cap *= 2;
	Deadline **items = realloc(h->items, (size_t)cap * sizeof(*items));
	if (items == NULL)
		return false;
	h->items = items;
	h->cap = cap;
	return true;
}

/* Whether a comes out before b. */
static bool before(const Deadline *a, const Deadline *b)
{
	return a->due_ms < b->due_ms || (a->due_ms == b->due_ms && a->seq < b->seq);
}

static void place(DeadlineHeap *h, uint32_t i, Deadline *d)
{
	h->items[i] = d;
	d->index = i;
}

/* Move the deadline at i up or down until the heap order holds again. */
static void fix(DeadlineHeap *h, uint32_t i)
{
	Deadline *d = h->items[i];
	while (i > 0) {
		uint32_t parent = (i - 1) / 2;
		if (!before(d, h->items[parent]))
			break;
		place(h, i, h->items[parent]);
		i = parent;
	}
	for (;;) {
		uint32_t child = 2 * i + 1;
		if (child >= h->len)
			break;
		if (child + 1 < h->len && before(h->items[child + 1], h->items[child]))
			child++;
		if (!before(h->items[child], d))
			break;
		place(h, i, h->items[child]);
		i = child;
	}
	place(h, i, d);
}

bool deadline_heap_set(DeadlineHeap *h, Deadline *d, uint64_t due_ms)
{
	if (!deadline_armed(d)) {
		if (h->len == h->cap)
			return false;
		d->index = h->len++;
		h->items[d->index] = d;
	}
	d->due_ms = due_ms;
	d->seq = h->next_seq++;
	fix(h, d->index);
	return true;
}

void deadline_heap_remove(DeadlineHeap *h, Deadline *d)
{
	if (!deadline_armed(d))
		return;
	uint32_t i = d->index;
	d->index = DEADLINE_UNSET;
	Deadline *last = h->items[--h->len];
	if (i < h->len) {
		h->items[i] = last;
		last->index = i;
		fix(h, i);
	}
}

Deadline *deadline_heap_pop_due(DeadlineHeap *h, uint64_t now_ms)
{
	Deadline *d = deadline_heap_peek(h);
	if (d == NULL || d->due_ms > now_ms)
		return NULL;
	deadline_heap_remove(h, d);
	return d;
}
