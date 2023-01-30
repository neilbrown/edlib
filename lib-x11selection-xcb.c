/*
 * Copyright Neil Brown ©2020-2022 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * xcbselection - integrate X11 clipboards with copybuf and selection.
 *
 * Use XCB directly to access PRIMARY and CLIPBOARD content in the X11
 * server and use it to access the selection and recent copied
 * content to other applications, and to use what is provided by those
 * applications to satisfy internal requests.
 *
 * "PRIMARY" represents a selection which only gets copied when a paste
 * request is made in another window.  "CLIPBOARD" represents content
 * that has already been copied.
 *
 * - If we don't own either PRIMARY or CLIPBOARD (as is initially the case),
 *   copy:get checks the timestamp on both (or whichever aren't owned) and
 *   compares with anything we previously collected.  If one is newer than
 *   newest, we collect it and copy:save it before falling through to let
 *   it be returned.
 * - If a local pane claims the selection, we claim PRIMARY.
 * - If a local pane creates a copy with copy:save, we claim PRIMARY and
 *   CLIPBOARD.
 * - If CLIPBOARD is requested while we own it, the result of "copy:get"
 *   is provided.
 * - If PRIMARY or CLIPBOARD is requested while we own it, we ask for the
 *   selection to be committed, then return the result of "copy:get".
 *   The commit happens for TIMESTAMP or content, but not for TARGETS.
 * - If copy:save is called locally (on this $DISPLAY) we claim both
 *   PRIMARY and CLIPBOARD.
 *
 *
 * We also claim the edlib selection at startup on behalf of whichever X11
 * application owns it.
 *
 * As multiple display panes could use the same X11 display, and it only
 * really makes sense to have a single selection manager per display,
 * the code that talks to X11 does not live in the pane stack.  We
 * create a global command "xcb-selection-$DISPLAY" which handles the
 * display, and any display-stack on that DISPLAY gets an xcb_display
 * pane which communicates with it.  The per-display selection becomes
 * shared among all displays with the same DISPLAY.  The xcb-common
 * command (which has a private pane for sending notifications) sits
 * between all the different displays and the X11 display and when any
 * claims the selection, it will claim from all the others.  When any
 * requests the selection be committed, it will notify the current owner
 * which will copy:save it.
 *
 * per-display pane handles:
 * - copy:save - ask xcb-common to claim both
 * - copy:get  - check if xcb-common can get clipboard content
 * - Notify:selection:claimed  - tell xcb-common that this display has
 *				 claimed selection
 * - Notify:selection:commit   - check if xcb-common can get selection
 *				 content
 *
 * When a mouse selection happens, the UI pane calls "selection:claim"
 * which will typically result in the xcb:display pane getting notified
 * that it losts the selection.  It tells the DISPLAY command
 * "selection-claim" which then notifies all related per-display panes to
 * claim their selections "Notify:xcb-claim", and claims PRIMARY on the
 * X11 server.
 *
 * When text is explicitly copied the UI pane calls "copy:save".  Our
 * per-display pane tells the DISPLAY command "clip-set" and it claims
 * both PRIMARY and CLIPBOARD from X11.
 *
 * When text is pasted via mouse, the UI pane calls "selection:commit"
 * and then "copy:get".  If some other display owns the selection, our
 * xcb-display pane is notified and sends "selection-commit" to
 * xcb-common.  If it doesn't own PRIMARY, it calls for the content of
 * PRIMARY copy:saves the result and claims the clipboard.  The
 * "copy:get" is then intercepted and xcb-common is asked for
 * "clip-get".  As it owns the clipboard it allows the request to fall
 * through.
 *
 * When text is yanked (pasted via keyboard), the "selection-commit"
 * isn't performed, so xcb-common won't have taken ownership of
 * CLIPBOARD.  So when copy:get calls "clip-get" xcb-common will
 * fetch content for CLIPBOARD if it exists and return that.
 *
 * When we lose ownership of PRIMARY, we tell all per-display panes
 * to claim the selection ("Notify:xcb-claim") on behalf of remote client.
 *
 * When we lose ownership of PRIMARY or CLIPBOARD, we discard any cached
 * content from other client.
 *
 * When content of CLIPBOARD is requested, we use copy:get to collect it.
 * When content of PRIMARY is requestd, we ask our per-display panes
 * to call selection:commit first ("Notify:xcb-commit", then do "copy:get".
 *
 * Some wisdom for understanding how to do this cam from xsel.c
 * Thanks Conrad Parker <conrad@vergenet.net> !!
 *
 */
#define _GNU_SOURCE for ppoll
#include <unistd.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>

#include "xcb.h"
#include "core.h"

enum my_atoms {
	a_TIMESTAMP, a_TARGETS, a_MULTIPLE, a_INCR,
	a_TEXT, a_STRING,
	a_text, a_textplain,
	a_COMPOUND_TEXT,
	a_UTF8_STRING,
	a_utf8, a_UTF8,
	a_NULL,
	a_CLIPBOARD, a_PRIMARY,
	a_XSEL_DATA,
	NR_ATOMS,
	NR_TARGETS = a_NULL,
};
static char *atom_names[NR_ATOMS] = {
	[a_TIMESTAMP]		= "TIMESTAMP",
	[a_TARGETS]		= "TARGETS",
	[a_MULTIPLE]		= "MULTIPLE",
	[a_INCR]		= "INCR",
	[a_TEXT]		= "TEXT",
	[a_STRING]		= "STRING",
	[a_UTF8_STRING]		= "UTF8_STRING",
	[a_COMPOUND_TEXT]	= "COMPOUND_TEXT",
	[a_text]		= "text",
	[a_textplain]		= "text/plain",
	[a_utf8]		= "text/plain;charset=utf-8",
	[a_UTF8]		= "text/plain;charset=UTF-8",
	[a_NULL]		= "NULL",
	[a_CLIPBOARD]		= "CLIPBOARD",
	[a_PRIMARY]		= "PRIMARY",
	[a_XSEL_DATA]		= "XSEL_DATA",
};

/* There are two different command maps.
 * xcb_common_map is attached to the common pane and handles
 *  requests sent to the DISPLAY command, and callbacks such as
 *  per-display panes closing.
 * xcb_display_map is attached to each per-display map and handles
 *  selection notifications and copy:* requests.
 * The DISPLAY command directs all valid request to the common
 * pane.
 */
static struct map *xcb_common_map, *xcb_display_map;
DEF_LOOKUP_CMD(xcb_common_handle, xcb_common_map);
DEF_LOOKUP_CMD(xcb_display_handle, xcb_display_map);

struct evlist {
	xcb_generic_event_t	*ev;
	struct evlist		*next;
};

struct xcbc_info {
	struct command		c;
	struct pane		*p safe;
	char			*display safe;
	xcb_connection_t	*conn safe;
	const xcb_setup_t	*setup safe;
	const xcb_screen_t	*screen safe;
	struct evlist		*head, **tail safe;
	int			maxlen;
	xcb_atom_t		atoms[NR_ATOMS];
	xcb_window_t		win;
	xcb_timestamp_t		last_save;
	xcb_timestamp_t		timestamp; // "now"
	xcb_timestamp_t		have_primary, have_clipboard;
	char			*pending_content;
	// targets??
};

static void collect_sel(struct xcbc_info *xci safe, enum my_atoms sel);
static xcb_timestamp_t collect_sel_stamp(struct xcbc_info *xci safe,
					 xcb_atom_t sel);
static void claim_sel(struct xcbc_info *xci safe, enum my_atoms sel);
static struct command *xcb_register(struct pane *p safe, char *display safe);
static void get_timestamp(struct xcbc_info *xci safe);

struct xcbd_info {
	struct command		*c safe;
	bool			committing;
};

DEF_CMD(xcbc_commit)
{
	/* Commit the selection - make it available for copy:get */
	struct xcbc_info *xci = ci->home->data;

	pane_notify("Notify:xcb-commit", xci->p);
	return 1;
}

DEF_CMD(xcbc_claim)
{
	/* claim the selection - so other X11 clients and other edlib
	 * displays can ask for it
	 */
	struct xcbc_info *xci = ci->home->data;

	home_pane_notify(xci->p, "Notify:xcb-claim", ci->focus);
	claim_sel(xci, a_PRIMARY);
	return 1;
}

DEF_CMD(xcbc_set)
{
	/* Claim the clipboard, because we just copied something to it */
	struct xcbc_info *xci = ci->home->data;

	claim_sel(xci, a_CLIPBOARD); // and primary
	return 1;
}

DEF_CMD(xcbc_get)
{
	/* If either PRIMARY or CLIPBOARD is newer than 'last_save',
	 * save it with "copy:save"
	 */
	struct xcbc_info *xci = ci->home->data;
	enum my_atoms best = a_NULL;
	xcb_timestamp_t ts;

	get_timestamp(xci);
	if (!xci->have_primary) {
		ts = collect_sel_stamp(xci, xci->atoms[a_PRIMARY]);
		if (ts > xci->last_save) {
			xci->last_save = ts;
			best = a_PRIMARY;
		}
	}
	if (!xci->have_clipboard) {
		ts = collect_sel_stamp(xci, xci->atoms[a_CLIPBOARD]);
		if (ts > xci->last_save) {
			xci->last_save = ts;
			best = a_CLIPBOARD;
		}
	}
	if (best != a_NULL)
		collect_sel(xci, best);
	return 1;
}

DEF_CMD(xcbc_register_display)
{
	pane_add_notify(ci->focus, ci->home, "Notify:xcb-claim");
	pane_add_notify(ci->focus, ci->home, "Notify:xcb-commit");

	pane_add_notify(ci->focus, ci->home, "Notify:xcb-check");
	pane_add_notify(ci->home, ci->focus, "Notify:Close");
	return 1;
}

DEF_CMD(xcbc_handle_close)
{
	/* A display window has closed.  If it was the last one we must
	 * close too, else we keep the x11 connection open unnecessarily
	 */
	if (pane_notify("Notify:xcb-check", ci->home) <= 0)
		pane_close(ci->home);
	return 1;
}

DEF_CMD(xcbc_close)
{
	struct xcbc_info *xci = ci->home->data;
	char *cn = strconcat(ci->home, "xcb-selection-", xci->display);

	call_comm("global-set-command", ci->home, &edlib_noop,
		  0, NULL, cn);
	xcb_disconnect(xci->conn);
	free(xci->display);
	free(xci->pending_content);
	while (xci->head) {
		struct evlist *evl = xci->head;
		xci->head = evl->next;
		free(evl->ev);
		free(evl);
	}

	return 1;
}

DEF_CMD(xcbd_copy_save)
{
	struct xcbd_info *xdi = ci->home->data;

	comm_call(xdi->c, "clip-set", ci->home);
	return Efallthrough;
}

DEF_CMD(xcbd_copy_get)
{
	struct xcbd_info *xdi = ci->home->data;

	if (ci->num == 0)
		/* If there is a selection, copy it now */
		comm_call(xdi->c, "clip-get", ci->home);
	return Efallthrough;
}

DEF_CMD(xcbd_sel_claimed)
{
	/* Something else on this display is claiming the selection.
	 * We need to tell the common pane to claim from elsewhere
	 * as a proxy.  When it trys to claim back from us,
	 * we will know to ignore because ->focus will be us.
	 */
	struct xcbd_info *xdi = ci->home->data;
	comm_call(xdi->c, "selection-claim", ci->home);
	return Efallthrough;
}

DEF_CMD(xcbd_sel_commit)
{
	struct xcbd_info *xdi = ci->home->data;

	if (ci->focus != ci->home)
		/* Wasn't explicitly addressed to me, so must be for
		 * some other pane (probably mode-emacs will handle it)
		 * so just fall through
		 */
		return Efallthrough;
	if (!xdi->committing)
		comm_call(xdi->c, "selection-commit", ci->home);
	/* '2' means 'call me again if someone else commits, I won't refresh
	 * my ownership.
	 */
	return 2;
}

DEF_CMD(xcbd_do_claim)
{
	if (ci->focus == ci->home)
		/* I'm asking for this, because my UI is claiming,
		 * so I must ignore.
		 */
		return Efallthrough;
	call("selection:claim", ci->home);
	return 1;
}

DEF_CMD(xcbd_do_commit)
{
	struct xcbd_info *xdi = ci->home->data;

	xdi->committing = True;
	call("selection:commit", ci->home);
	xdi->committing = False;
	return 1;
}

DEF_CMD(xcbc_do_check)
{
	if (!(ci->home->damaged & DAMAGED_CLOSED))
		/* Yes, I'm still here */
		return 1;
	return Efallthrough;
}

DEF_CMD(xcb_common)
{
	struct xcbc_info *xci = container_of(ci->comm, struct xcbc_info, c);

	if (ci->home != ci->focus)
		/* Not called via comm_call() retreived with global-get-command */
		return Efallthrough;
	return pane_call(xci->p, ci->key, ci->focus,
			 ci->num, ci->mark, ci->str,
			 ci->num2, ci->mark2, ci->str2,
			 ci->x, ci->y, ci->comm2);
}

static void xcbc_free_cmd(struct command *c safe)
{
	struct xcbc_info *xci = container_of(c, struct xcbc_info, c);

	pane_close(xci->p);
}

DEF_CMD(xcbd_attach)
{
	struct xcbd_info *xdi;
	char *d;
	char *cn;
	struct command *c;
	struct pane *p;

	d = pane_attr_get(ci->focus, "DISPLAY");
	if (!d || !*d)
		return Efalse;

	cn = strconcat(ci->focus, "xcb-selection-", d);
	if (!cn)
		return Efail;
	c = call_ret(comm, "global-get-command", ci->focus, 0, NULL, cn);
	if (!c) {
		c = xcb_register(ci->focus, d);
		if (!c)
			return Efail;
		call_comm("global-set-command", ci->focus, c,
			  0, NULL, cn);
	}
	alloc(xdi, pane);
	xdi->c = c;
	p = pane_register(ci->focus, 0, &xcb_display_handle.c, xdi);
	if (!p) {
		unalloc(xdi, pane);
		return Efail;
	}
	comm_call(c, "register", p);
	call("selection:claim", p, 1);
	comm_call(ci->comm2, "cb", p);
	return 1;
}

static void handle_property_notify(struct xcbc_info *xci,
				   xcb_property_notify_event_t *pne)
{
	// FIXME Later - for INCR replies

	return;
}

static void handle_selection_clear(struct xcbc_info *xci safe,
				   xcb_selection_clear_event_t *sce safe)
{
	if (sce->selection == xci->atoms[a_PRIMARY]) {
		xci->have_primary = XCB_CURRENT_TIME;
		pane_notify("Notify:xcb-claim", xci->p);
	}
	if (sce->selection == xci->atoms[a_CLIPBOARD])
		xci->have_clipboard = XCB_CURRENT_TIME;
	return;
}

static void store_content(struct xcbc_info *xci safe, xcb_window_t requestor,
			  xcb_atom_t prop, xcb_atom_t target,
			  char *content safe)
{
	int len = strlen(content);
	int pos = 0;
	int send;
	int max = (xci->maxlen+1)/2;
	xcb_void_cookie_t c;
	xcb_generic_error_t *ret = NULL;

	send = len;
	if (send > max)
		send = max;

	c = xcb_change_property_checked(xci->conn,
					XCB_PROP_MODE_REPLACE, requestor,
					prop, target,
					8, send,
					content + pos);
	ret = xcb_request_check(xci->conn, c);
	while ((!ret || ret->error_code == 0) && pos+send < len) {
		/* Need to send some more */
		pos += send;
		send = len - pos;
		if (send > max)
			send = max;
		c = xcb_change_property_checked(
			xci->conn,
			XCB_PROP_MODE_APPEND, requestor,
			prop, target,
			8, send,
			content + pos);
		free(ret);
		ret = xcb_request_check(xci->conn, c);
	}
	if (!ret || ret->error_code != XCB_ALLOC) {
		free(ret);
		free(content);
		return;
	}
	/* Got an ALLOC error, need to do INCR sent */
	/* Possibly shoul do INCR much earlier.  Don't want too much
	 * wastage.  But then... I can send 2M without INCR,
	 * maybe there isn't a limit?
	 */
	LOG("Need to do INCR send after %d", pos);
	free(content);
}

static void handle_selection_request(struct xcbc_info *xci safe,
				     xcb_selection_request_event_t *sre safe)
{
	int a;
	xcb_selection_notify_event_t sne = {};
	xcb_timestamp_t when = XCB_CURRENT_TIME;
	// FIXME
	// convert to iso-8859-15 for COMPOUND_TEXT and may for
	// TEXT
	//LOG("sel request %d", sre->target);

	sne.response_type = XCB_SELECTION_NOTIFY;
	sne.time = sre->time;
	sne.requestor = sre->requestor;
	sne.selection = sre->selection;
	sne.target = sre->target;
	sne.property = sre->property;

	for (a = 0; a < NR_TARGETS; a++)
		if (xci->atoms[a] == sre->target)
			break;

	if (sre->selection == xci->atoms[a_PRIMARY])
		when = xci->have_primary;
	if (sre->selection == xci->atoms[a_CLIPBOARD])
		when = xci->have_clipboard;

	if (when == XCB_CURRENT_TIME) {
		/* I want to require sre->time >= when, but sometimes it isn't. */
		LOG("x11selection-xcb request for selection not held %d %d %d",
		    sre->selection, when, sre->time);
		sne.property = XCB_ATOM_NONE;
	} else if (a >= NR_TARGETS) {
		LOG("unknown target %d", sre->target);
		sne.property = XCB_ATOM_NONE;
	} else if (a >= a_TEXT) {
		char *content = NULL;
		// LOG("Request for text");
		if ((sre->selection == xci->atoms[a_PRIMARY] &&
		     xci->have_primary) ||
		    (sre->selection == xci->atoms[a_CLIPBOARD] &&
		     xci->have_clipboard)) {
			pane_notify("Notify:xcb-commit", xci->p);
			content = call_ret(str, "copy:get", xci->p);
		}
		LOG("Returning content for %s: %.20s",
		    sre->selection == xci->atoms[a_PRIMARY] ? "PRIMARY" : "CLIPBOARD",
		    content);
		if (content)
			store_content(xci, sre->requestor, sre->property,
				      sre->target, content);
		else
			sne.property = XCB_ATOM_NONE;
	} else if (a == a_TIMESTAMP) {
		// LOG("Sending timestamp");
		/* If there is a new selection, this will reclaim and update
		 * timestamp.
		 */
		pane_notify("Notify:xcb-commit", xci->p);
		xcb_change_property(xci->conn,
				    XCB_PROP_MODE_REPLACE, sre->requestor,
				    sre->property, XCB_ATOM_INTEGER, 32,
				    1, &when);
	} else if (a == a_TARGETS) {
		// LOG("Sending targets to %d", sre->requestor);
		xcb_change_property(xci->conn,
				    XCB_PROP_MODE_REPLACE, sre->requestor,
				    sre->property, XCB_ATOM_ATOM, 32,
				    NR_TARGETS, xci->atoms);
	} else if (a == a_MULTIPLE) {
		LOG("Failing MULTIPLE");
		// FIXME
		sne.property = XCB_ATOM_NONE;
	} else if (a == a_INCR) {
		LOG("Failing INCR");
		// FIXME
		sne.property = XCB_ATOM_NONE;
	}
	xcb_send_event(xci->conn, 0, sre->requestor, 0, (void*)&sne);
	xcb_flush(xci->conn);
	return;
}

#define Sec (1000 * 1000 * 1000)
#define Msec (1000 * 1000)

static xcb_generic_event_t *__xcb_wait_for_event_timeo(
	xcb_connection_t *conn, unsigned int request,
	xcb_generic_error_t **e, int msecs)
{
	struct timespec now, delay, deadline;
	struct pollfd pfd;
	xcb_generic_event_t *ev;

	if (!conn)
		return NULL;
	clock_gettime(CLOCK_MONOTONIC_COARSE, &deadline);
	deadline.tv_nsec += msecs * Msec;
	if (deadline.tv_nsec >= Sec) {
		deadline.tv_sec += 1;
		deadline.tv_nsec -= Sec;
	}
	if (request)
		xcb_flush(conn);
	while (1) {
		if (request) {
			if (xcb_poll_for_reply(conn, request, (void**)&ev, e))
				return ev;
		} else {
			ev = xcb_poll_for_event(conn);
			if (ev)
				return ev;
		}
		pfd.fd = xcb_get_file_descriptor(conn);
		pfd.events = POLLIN;
		pfd.revents = 0;
		clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
		if (now.tv_sec > deadline.tv_sec)
			return NULL;
		if (now.tv_sec == deadline.tv_sec &&
		    now.tv_nsec >= deadline.tv_nsec)
			return NULL;
		delay.tv_sec = deadline.tv_sec - now.tv_sec;
		if (deadline.tv_nsec >= now.tv_nsec)
			delay.tv_nsec = deadline.tv_nsec - now.tv_nsec;
		else {
			delay.tv_sec -= 1;
			delay.tv_nsec = Sec + deadline.tv_nsec - now.tv_nsec;
		}
		if (delay.tv_sec == 0 && delay.tv_nsec < Msec)
			/* Assume coarse granularity is at least 1ms */
			return NULL;
		if (ppoll(&pfd, 1, &delay, NULL) < 0)
			return NULL;
	}
}

#define xcb_wait_for_event_timeo(c, ms)		\
	__xcb_wait_for_event_timeo(c, 0, NULL, ms)
#define xcb_wait_for_reply_timeo(c, rq, e, ms)		\
	__xcb_wait_for_event_timeo(c, rq, e, ms)

#define REPLY_TIMEO 50

#define DECL_REPLY_TIMEO(req)						\
	static inline xcb_##req##_reply_t *				\
	xcb_##req##_reply_timeo(xcb_connection_t *c,			\
				xcb_##req##_cookie_t cookie,		\
				xcb_generic_error_t **e)		\
	{								\
		return (xcb_##req##_reply_t *)				\
			xcb_wait_for_reply_timeo(c, cookie.sequence,	\
						 e, REPLY_TIMEO);	\
	}

DECL_REPLY_TIMEO(get_property)
DECL_REPLY_TIMEO(get_selection_owner)
DECL_REPLY_TIMEO(intern_atom)

static xcb_generic_event_t *wait_for(struct xcbc_info *xci safe,
				     uint8_t type)
{
	struct evlist *evl;
	xcb_generic_event_t *ev;

	xcb_flush(xci->conn);
	while ((ev = xcb_wait_for_event_timeo(xci->conn, 500)) != NULL) {
		if ((ev->response_type & 0x7f) == type)
			return ev;
		// LOG("Got %x wanted %x", ev->response_type & 0x7f, type);
		if (!xci->head)
			xci->tail = &xci->head;
		alloc(evl, pane);
		evl->ev = ev;
		evl->next = NULL;
		*xci->tail = evl;
		xci->tail = &evl->next;
	}
	return NULL;
}

static xcb_generic_event_t *next_event(struct xcbc_info *xci safe)
{
	if (xci->head) {
		struct evlist *evl = xci->head;
		xcb_generic_event_t *ev;
		xci->head = evl->next;
		if (!xci->head)
			xci->tail = &xci->head;
		ev = evl->ev;
		unalloc(evl, pane);
		return ev;
	}
	return xcb_poll_for_event(xci->conn);
}

DEF_CMD(xcbc_input)
{
	struct xcbc_info *xci = ci->home->data;
	xcb_generic_event_t *ev;
	int ret = 1;

	if (ci->num < 0)
		/* This is a poll - only return 1 on something happening */
		ret = Efalse;

	while((ev = next_event(xci)) != NULL) {
		switch (ev->response_type & 0x7f) {
			xcb_property_notify_event_t *pne;
			xcb_selection_clear_event_t *sce;
			xcb_selection_request_event_t *sre;
		case XCB_PROPERTY_NOTIFY:
			pne = (void*)ev;
			xci->timestamp = pne->time;
			if (pne->state == XCB_PROPERTY_DELETE)
				// might need to provide more content
				handle_property_notify(xci, pne);
			break;
		case XCB_SELECTION_CLEAR:
			/* Someone claimed my select */
			sce = (void*)ev;
			handle_selection_clear(xci, sce);
			break;
		case XCB_SELECTION_REQUEST:
			/* Someone wants my content */
			sre = (void*)ev;
			handle_selection_request(xci, sre);
			break;
		}
		free(ev);
	}
	xcb_flush(xci->conn);
	if (xcb_connection_has_error(xci->conn))
		pane_close(ci->home);
	return ret;
}

static void get_timestamp(struct xcbc_info *xci safe)
{
	/* We need a timestamp - use property change which appends
	 * nothing to WM_NAME to get it.  This will cause a
	 * XCB_PROPERT_NOTIFY event.
	 */
	xcb_generic_event_t *ev;
	xcb_property_notify_event_t *pev;

	xcb_change_property(xci->conn, XCB_PROP_MODE_APPEND, xci->win,
			    XCB_ATOM_WM_NAME, XCB_ATOM_STRING,
			    8, 0, NULL);
	ev = wait_for(xci, XCB_PROPERTY_NOTIFY);
	if (!ev)
		return;
	pev = (void*)ev;
	xci->timestamp = pev->time;
	free(ev);
}

static void claim_sel(struct xcbc_info *xci safe, enum my_atoms sel)
{
	/* Always get primary, maybe get clipboard */
	xcb_get_selection_owner_cookie_t pck, cck;
	xcb_get_selection_owner_reply_t *rep;

	get_timestamp(xci);
	xcb_set_selection_owner(xci->conn, xci->win,
				xci->atoms[a_PRIMARY], xci->timestamp);
	if (sel != a_PRIMARY)
		xcb_set_selection_owner(xci->conn, xci->win,
					xci->atoms[sel], xci->timestamp);
	pck = xcb_get_selection_owner(xci->conn, xci->atoms[a_PRIMARY]);
	if (sel != a_PRIMARY)
		cck = xcb_get_selection_owner(xci->conn,
					      xci->atoms[sel]);
	rep = xcb_get_selection_owner_reply_timeo(xci->conn, pck, NULL);
	if (rep && rep->owner == xci->win)
		xci->have_primary = xci->timestamp;
	else {
		LOG("failed to claim primary - have = %u",
		    (unsigned int)xci->have_primary);
		xci->have_primary = XCB_CURRENT_TIME;
	}
	free(rep);
	if (sel != a_PRIMARY) {
		rep = xcb_get_selection_owner_reply_timeo(xci->conn, cck,
							  NULL);
		if (rep && rep->owner == xci->win)
			xci->have_clipboard = xci->timestamp;
		else {
			LOG("failed to claim clipboard - have = %u",
			    (unsigned int)xci->have_clipboard);
			xci->have_clipboard = XCB_CURRENT_TIME;
		}
		free(rep);
	}
}

static char *collect_incr(struct xcbc_info *xci safe,
			  xcb_atom_t self, int size_est)
{
	// FIXME;
	xcb_delete_property(xci->conn, xci->win, xci->atoms[a_XSEL_DATA]);
	return NULL;
}

static xcb_timestamp_t collect_sel_stamp(struct xcbc_info *xci safe,
					 xcb_atom_t sel)
{
	xcb_generic_event_t *ev;
	xcb_selection_notify_event_t *nev;
	xcb_get_property_cookie_t gpc;
	xcb_get_property_reply_t *gpr;
	void *val;
	unsigned int len;
	xcb_timestamp_t ret = XCB_CURRENT_TIME;

	xcb_convert_selection(xci->conn, xci->win, sel,
			      xci->atoms[a_TIMESTAMP],
			      xci->atoms[a_XSEL_DATA], xci->timestamp);
	ev = wait_for(xci, XCB_SELECTION_NOTIFY);
	if (!ev)
		return ret;
	nev = (void*)ev;
	if (nev->requestor != xci->win || nev->selection != sel ||
	    nev->property == XCB_ATOM_NONE) {
		free(ev);
		return ret;
	}
	gpc = xcb_get_property(xci->conn, 0, xci->win, nev->property,
			       XCB_ATOM_INTEGER, 0, 4);
	gpr = xcb_get_property_reply_timeo(xci->conn, gpc, NULL);
	if (!gpr) {
		free(ev);
		return ret;
	}
	val = xcb_get_property_value(gpr);
	len = xcb_get_property_value_length(gpr);
	if (gpr->type == XCB_ATOM_INTEGER && len == 4 && val)
		ret = *(uint32_t*)val;
	xcb_delete_property(xci->conn, xci->win, xci->atoms[a_XSEL_DATA]);
	free(gpr);
	free(ev);
	return ret;
}

static char *collect_sel_type(struct xcbc_info *xci safe,
			      xcb_atom_t sel, xcb_atom_t target)
{
	xcb_generic_event_t *ev;
	xcb_selection_notify_event_t *nev;
	xcb_get_property_cookie_t gpc;
	xcb_get_property_reply_t *gpr = NULL;
	void *val;
	unsigned int len;
	char *ret = NULL;
	int start = 0;
	unsigned int total;

	xcb_convert_selection(xci->conn, xci->win, sel, target,
			      xci->atoms[a_XSEL_DATA], xci->timestamp);
	ev = wait_for(xci, XCB_SELECTION_NOTIFY);
	if (!ev)
		return NULL;
	nev = (void*)ev;
	if (nev->requestor != xci->win || nev->selection != sel ||
	    nev->property == XCB_ATOM_NONE) {
		LOG("not for me  %d/%d %d/%d %d/%d", nev->requestor, xci->win,
		    nev->selection, sel, nev->property, XCB_ATOM_NONE);
		free(ev);
		return NULL;
	}
	gpc = xcb_get_property(xci->conn, 0, xci->win, nev->property,
			       XCB_GET_PROPERTY_TYPE_ANY, start/4,
			       xci->maxlen/4/2);
	gpr = xcb_get_property_reply_timeo(xci->conn, gpc, NULL);
	if (!gpr) {
		LOG("get property reply failed");
		goto abort;
	}
	val = xcb_get_property_value(gpr);
	len = xcb_get_property_value_length(gpr);
	if (gpr->type == xci->atoms[a_INCR] && len >= sizeof(uint32_t) && val) {
		ret = collect_incr(xci, sel, *(uint32_t*)val);
		free(gpr);
		free(ev);
		return ret;
	}
	if (gpr->format != 8) {
		LOG("get_propery_value reported unsupported type: %d",
		    gpr->format);
		free(gpr);
		goto abort;
	}
	total = len + gpr->bytes_after + 1;
	ret = malloc(total);
	memcpy(ret+start, val, len);
	while (len && gpr->bytes_after > 0) {
		start += len;
		gpc = xcb_get_property(xci->conn, 0, xci->win, nev->property,
				       gpr->type, start/4, xci->maxlen/4/2);
		free(gpr);
		gpr = xcb_get_property_reply_timeo(xci->conn, gpc, NULL);
		if (!gpr) {
			LOG("get property reply failed");
			goto abort;
		}
		val = xcb_get_property_value(gpr);
		len = xcb_get_property_value_length(gpr);
		if (start + len >= total)
			len = total - 1 - start;
		memcpy(ret+start, val, len);
	}
	ret[start + len] = '\0';

	xcb_delete_property(xci->conn, xci->win, xci->atoms[a_XSEL_DATA]);
	free(gpr);
	free(ev);
	return ret;
abort:
	free(ev);
	free(ret);
	return NULL;
}

static void strip_cr(char *from safe)
{
	char *to = from;

	while ((*to = *from)) {
		from++;
		if (*to != '\r')
			to++;
	}
}

static void collect_sel(struct xcbc_info *xci safe, enum my_atoms sel)
{
	/* If 'sel' can be fetched, copy:save it. */
	char *ret = NULL;
	enum my_atoms a;
	xcb_selection_notify_event_t *nev;
	xcb_get_property_reply_t *gpr = NULL;
	xcb_atom_t *targets = NULL,
		dflt[] = {XCB_ATOM_STRING, xci->atoms[a_TEXT]};
	int ntargets;
	int i;

	xcb_convert_selection(xci->conn, xci->win, xci->atoms[sel],
			      xci->atoms[a_TARGETS],
			      xci->atoms[a_XSEL_DATA], xci->timestamp);
	nev = (void*)wait_for(xci, XCB_SELECTION_NOTIFY);
	if (nev && nev->requestor == xci->win &&
	    nev->selection == xci->atoms[sel] &&
	    nev->property != XCB_ATOM_NONE) {
		xcb_get_property_cookie_t gpc;
		gpc = xcb_get_property(xci->conn, 0, xci->win, nev->property,
				       XCB_ATOM_ATOM, 0,
				       xci->maxlen/4/2);
		gpr = xcb_get_property_reply_timeo(xci->conn, gpc, NULL);
		if (gpr && gpr->type == XCB_ATOM_ATOM && gpr->format == 32) {
			targets = (void*) xcb_get_property_value(gpr);
			ntargets = gpr->value_len;
		}
	}
	if (!targets) {
		targets = dflt;
		ntargets = ARRAY_SIZE(dflt);
	}

	for (a = NR_TARGETS - 1; !ret && a >= a_TEXT; a -= 1) {
		for (i = 0; i < ntargets; i++)
			if (targets[i] == xci->atoms[a])
				/* This one please */
				break;
		if (i < ntargets)
			ret = collect_sel_type(xci, xci->atoms[sel],
					       targets[i]);
	}
	if (ret) {
		strip_cr(ret);
		LOG("copy:save from %s selection: %.20s",
		    sel == a_CLIPBOARD ? "CLIPBOARD" : "PRIMARY",
		    ret);
		call("copy:save", xci->p, 0, NULL, ret);
		free(ret);
	}
	free(gpr);
	free(nev);
}

static struct command *xcb_register(struct pane *p safe, char *display safe)
{
	struct xcbc_info *xci;
	struct pane *p2;
	xcb_connection_t *conn;
	xcb_intern_atom_cookie_t cookies[NR_ATOMS];
	xcb_screen_iterator_t iter;
	uint32_t valwin[1];
	int screen;
	int i;
	char *disp_auth;

	disp_auth = pane_attr_get(p, "XAUTHORITY");
	if (!disp_auth)
		disp_auth = getenv("XAUTHORITY");

	conn = xcb_connect_auth(display, disp_auth, &screen);
	if (!conn || xcb_connection_has_error(conn))
		return NULL;

	alloc(xci, pane);

	xci->conn = conn;
	xci->display = strdup(display);
	xci->setup = safe_cast xcb_get_setup(conn);
	xci->maxlen = xci->setup->maximum_request_length;
	iter = xcb_setup_roots_iterator(xci->setup);
	for (i = 0; i < screen; i++)
		xcb_screen_next(&iter);
	xci->screen = safe_cast iter.data;

	for (i = 0; i < NR_ATOMS; i++) {
		char *n = atom_names[i];
		if (!n)
			continue;
		cookies[i] = xcb_intern_atom(conn, 0, strlen(n), n);
	}
	/* Need a dedicated (invisible) window to getting events */
	xci->win = xcb_generate_id(conn);
	valwin[0] = XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, xci->win,
				    xci->screen->root,
				    0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
				    xci->screen->root_visual,
				    XCB_CW_EVENT_MASK, valwin);
	get_timestamp(xci);
	xcb_flush(conn);
	/* Now resolve all those cookies */
	for (i = 0; i < NR_ATOMS; i++) {
		xcb_intern_atom_reply_t *r;
		r = xcb_intern_atom_reply_timeo(conn, cookies[i], NULL);
		if (!r)
			goto abort;
		xci->atoms[i] = r->atom;
		free(r);
	}

	xci->c = xcb_common;
	xci->c.free = xcbc_free_cmd;
	p2 = pane_register(pane_root(p), 0, &xcb_common_handle.c, xci);
	if (!p2)
		goto abort;
	xci->p = p2;
	call_comm("event:read", p2, &xcbc_input, xcb_get_file_descriptor(conn));
	call_comm("event:poll", p2, &xcbc_input);
	// LOG("registered xcb %d %d", xcb_get_file_descriptor(conn), xci->win);
	return &xci->c;
abort:
	xcb_disconnect(conn);
	free(xci->display);
	unalloc(xci, pane);
	return NULL;
}

void edlib_init(struct pane *ed safe)
{
	if (!xcb_common_map) {
		xcb_common_map = key_alloc();
		key_add(xcb_common_map, "selection-commit", &xcbc_commit);
		key_add(xcb_common_map, "selection-claim", &xcbc_claim);

		key_add(xcb_common_map, "clip-set", &xcbc_set);
		key_add(xcb_common_map, "clip-get", &xcbc_get);

		key_add(xcb_common_map, "register", &xcbc_register_display);
		key_add(xcb_common_map, "Notify:Close", &xcbc_handle_close);

		key_add(xcb_common_map, "Close", &xcbc_close);
		key_add(xcb_common_map, "Free", &edlib_do_free);

		xcb_display_map = key_alloc();
		key_add(xcb_display_map, "copy:save", &xcbd_copy_save);
		key_add(xcb_display_map, "copy:get", &xcbd_copy_get);
		key_add(xcb_display_map, "Notify:selection:claimed",
			&xcbd_sel_claimed);
		key_add(xcb_display_map, "Notify:selection:commit",
			&xcbd_sel_commit);
		key_add(xcb_display_map, "Notify:xcb-claim",
			&xcbd_do_claim);
		key_add(xcb_display_map, "Notify:xcb-commit",
			&xcbd_do_commit);
		key_add(xcb_display_map, "Notify:xcb-check",
			&xcbc_do_check);

		key_add(xcb_display_map, "Free", &edlib_do_free);
	}

	call_comm("global-set-command", ed, &xcbd_attach,
		  0, NULL, "attach-x11selection");
}
