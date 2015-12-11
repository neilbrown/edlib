/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * libevent support for edlib.
 *
 * Register command "libevent:activate".
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
};

static void call_event(int thing, short sev, void *evv)
{
	struct evt *ev = evv;
	struct cmd_info ci = {0};

	ci.key = "callback:event";
	ci.home = ci.focus = ev->home;
	ci.comm = ev->comm;
	ci.numeric = thing;
	ev->comm->func(&ci);
}

DEF_CMD(libevent_read)
{
	struct evt *ev = malloc(sizeof(*ev));
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
	struct evt *ev = malloc(sizeof(*ev));
	ev->l = event_new(base, ci->numeric, EV_SIGNAL|EV_PERSIST,
			  call_event, ev);
	ev->home = ci->focus;
	ev->comm = ci->comm2;
	list_add(&ev->lst, &event_list);
	event_add(ev->l, NULL);
	return 1;
}

DEF_CMD(libevent_run)
{
	struct event_base *b = base;
	if (!b)
		return 0;

	event_base_dispatch(b);
	while (!list_empty(&event_list)) {
		struct evt *ev = list_first_entry(&event_list, struct evt, lst);
		list_del(&ev->lst);
		event_del(ev->l);
		event_free(ev->l);
	}
	event_base_free(b);
	return 1;
}

DEF_CMD(libevent_deactivate)
{
	event_base_loopbreak(base);
	base = NULL;
	return 1;
}

DEF_CMD(libevent_activate)
{
	if (base)
		return 1;
	base = event_base_new();
	INIT_LIST_HEAD(&event_list);

	call_comm("global-set-command", ci->focus, 0, NULL, "event:read",
		  0, &libevent_read);
	call_comm("global-set-command", ci->focus, 0, NULL, "event:signal",
		  0, &libevent_signal);
	call_comm("global-set-command", ci->focus, 0, NULL, "event:run",
		  0, &libevent_run);
	call_comm("global-set-command", ci->focus, 0, NULL, "event:deactivate",
		  0, &libevent_deactivate);

	return 1;
}

void edlib_init(struct editor *ed)
{
	call_comm("global-set-command", &ed->root, 0, NULL, "libevent:activate",
		  0, &libevent_activate);
}
