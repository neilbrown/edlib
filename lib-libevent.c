/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * libevent support for edlib.
 *
 * Register command "attach-libevent".
 * When that is called, register:
 *   "event:read"
 *   "event:write"
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
#define PANE_DATA_TYPE struct event_info
#include "core.h"
#include "misc.h"

enum {
	EV_LIST,	/* Events handled by libevent */
	POLL_LIST,	/* Events to poll before calling event_base_loop */
	PRIO_0_LIST,	/* background task - run one per loop */
	PRIO_1_LIST,	/* non-trivial follow-up tasks, line pane_refresh */
	PRIO_2_LIST,	/* fast follow-up tasks like freeing memory */
	NR_LISTS
};

struct event_info {
	struct event_base *base;
	struct list_head event_list[NR_LISTS];
	struct pane *home safe;
	int dont_block;
	int deactivated;
	struct command read, write, signal, timer, poll, on_idle,
		run, deactivate, free, refresh, noblock;
};
#include "core-pane.h"

static struct map *libevent_map;
DEF_LOOKUP_CMD(libevent_handle, libevent_map);

struct evt {
	struct event *l;
	struct pane *home safe;
	char *event safe;
	struct command *comm safe;
	struct list_head lst;
	int active;	/* Don't delete or free this event, it is running */
	int num;	/* signal or mseconds or fd */
};

static void call_event(int thing, short sev, void *evv)
{
	struct evt *ev safe = safe_cast evv;
	int type;

	if (sev & EV_SIGNAL)
		type = TIME_SIG;
	else
		type = TIME_READ;

	ev->active = 1;
	time_start(type);
	if (comm_call(ev->comm, "callback:event", ev->home, thing) < 0 ||
	    ev->active == 2) {
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
		tv.tv_sec = ev->num / 1000;
		tv.tv_usec = (ev->num % 1000) * 1000;
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
	list_for_each_entry(ev, &ei->event_list[EV_LIST], lst) {
		int fd = event_get_fd(ev->l);
		if (fd >= 0 && ci->num >= 0 && fd == ci->num)
			event_del(ev->l);
	}

	ev = malloc(sizeof(*ev));

	if (!ei->base)
		ei->base = event_base_new();

	ev->l = safe_cast event_new(ei->base, ci->num, EV_READ|EV_PERSIST,
				    call_event, ev);
	ev->home = ci->focus;
	ev->comm = command_get(ci->comm2);
	ev->num = ci->num;
	ev->active = 0;
	ev->event = "event:read";
	pane_add_notify(ei->home, ev->home, "Notify:Close");
	list_add(&ev->lst, &ei->event_list[EV_LIST]);
	event_add(ev->l, NULL);
	return 1;
}

DEF_CB(libevent_write)
{
	struct event_info *ei = container_of(ci->comm, struct event_info, write);
	struct evt *ev;

	if (!ci->comm2)
		return Enoarg;

	/* If there is already an event with this 'fd', we need
	 * to remove it now, else libevent gets confused.
	 * Presumably call_event() is now running and will clean up
	 * soon.
	 */
	list_for_each_entry(ev, &ei->event_list[EV_LIST], lst) {
		int fd = event_get_fd(ev->l);
		if (fd >= 0 && ci->num >= 0 && fd == ci->num)
			event_del(ev->l);
	}

	ev = malloc(sizeof(*ev));

	if (!ei->base)
		ei->base = event_base_new();

	ev->l = safe_cast event_new(ei->base, ci->num, EV_WRITE|EV_PERSIST,
				    call_event, ev);
	ev->home = ci->focus;
	ev->comm = command_get(ci->comm2);
	ev->num = ci->num;
	ev->active = 0;
	ev->event = "event:write";
	pane_add_notify(ei->home, ev->home, "Notify:Close");
	list_add(&ev->lst, &ei->event_list[EV_LIST]);
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
	ev->num = ci->num;
	ev->active = 0;
	ev->event = "event:signal";
	pane_add_notify(ei->home, ev->home, "Notify:Close");
	list_add(&ev->lst, &ei->event_list[EV_LIST]);
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
	ev->num = ci->num;
	ev->active = 0;
	ev->event = "event:timer";
	pane_add_notify(ei->home, ev->home, "Notify:Close");
	list_add(&ev->lst, &ei->event_list[EV_LIST]);
	tv.tv_sec = ev->num / 1000;
	tv.tv_usec = (ev->num % 1000) * 1000;
	event_add(ev->l, &tv);
	return 1;
}

DEF_CB(libevent_poll)
{
	struct event_info *ei = container_of(ci->comm, struct event_info, poll);
	struct evt *ev;

	if (!ci->comm2)
		return Enoarg;

	ev = malloc(sizeof(*ev));

	if (!ei->base)
		ei->base = event_base_new();

	ev->home = ci->focus;
	ev->comm = command_get(ci->comm2);
	ev->active = 0;
	ev->event = "event:poll";
	ev->num = -1;
	pane_add_notify(ei->home, ev->home, "Notify:Close");
	list_add(&ev->lst, &ei->event_list[POLL_LIST]);
	return 1;
}

DEF_CMD(libevent_on_idle)
{
	struct event_info *ei = container_of(ci->comm, struct event_info, on_idle);
	struct evt *ev;
	int prio = ci->num;

	if (!ci->comm2)
		return Enoarg;

	ev = malloc(sizeof(*ev));

	if (!ei->base)
		ei->base = event_base_new();

	ev->home = ci->focus;
	pane_add_notify(ei->home, ev->home, "Notify:Close");
	ev->comm = command_get(ci->comm2);
	ev->active = 0;
	ev->event = "event:on-idle";
	if (prio < 0)
		prio = 0;
	if (prio > 2)
		prio = 2;
	ev->num = prio;
	list_add(&ev->lst, &ei->event_list[PRIO_0_LIST + prio]);
	return 1;
}

static int run_list(struct event_info *ei safe, int list, char *cb safe,
		    bool stop_on_first)
{
	bool dont_block = False;
	struct evt *ev;

	list_for_each_entry(ev, &ei->event_list[list], lst) {
		ev->active = 1;
		if (comm_call(ev->comm, cb, ev->home, ev->num) >= 1)
			dont_block = True;
		if (ev->active == 2 || list >= PRIO_0_LIST) {
			list_del(&ev->lst);
			command_put(ev->comm);
			free(ev);
			break;
		} else
			ev->active = 0;
		if (dont_block && stop_on_first)
			/* Other things might have been removed from list */
			break;
	}
	return dont_block;
}

DEF_CB(libevent_run)
{
	struct event_info *ei = container_of(ci->comm, struct event_info, run);
	struct event_base *b = ei->base;
	int dont_block = ei->dont_block;
	struct evt *ev;
	int i;

	ei->dont_block = 0;

	if (ei->deactivated)
		return Efallthrough;
	if (!b) {
		/* No events to wait for.. */
		if (dont_block)
			return 1;
		return 0;
	}

	/* First run any 'poll' events */
	if (run_list(ei, POLL_LIST, "callback:poll", True))
		dont_block = True;

	for (i = PRIO_0_LIST ; i <= PRIO_2_LIST; i++)
		if (!list_empty(&ei->event_list[i]))
			dont_block = 1;

	/* Disable any alarm set by python (or other interpreter) */
	alarm(0);
	event_base_loop(b, EVLOOP_ONCE | (dont_block ? EVLOOP_NONBLOCK : 0));

	time_start(TIME_IDLE);
	/* Prio 2 comes first - unconditional */
	run_list(ei, PRIO_2_LIST, "callback:on-idle", False);
	/* Now prio1 */
	run_list(ei, PRIO_1_LIST, "callback:on-idle", False);
	/* Repeat PRIO_2 just in case */
	run_list(ei, PRIO_2_LIST, "callback:on-idle", False);
	/* And do one background task */
	run_list(ei, PRIO_0_LIST, "callback:on-idle", True);
	time_stop(TIME_IDLE);

	/* Check if we have been deactivated. */
	if (ei->base == b)
		return 1;

	for (i = 0 ; i < NR_LISTS; i++) {
		while (!list_empty(&ei->event_list[i])) {
			ev = list_first_entry(&ei->event_list[i], struct evt, lst);
			list_del(&ev->lst);
			if (i == EV_LIST) {
				event_del(ev->l);
				event_free(ev->l);
			}
			command_put(ev->comm);
			free(ev);
		}
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
	int i;

	for (i = 0; i < NR_LISTS; i++) {
		list_for_each_entry_safe(ev, tmp, &ei->event_list[i], lst)
			if (ev->home == ci->focus &&
			    (ci->comm2 == NULL || ev->comm == ci->comm2)) {
				list_del_init(&ev->lst);
				if (ev->active)
					ev->active = 2;
				else {
					if (i == EV_LIST) {
						event_del(ev->l);
						event_free(ev->l);
					}
					command_put(ev->comm);
					free(ev);
				}
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
	int i;

	for (i = 0; i < NR_LISTS; i++) {
		list_add(&old, &ei->event_list[i]);
		list_del_init(&ei->event_list[i]);
		list_for_each_entry_safe(ev, tmp, &old, lst) {
			if (i == EV_LIST) {
				event_del(ev->l);
				event_free(ev->l);
			}
			list_del(&ev->lst);
			call_comm(ev->event, ev->home, ev->comm, ev->num);
			command_put(ev->comm);
			free(ev);
		}
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
	int i;

	p = pane_register(pane_root(ci->home), 0, &libevent_handle.c);
	if (!p)
		return Efail;
	ei = p->data;
	ei->home = p;
	for (i = 0; i < NR_LISTS; i++)
		INIT_LIST_HEAD(&ei->event_list[i]);
	ei->read = libevent_read;
	ei->write = libevent_write;
	ei->signal = libevent_signal;
	ei->timer = libevent_timer;
	ei->poll = libevent_poll;
	ei->on_idle = libevent_on_idle;
	ei->run = libevent_run;
	ei->deactivate = libevent_deactivate;
	ei->free = libevent_free;
	ei->refresh = libevent_refresh;
	ei->noblock = libevent_noblock;

	/* These are defaults, so make them sort late */
	call_comm("global-set-command", ci->focus, &ei->read,
		  0, NULL, "event:read-zz");
	call_comm("global-set-command", ci->focus, &ei->write,
		  0, NULL, "event:write-zz");
	call_comm("global-set-command", ci->focus, &ei->signal,
		  0, NULL, "event:signal-zz");
	call_comm("global-set-command", ci->focus, &ei->timer,
		  0, NULL, "event:timer-zz");
	call_comm("global-set-command", ci->focus, &ei->poll,
		  0, NULL, "event:poll-zz");
	call_comm("global-set-command", ci->focus, &ei->on_idle,
		  0, NULL, "event:on-idle-zz");
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
}
