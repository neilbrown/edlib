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
#include <string.h>
#include "core.h"

struct event_info {
	struct event_base *base;
	struct list_head event_list;
	struct pane *home safe;
	struct command read, signal, timer, run, deactivate, free;
};

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

	if ((ret=comm_call(ev->comm, "callback:event", ev->home, thing)) < 0) {
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

	if (comm_call(ev->comm, "callback:event", ev->home, thing) < 0) {
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
	struct event_info *ei = container_of(ci->comm, struct event_info, read);
	struct evt *ev;

	if (!ci->comm2)
		return -1;

	/* If there is already an event with this 'fd', we need
	 * to remove it now, else libevent gets confused.
	 * Presumably call_event() is now running and will clean up
	 * soon.
	 */
	list_for_each_entry(ev, &ei->event_list, lst)
		if (ci->numeric >= 0 && ev->fd == ci->numeric) {
			event_del(ev->l);
			ev->fd = -1;
		}

	ev = malloc(sizeof(*ev));

	if (!ei->base)
		ei->base = event_base_new();

	ev->l = safe_cast event_new(ei->base, ci->numeric, EV_READ|EV_PERSIST,
				    call_event, ev);
	ev->home = ci->focus;
	ev->comm = command_get(ci->comm2);
	ev->fd = ci->numeric;
	pane_add_notify(ei->home, ev->home, "Notify:Close");
	list_add(&ev->lst, &ei->event_list);
	event_add(ev->l, NULL);
	return 1;
}

DEF_CMD(libevent_signal)
{
	struct event_info *ei = container_of(ci->comm, struct event_info, signal);
	struct evt *ev;

	if (!ci->comm2)
		return -1;

	ev = malloc(sizeof(*ev));

	if (!ei->base)
		ei->base = event_base_new();

	ev->l = safe_cast event_new(ei->base, ci->numeric, EV_SIGNAL|EV_PERSIST,
				    call_event, ev);
	ev->home = ci->focus;
	ev->comm = command_get(ci->comm2);
	ev->fd = -1;
	pane_add_notify(ei->home, ev->home, "Notify:Close");
	list_add(&ev->lst, &ei->event_list);
	event_add(ev->l, NULL);
	return 1;
}

DEF_CMD(libevent_timer)
{
	struct event_info *ei = container_of(ci->comm, struct event_info, timer);
	struct evt *ev;
	struct timeval tv;

	if (!ci->comm2)
		return -1;

	ev = malloc(sizeof(*ev));

	if (!ei->base)
		ei->base = event_base_new();

	ev->l = safe_cast event_new(ei->base, -1, 0,
				    call_timeout_event, ev);
	ev->home = ci->focus;
	ev->comm = command_get(ci->comm2);
	ev->seconds = ci->numeric;
	ev->fd = -1;
	pane_add_notify(ei->home, ev->home, "Notify:Close");
	list_add(&ev->lst, &ei->event_list);
	tv.tv_sec = ev->seconds;
	tv.tv_usec = 0;
	event_add(ev->l, &tv);
	return 1;
}

DEF_CMD(libevent_run)
{
	struct event_info *ei = container_of(ci->comm, struct event_info, run);
	struct event_base *b = ei->base;

	if (!b)
		return 0;

	event_base_loop(b, EVLOOP_ONCE);
	if (ei->base == b)
		return 1;
	while (!list_empty(&ei->event_list)) {
		struct evt *ev = list_first_entry(&ei->event_list, struct evt, lst);
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
	struct event_info *ei = container_of(ci->comm, struct event_info, deactivate);
	ei->base = NULL;
	return 1;
}

DEF_CMD(libevent_free)
{
	/* destroy for ci->focus and, if comm2 given, which activate
	 * comm2
	 */
	struct evt *ev;
	struct list_head *tmp;
	struct event_info *ei = container_of(ci->comm, struct event_info, free);

	list_for_each_entry_safe(ev, tmp, &ei->event_list, lst)
		if (ev->home == ci->focus &&
		    (ci->comm2 == NULL || ev->comm == ci->comm2)) {
			event_del(ev->l);
			list_del(&ev->lst);
			command_put(ev->comm);
			free(ev);
		}
	return 1;
}

DEF_CMD(libevent_handle)
{
	struct event_info *ei = ci->home->data;

	if (strcmp(ci->key, "Notify:Close") == 0) {
		comm_call(&ei->free, "free", ci->focus);
		return 1;
	}
	return 1;
}

DEF_CMD(libevent_activate)
{
	struct event_info *ei = calloc(1, sizeof(*ei));

	INIT_LIST_HEAD(&ei->event_list);
	ei->read = libevent_read;
	ei->signal = libevent_signal;
	ei->timer = libevent_timer;
	ei->run = libevent_run;
	ei->deactivate = libevent_deactivate;
	ei->free = libevent_free;
	ei->home = pane_register(ei->home, 0, &libevent_handle, ei, NULL);

	/* These are defaults, so make them sort late */
	call_comm("global-set-command", ci->focus, 0, NULL, "event:read-zz",
		  &ei->read);
	call_comm("global-set-command", ci->focus, 0, NULL, "event:signal-zz",
		  &ei->signal);
	call_comm("global-set-command", ci->focus, 0, NULL, "event:timer-zz",
		  &ei->timer);
	call_comm("global-set-command", ci->focus, 0, NULL, "event:run-zz",
		  &ei->run);
	call_comm("global-set-command", ci->focus, 0, NULL, "event:deactivate-zz",
		  &ei->deactivate);
	call_comm("global-set-command", ci->focus, 0, NULL, "event:free-zz",
		  &ei->free);

	return 1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, 0, NULL, "attach-libevent",
		  &libevent_activate);
}
