#pragma once

#include "sys/queue.h"
#include "type-macros.h"
#include <stddef.h>

#define WEAKREF_CAST(ref, type, member) \
	((ref).subject ? container_of((ref).subject, type, member) : NULL)

struct weakref_subject;

struct weakref_observer {
	LIST_ENTRY(weakref_observer) link;
	struct weakref_subject* subject;
};

LIST_HEAD(weakref_subject, weakref_observer);

static inline void weakref_observer_init(struct weakref_observer* observer,
		struct weakref_subject* subject)
{
	observer->subject = subject;
	LIST_INSERT_HEAD(subject, observer, link);
}

static inline void weakref_observer_deinit(struct weakref_observer* self)
{
	if (self->subject)
		LIST_REMOVE(self, link);
	self->subject = NULL;
}

static inline void weakref_subject_init(struct weakref_subject* self)
{
	LIST_INIT(self);
}

static inline void weakref_subject_deinit(struct weakref_subject* self)
{
	while (!LIST_EMPTY(self)) {
		struct weakref_observer* observer = LIST_FIRST(self);
		LIST_REMOVE(observer, link);
		observer->subject = NULL;
	}
}
