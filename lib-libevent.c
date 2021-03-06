/*
 * Copyright Neil Brown ©2015-2020 <neil@brown.name>
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
#include <unistd.h>
#include <event.h>
#include <string.h>
#include "core.h"
#include "misc.h"

static struct map *libevent_map;
DEF_LOOKUP_CMD(libevent_handle, libevent_map);

struct event_info {
	struct event_base *base;
	struct list_head event_list;
	struct pane *home safe;
	int dont_block;
	int deactivated;
	struct command read, signal, timer, run, deactivate,
		free, refresh, noblock;
};

struct evt {
	struct event *l safe;
	struct pane *home safe;
	char *event safe;
	struct command *comm safe;
	struct list_head lst;
	int active;	/* Don't delete or free this event, it is running */
	int num;
	int mseconds;
	int fd;
};

static void call_event(int thing, short sev, void *evv)
{
	struct evt *ev safe = safe_cast evv;
	int ret;
	int oldfd = ev->fd;
	int type;

	if (sev & EV_SIGNAL)
		type = TIME_SIG;
	else
		type = TIME_READ;

	ev->active = 1;
	time_start(type);
	if ((ret=comm_call(ev->comm, "callback:event", ev->home, thing)) < 0 ||
	    ev->active == 2) {
		if (oldfd == ev->fd)
			/* No early removal */
			event_del(ev->l);
		event_free(ev->l);
		list_del(&ev->lst);
		command_put(ev->comm);
		free(ev);
	} else
		ev->active = 0;
	time_stop(type);
}

static void call_timeout_event(int thing, short sev, void *evv)
{
	struct evt *ev safe = safe_cast evv;

	ev->active = 1;
	time_start(TIME_TIMER);
	if (comm_call(ev->comm, "callback:event", ev->home, thing) < 0 ||
	    ev->active == 2) {
		event_free(ev->l);
		list_del(&ev->lst);
		command_put(ev->comm);
		free(ev);
	} else {
		struct timeval tv;
		tv.tv_sec = ev->mseconds / 1000;
		tv.tv_usec = (ev->mseconds % 1000) * 1000;
		ev->active = 0;
		event_add(ev->l, &tv);
	}
	time_stop(TIME_TIMER);
}

DEF_CB(libevent_read)
{
	struct event_info *ei = container_of(ci->comm, struct event_info, read);
	struct evt *ev;

	if (!ci->comm2)
		return Enoarg;

	/* If there is already an event with this 'fd', we need
	 * to remove it now, else libevent gets confused.
	 * Presumably call_event() is now running and will clean up
	 * soon.
	 */
	list_for_each_entry(ev, &ei->event_list, lst)
		if (ci->num >= 0 && ev->fd == ci->num) {
			event_del(ev->l);
			ev->fd = -1;
		}

	ev = malloc(sizeof(*ev));

	if (!ei->base)
		ei->base = event_base_new();

	ev->l = safe_cast event_new(ei->base, ci->num, EV_READ|EV_PERSIST,
				    call_event, ev);
	ev->home = ci->focus;
	ev->comm = command_get(ci->comm2);
	ev->fd = ci->num;
	ev->num = ci->num;
	ev->active = 0;
	ev->event = "event:read";
	pane_add_notify(ei->home, ev->home, "Notify:Close");
	list_add(&ev->lst, &ei->event_list);
	event_add(ev->l, NULL);
	return 1;
}

DEF_CB(libevent_signal)
{
	struct event_info *ei = container_of(ci->comm, struct event_info, signal);
	struct evt *ev;

	if (!ci->comm2)
		return Enoarg;

	ev = malloc(sizeof(*ev));

	if (!ei->base)
		ei->base = event_base_new();

	ev->l = safe_cast event_new(ei->base, ci->num, EV_SIGNAL|EV_PERSIST,
				    call_event, ev);
	ev->home = ci->focus;
	ev->comm = command_get(ci->comm2);
	ev->fd = -1;
	ev->num = ci->num;
	ev->active = 0;
	ev->event = "event:signal";
	pane_add_notify(ei->home, ev->home, "Notify:Close");
	list_add(&ev->lst, &ei->event_list);
	event_add(ev->l, NULL);
	return 1;
}

DEF_CB(libevent_timer)
{
	struct event_info *ei = container_of(ci->comm, struct event_info, timer);
	struct evt *ev;
	struct timeval tv;

	if (!ci->comm2)
		return Enoarg;

	ev = malloc(sizeof(*ev));

	if (!ei->base)
		ei->base = event_base_new();

	ev->l = safe_cast event_new(ei->base, -1, 0,
				    call_timeout_event, ev);
	ev->home = ci->focus;
	ev->comm = command_get(ci->comm2);
	ev->mseconds = ci->num;
	ev->fd = -1;
	ev->num = ci->num;
	ev->active = 0;
	ev->event = "event:timer";
	pane_add_notify(ei->home, ev->home, "Notify:Close");
	list_add(&ev->lst, &ei->event_list);
	tv.tv_sec = ev->mseconds / 1000;
	tv.tv_usec = (ev->mseconds % 1000) * 1000;
	event_add(ev->l, &tv);
	return 1;
}

DEF_CB(libevent_run)
{
	struct event_info *ei = container_of(ci->comm, struct event_info, run);
	struct event_base *b = ei->base;
	int dont_block = ei->dont_block;

	ei->dont_block = 0;

	if (ei->deactivated)
		return Efallthrough;
	if (!b) {
		/* No events to wait for.. */
		if (dont_block)
			return 1;
		return 0;
	}

	/* Disable any alarm set by python (or other interpreter) */
	alarm(0);
	event_base_loop(b, dont_block ? EVLOOP_NONBLOCK : EVLOOP_ONCE);
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
	return Efail;
}

DEF_CB(libevent_deactivate)
{
	struct event_info *ei = container_of(ci->comm, struct event_info, deactivate);
	ei->base = NULL;
	ei->deactivated = 1;
	return 1;
}

DEF_CB(libevent_free)
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
			list_del_init(&ev->lst);
			if (ev->active)
				ev->active = 2;
			else {
				event_del(ev->l);
				command_put(ev->comm);
				free(ev);
			}
		}
	return 1;
}

DEF_CB(libevent_refresh)
{
	struct evt *ev;
	struct list_head *tmp;
	struct event_info *ei = container_of(ci->comm, struct event_info, refresh);
	struct list_head old;

	list_add(&old, &ei->event_list);
	list_del_init(&ei->event_list);
	list_for_each_entry_safe(ev, tmp, &old, lst) {
		event_del(ev->l);
		list_del(&ev->lst);
		call_comm(ev->event, ev->home, ev->comm, ev->num);
		command_put(ev->comm);
		free(ev);
	}
	return Efallthrough;
}

DEF_CB(libevent_noblock)
{
	struct event_info *ei = container_of(ci->comm, struct event_info,
					     noblock);

	ei->dont_block = 1;
	return 1;
}

DEF_CMD(libevent_notify)
{
	struct event_info *ei = ci->home->data;

	comm_call(&ei->free, "free", ci->focus);
	return 1;
}

DEF_CMD(libevent_activate)
{
	struct event_info *ei;
	struct pane *p;

	alloc(ei, pane);
	INIT_LIST_HEAD(&ei->event_list);
	ei->read = libevent_read;
	ei->signal = libevent_signal;
	ei->timer = libevent_timer;
	ei->run = libevent_run;
	ei->deactivate = libevent_deactivate;
	ei->free = libevent_free;
	ei->refresh = libevent_refresh;
	ei->noblock = libevent_noblock;
	p = pane_register(ei->home, 0, &libevent_handle.c, ei);
	if (!p)
		return Efail;
	ei->home = p;

	/* These are defaults, so make them sort late */
	call_comm("global-set-command", ci->focus, &ei->read,
		  0, NULL, "event:read-zz");
	call_comm("global-set-command", ci->focus, &ei->signal,
		  0, NULL, "event:signal-zz");
	call_comm("global-set-command", ci->focus, &ei->timer,
		  0, NULL, "event:timer-zz");
	call_comm("global-set-command", ci->focus, &ei->run,
		  0, NULL, "event:run-zz");
	call_comm("global-set-command", ci->focus, &ei->deactivate,
		  0, NULL, "event:deactivate-zz");
	call_comm("global-set-command", ci->focus, &ei->free,
		  0, NULL, "event:free-zz");
	call_comm("global-set-command", ci->focus, &ei->refresh,
		  0, NULL, "event:refresh-zz");
	call_comm("global-set-command", ci->focus, &ei->noblock,
		  0, NULL, "event:noblock-zz");
	call("event:refresh", ci->focus);

	return 1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &libevent_activate,
		  0, NULL, "attach-libevent");

	if (libevent_map)
		return;
	libevent_map = key_alloc();
	key_add(libevent_map, "Notify:Close", &libevent_notify);
	key_add(libevent_map, "Free", &edlib_do_free);
}
