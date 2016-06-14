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
	struct event *l;
	struct pane *home;
	struct command *comm;
	struct list_head lst;
	int seconds;
};

static void call_event(int thing, short sev, void *evv)
{
	struct evt *ev = evv;
	struct cmd_info ci = {};

	ci.key = "callback:event";
	ci.home = ci.focus = ev->home;
	ci.comm = ev->comm;
	ci.numeric = thing;
	if (ev->comm->func(&ci) < 0) {
		event_del(ev->l);
		event_free(ev->l);
		list_del(&ev->lst);
		free(ev);
	}
}

static void call_timeout_event(int thing, short sev, void *evv)
{
	struct evt *ev = evv;
	struct cmd_info ci = {};

	ci.key = "callback:event";
	ci.home = ci.focus = ev->home;
	ci.comm = ev->comm;
	ci.numeric = thing;
	if (ev->comm->func(&ci) <= 0) {
		event_free(ev->l);
		list_del(&ev->lst);
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

	ev = malloc(sizeof(*ev));

	if (!base)
		base = event_base_new();

	ev->l = event_new(base, ci->numeric, EV_READ|EV_PERSIST,
			  call_event, ev);
	ev->home = ci->focus;
	ev->comm = ci->comm2;
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

	ev->l = event_new(base, ci->numeric, EV_SIGNAL|EV_PERSIST,
			  call_event, ev);
	ev->home = ci->focus;
	ev->comm = ci->comm2;
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

	ev->l = event_new(base, -1, 0,
			  call_timeout_event, ev);
	ev->home = ci->focus;
	ev->comm = ci->comm2;
	ev->seconds = ci->numeric;
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
		  0, &libevent_read);
	call_comm("global-set-command", ci->focus, 0, NULL, "event:signal-zz",
		  0, &libevent_signal);
	call_comm("global-set-command", ci->focus, 0, NULL, "event:timer-zz",
		  0, &libevent_timer);
	call_comm("global-set-command", ci->focus, 0, NULL, "event:run-zz",
		  0, &libevent_run);
	call_comm("global-set-command", ci->focus, 0, NULL, "event:deactivate-zz",
		  0, &libevent_deactivate);

	return 1;
}

void edlib_init(struct pane *ed)
{
	INIT_LIST_HEAD(&event_list);
	call_comm("global-set-command", ed, 0, NULL, "attach-libevent",
		  0, &libevent_activate);
}
