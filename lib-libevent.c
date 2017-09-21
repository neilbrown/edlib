/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * libevent support for edlib.
 *
 * Register command "attach-libevent".
 * When that is called, register:
 *   "event:read"
 *   "event:signal"
 *   "event:run"
 *   "event:deactivate"
 *
 * When "event:deactivate" is called, cause event:run to abort.
 */

#include <stdlib.h>
#include <event.h>
#include "core.h"

static struct event_base *base;
static struct list_head event_list;

struct evt {
	struct event *l safe;
	struct pane *home safe;
	struct command *comm safe;
	struct list_head lst;
	int seconds;
	int fd;
};

static void call_event(int thing, short sev, void *evv)
{
	struct evt *ev safe = safe_cast evv;
	int ret;
	int oldfd = ev->fd;

	if ((ret=comm_call(ev->comm, "callback:event", ev->home, thing,
			   NULL, NULL, 0)) < 0) {
		if (oldfd == ev->fd)
			/* No early removal */
			event_del(ev->l);
		event_free(ev->l);
		list_del(&ev->lst);
		command_put(ev->comm);
		free(ev);
	}
}

static void call_timeout_event(int thing, short sev, void *evv)
{
	struct evt *ev safe = safe_cast evv;

	if (comm_call(ev->comm, "callback:event", ev->home, thing,
		      NULL, NULL, 0) < 0) {
		event_free(ev->l);
		list_del(&ev->lst);
		command_put(ev->comm);
		free(ev);
	} else {
		struct timeval tv;
		tv.tv_sec = ev->seconds;
		tv.tv_usec = 0;
		event_add(ev->l, &tv);
	}
}

DEF_CMD(libevent_read)
{
	struct evt *ev;

	if (!ci->comm2)
		return -1;

	/* If there is already an event with this 'fd', we need
	 * to remove it now, else libevent gets confused.
	 * Presumably call_event() is now running and will clean up
	 * soon.
	 */
	list_for_each_entry(ev, &event_list, lst)
		if (ci->numeric >= 0 && ev->fd == ci->numeric) {
			event_del(ev->l);
			ev->fd = -1;
		}

	ev = malloc(sizeof(*ev));

	if (!base)
		base = event_base_new();

	ev->l = safe_cast event_new(base, ci->numeric, EV_READ|EV_PERSIST,
				    call_event, ev);
	ev->home = ci->focus;
	ev->comm = command_get(ci->comm2);
	ev->fd = ci->numeric;
	list_add(&ev->lst, &event_list);
	event_add(ev->l, NULL);
	return 1;
}

DEF_CMD(libevent_signal)
{
	struct evt *ev;

	if (!ci->comm2)
		return -1;

	ev = malloc(sizeof(*ev));

	if (!base)
		base = event_base_new();

	ev->l = safe_cast event_new(base, ci->numeric, EV_SIGNAL|EV_PERSIST,
				    call_event, ev);
	ev->home = ci->focus;
	ev->comm = command_get(ci->comm2);
	ev->fd = -1;
	list_add(&ev->lst, &event_list);
	event_add(ev->l, NULL);
	return 1;
}

DEF_CMD(libevent_timer)
{
	struct evt *ev;
	struct timeval tv;

	if (!ci->comm2)
		return -1;

	ev = malloc(sizeof(*ev));

	if (!base)
		base = event_base_new();

	ev->l = safe_cast event_new(base, -1, 0,
				    call_timeout_event, ev);
	ev->home = ci->focus;
	ev->comm = command_get(ci->comm2);
	ev->seconds = ci->numeric;
	ev->fd = -1;
	list_add(&ev->lst, &event_list);
	tv.tv_sec = ev->seconds;
	tv.tv_usec = 0;
	event_add(ev->l, &tv);
	return 1;
}

DEF_CMD(libevent_run)
{
	struct event_base *b = base;
	if (!b)
		return 0;

	event_base_loop(b, EVLOOP_ONCE);
	if (base == b)
		return 1;
	while (!list_empty(&event_list)) {
		struct evt *ev = list_first_entry(&event_list, struct evt, lst);
		list_del(&ev->lst);
		event_del(ev->l);
		event_free(ev->l);
		command_put(ev->comm);
		free(ev);
	}
	event_base_free(b);
	return -1;
}

DEF_CMD(libevent_deactivate)
{
	base = NULL;
	return 1;
}

DEF_CMD(libevent_activate)
{
	/* These are defaults, so make them sort late */
	call_comm("global-set-command", ci->focus, 0, NULL, "event:read-zz",
		  &libevent_read);
	call_comm("global-set-command", ci->focus, 0, NULL, "event:signal-zz",
		  &libevent_signal);
	call_comm("global-set-command", ci->focus, 0, NULL, "event:timer-zz",
		  &libevent_timer);
	call_comm("global-set-command", ci->focus, 0, NULL, "event:run-zz",
		  &libevent_run);
	call_comm("global-set-command", ci->focus, 0, NULL, "event:deactivate-zz",
		  &libevent_deactivate);

	return 1;
}

void edlib_init(struct pane *ed safe)
{
	INIT_LIST_HEAD(&event_list);
	call_comm("global-set-command", ed, 0, NULL, "attach-libevent",
		  &libevent_activate);
}
