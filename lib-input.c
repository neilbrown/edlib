/*
 * Copyright Neil Brown ©2015-2022 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Core input translation.
 * This module transalates keystrokes and mouse events into commands.
 * This involves tracking the current 'mode' state.
 *
 */

#define _GNU_SOURCE /*  for asprintf */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

#include "core.h"

#define LOGSIZE 128
struct input_mode {
	const char	*mode safe;
	const char	*context safe; /* for status lines */
	int		num, num2;
	struct pane	*focus, *source, *grab;
	struct mark	*point;
	struct mouse_state {
		struct timespec	last_action;
		int		last_x, last_y;
		int		click_count;
		int		click_on_up;
		char		*mod;
	} buttons[3];

	char		*log[LOGSIZE];
	int		head;
};

/* 'head' is 1 more than the last key added. */
static void log_add(struct input_mode *im safe,
		    const char *type safe, const char *key safe)
{
	free(im->log[im->head]);
	im->log[im->head] = strconcat(NULL, type, key);
	im->head = (im->head + 1) % LOGSIZE;
}

static void log_dump(struct pane *p safe)
{
	struct input_mode *im = p->data;
	int i;
	int cnt = 0;
	char *line = NULL;

	i = im->head;
	do {
		if (!im->log[i])
			continue;
		if (line)
			line = strconcat(p, line, ", ", im->log[i]);
		else
			line = strconcat(p, im->log[i]);
		cnt += 1;
		if (cnt % 10 == 0) {
			LOG("Input %d: %s", cnt/10, line);
			line = NULL;
		}
	} while ((i = (i+1) % LOGSIZE) != im->head);

	if (line)
		LOG("Input: %s", line);
}

DEF_CMD(log_input)
{
	log_dump(ci->home);
	return 1;
}

static void report_status(struct pane *focus safe, struct input_mode *im safe)
{
	char *st = NULL;

	if (im->num == NO_NUMERIC && im->mode[0] == 0)
		return;

	if (im->num == NO_NUMERIC)
		asprintf(&st, " %s", im->mode);
	else if (im->num == -NO_NUMERIC)
		asprintf(&st, " - %s", im->mode);
	else
		asprintf(&st, " %d %s", im->num, im->mode);
	call("Message:modal", focus, 0, NULL, st);
}

DEF_CMD(set_mode)
{
	struct input_mode *im = ci->home->data;

	if (ci->str) {
		free((void*)im->mode);
		im->mode = strdup(ci->str);
	}
	if (ci->str2) {
		free((void*)im->context);
		im->context = strdup(ci->str2);
		attr_set_str(&ci->home->attrs, "display-context", im->context);
		call("window:notify:display-context", ci->home);
	}
	report_status(ci->focus, im);
	return 1;
}

DEF_CMD(set_num)
{
	struct input_mode *im = ci->home->data;

	im->num = ci->num;
	report_status(ci->focus, im);
	return 1;
}

DEF_CMD(set_num2)
{
	struct input_mode *im = ci->home->data;

	im->num2 = ci->num;
	return 1;
}

DEF_CMD(set_all)
{
	struct input_mode *im = ci->home->data;

	if (ci->str) {
		free((void*)im->mode);
		im->mode = strdup(ci->str);
	}
	if (ci->str2) {
		free((void*)im->context);
		im->context = strdup(ci->str2);
		attr_set_str(&ci->home->attrs, "display-context", im->context);
		call("window:notify:display-context", ci->home);
	}
	im->num = ci->num;
	im->num2 = ci->num;
	report_status(ci->focus, im);
	return 1;
}

static const char *safe ctrl_map[][2] = {
	{ ":Backspace",	":C-H" },
	{ ":Enter",	":C-M" },
	{ ":ESC",	":C-[" },
	{ ":LF",	":C-J" },
	{ ":Tab",	":C-I" },
	{ ":Del",	":C-?" },
	{ ":A:Backspace",":A:C-H" },
	{ ":A:Enter",	":A:C-M" },
	{ ":A:ESC",	":A:C-[" },
	{ ":A:LF",	":A:C-J" },
	{ ":A:Tab",	":A:C-I" },
	{ ":A:Del",	":A:C-?" },
};

static const char *map_key(const char *key safe)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ctrl_map) ; i++) {
		if (strcmp(key, ctrl_map[i][0]) == 0)
			return ctrl_map[i][1];
	}
	return NULL;
}

DEF_CMD(keystroke)
{
	const char *key;
	struct input_mode *im = ci->home->data;
	struct pane *p;
	int ret;
	int num = im->num;
	int num2 = im->num2;
	const char *mode = im->mode;
	const char *alt;
	struct mark *m;

	if (!ci->str)
		return Enoarg;

	call("window:notify:Keystroke-notify", ci->home, 0, NULL, ci->str);
	log_add(im, "K", ci->str);

	im->mode = strdup("");
	im->num = NO_NUMERIC;
	im->num2 = 0;

	if (im->source != ci->focus) {
		im->source = ci->focus;
		im->focus = NULL;
		im->point = NULL;
	}

	if (!im->focus || im->focus->focus) {
		p = pane_leaf(ci->focus);
		im->focus = p;
		pane_add_notify(ci->home, p, "Notify:Close");
	}
	p = im->focus;

	if (!im->point)
		im->point = call_ret(mark, "doc:point", p);
	m = im->point;

	key = strconcat(ci->home, "K", mode, ci->str);
	time_starts();
	ret = call(key, p, num, m, NULL, num2);
	if (ret == 0 && (alt = map_key(ci->str)) != NULL) {
		key = strconcat(ci->home, "K", mode, alt);
		ret = call(key, p, num, m, NULL, num2);
	}
	time_ends();
	if (ret < 0)
		call("Message:default", ci->focus, 0, NULL,
		     "** Command Failed **");
	return Efallthrough;
}

static int tspec_diff_ms(struct timespec *a safe, struct timespec *b safe)
{
	return ((a->tv_sec - b->tv_sec) * 1000 +
		(a->tv_nsec - b->tv_nsec) / 1000000);
}

DEF_CMD(mouse_event)
{
	struct input_mode *im = ci->home->data;
	struct xy xy;
	int num, ex;
	struct pane *focus;
	char *key;
	struct timespec now;
	unsigned int b;
	int press;
	int r;
	const char *mode;
	const char *cmd;
	char *mod; /* :A:C:S modifiers - optional */
	struct mouse_state *ms = NULL;

	clock_gettime(CLOCK_MONOTONIC, &now);

	if (!ci->str)
		return Enoarg;

	call("window:notify:Mouse-event-notify", ci->home,
	     ci->num, NULL, ci->str,
	     ci->num2);
	log_add(im, "M", ci->str);

	if (ci->num2 == 1) {
		/* Press */
		press = 1;
		b = ci->num - 1;
	} else if (ci->num2 == 2) {
		/* Release */
		press = 0;
		b = ci->num - 1;
	} else {
		/* 3 is Motion */
		press = 0;
		b = 100;
	}
	if (b < 3) {
		bool repeat = False;

		ms = &im->buttons[b];

		/* FIXME the max movement for a double-click should be
		 * about a char width - maybe something based on scale
		 */
		if (tspec_diff_ms(&now, &ms->last_action) <= 500 &&
		    (abs(ci->x - ms->last_x) +
		     abs(ci->y - ms->last_y)) <= 2)
			/* FIXME should I let 'release' know that
			 * it was close to the 'press'??
			 */
			repeat = True;
		else
			/* Too much time or space has elapsed.
			 * This cannot be a click.
			 */
			ms->click_on_up = 0;

		if (press) {
			if (!repeat)
				ms->click_count = 1;
			else if (ms->click_count < 3)
				ms->click_count += 1;
		}
		ms->last_action = now;
		ms->last_x = ci->x; ms->last_y = ci->y;
	}

	focus = ci->focus;
	num = im->num;
	ex = im->num2;
	/* FIXME is there any point in this? */
	xy = pane_mapxy(ci->focus, focus, ci->x, ci->y, False);

	mode = strsave(ci->home, im->mode);
	free((void*)im->mode);
	im->mode = strdup("");
	im->num = NO_NUMERIC;
	im->num2 = 0;

	if (im->grab && !press) {
		/* Release and Motion should go to the same
		 * place as Press went.
		 */
		focus = im->grab;
		xy = pane_mapxy(focus, ci->focus, 0, 0, False);
		xy.x = ci->x - xy.x;
		xy.y = ci->y - xy.y;
	} else while (1) {
			struct pane *t, *chld = NULL;

			list_for_each_entry(t, &focus->children, siblings) {
				if (t->z < 0)
					continue;
				if (xy.x < t->x || xy.x >= t->x + t->w)
					continue;
				if (xy.y < t->y || xy.y >= t->y + t->h)
					continue;
				if (chld == NULL || t->z > chld->z)
					chld = t;
			}
			/* descend into chld */
			if (!chld)
				break;
			xy.x -= chld->x;
			xy.y -= chld->y;
			focus = chld;
		}
	if (im->grab != focus) {
		im->grab = focus;
		pane_add_notify(ci->home, focus, "Notify:Close");
	}

	if (!ms) {
		int ret;
		key = strconcat(ci->home, "M", mode, ci->str);
		time_starts();
		ret = call(key, focus, num, NULL, NULL, ex, NULL, NULL,
			    xy.x, xy.y);
		time_ends();
		return ret;
	}
	if (press) {
		/* identify and save modifiers */
		free(ms->mod);
		if (ci->str2)
			mod = strdup(ci->str2);
		else {
			char *c = strrchr(ci->str, ':');
			if (c)
				mod = strndup(ci->str, c - ci->str);
			else
				mod = strdup("");
		}
		ms->mod = mod;
	}

	if (press)
		cmd = "Press-";
	else if (ms->click_on_up)
		cmd = "Click-";
	else
		cmd = "Release-";

	/* Try :nPress :(n-1)Press ... (or :nRelease or :nClick)
	 * until something gets a result.
	 * 'n' is T (triple) or D(double) or ""(Single).
	 * If nothing got a result for a Press,, register for
	 * 'click' on release.
	 */
	ms->click_on_up = 1;
	for (r = ms->click_count; r >= 1 ; r--) {
		int ret;
		char *mult = "\0\0D\0T" + (r-1)*2;
		char n[2];

		n[0] = '1' + b;
		n[1] = 0;
		key = strconcat(ci->home, "M", mode, ms->mod, ":", mult,
				cmd, n);
		time_starts();
		ret = call(key, focus, num, NULL, NULL, ex,
			   NULL, NULL, xy.x, xy.y);
		time_ends();
		if (ret > 0) {
			/* If this is 'press', then don't want
			 * click_on_up.  If this is release, it
			 * don't matter what we set.
			 */
			ms->click_on_up = 0;
			return ret;
		}
	}
	return Efalse;
}

DEF_CMD(refocus)
{
	struct input_mode *im = ci->home->data;

	im->focus = NULL;
	im->point = NULL;
	im->source = NULL;
	return Efallthrough;
}

DEF_CMD(close_focus)
{
	struct input_mode *im = ci->home->data;

	if (im->focus == ci->focus) {
		im->focus = NULL;
		im->point = NULL;
		im->source = NULL;
	}

	if (im->grab == ci->focus)
		im->grab = NULL;
	return 1;
}

DEF_CMD(input_free)
{
	struct input_mode *im = ci->home->data;
	int i;

	for (i = 0; i < 3; i++)
		free(im->buttons[i].mod);
	free((void*)im->mode);
	free((void*)im->context);
	unalloc(im, pane);
	return 1;
}

static struct map *im_map;
static void register_map(void)
{
	if (im_map)
		return;
	im_map = key_alloc();
	key_add(im_map, "Keystroke", &keystroke);
	key_add(im_map, "Mouse-event", &mouse_event);
	key_add(im_map, "Mode:set-mode", &set_mode);
	key_add(im_map, "Mode:set-num", &set_num);
	key_add(im_map, "Mode:set-num2", &set_num2);
	key_add(im_map, "Mode:set-all", &set_all);
	key_add(im_map, "pane:refocus", &refocus);
	key_add(im_map, "Notify:Close", &close_focus);
	key_add(im_map, "input:log", &log_input);
	key_add(im_map, "Free", &input_free);
}

DEF_LOOKUP_CMD(input_handle, im_map);
DEF_CMD(input_attach)
{
	struct input_mode *im;
	struct pane *p;

	register_map();

	alloc(im, pane);
	im->mode = strdup("");
	im->context = strdup("");
	im->num = NO_NUMERIC;
	im->num2 = 0;

	p = pane_register(ci->focus, 0, &input_handle.c, im);
	if (!p)
		return Efail;

	return comm_call(ci->comm2, "callback:attach", p);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &input_attach, 0, NULL, "attach-input");
}
