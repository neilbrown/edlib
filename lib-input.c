/*
 * Copyright Neil Brown ©2015-2019 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Core input translation.
 * This module transalates keystrokes and mouse events into commands.
 * This involves tracking the current 'mode' state.
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core.h"

struct input_mode {
	const char	*mode safe;
	int		num, num2;
	struct pane	*focus, *source;
	struct mark	*point;
	struct mouse_state {
		struct timespec	last_up;
		int		is_down;
		int		click_count;
		int		ignore_up;
	} buttons[3];
};

DEF_CMD(set_mode)
{
	struct input_mode *im = ci->home->data;

	if (!ci->str)
		return Enoarg;
	free((void*)im->mode);
	im->mode = strdup(ci->str);
	return 1;
}

DEF_CMD(set_num)
{
	struct input_mode *im = ci->home->data;

	im->num = ci->num;
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
	im->num = ci->num;
	im->num2 = ci->num;
	return 1;
}

DEF_CMD(keystroke)
{
	const char *key;
	char *vkey = NULL;
	struct input_mode *im = ci->home->data;
	struct pane *p;
	int l;
	int ret;
	int num = im->num;
	int num2 = im->num2;
	struct mark *m;
	int cnt = 1;
	const char *k = ci->str;
	char *end;

	if (!ci->str)
		return Enoarg;

	pane_notify("Keystroke-notify", ci->home, 0, NULL, ci->str);

	while ((end = strchr(k, '\037')) != NULL) {
		cnt += 1;
		k = end + 1;
		while (*k == '\037')
			k++;
	}
	l = (1 + strlen(im->mode)) * cnt + strlen(ci->str) + 1;

	vkey = malloc(l);
	memset(vkey, 0, l);
	k = ci->str;
	while ((end = strchr(k, '\037')) != NULL) {
		end += 1;
		strcat(vkey, "K");
		strcat(vkey, im->mode);
		strncat(vkey, k, end-k);
		k = end;
		while (*k == '\037')
			k++;
	}
	strcat(vkey, "K");
	strcat(vkey, im->mode);
	strcat(vkey, k);
	key = vkey;

	free((void*)im->mode);
	im->mode = strdup("");
	im->num = NO_NUMERIC;
	im->num2 = 0;

	if (im->source != ci->focus) {
		im->source = ci->focus;
		im->focus = NULL;
		im->point = NULL;
	}

	if (!im->focus || im->focus->focus) {
		p = ci->focus;
		while (p->focus)
			p = p->focus;
		im->focus = p;
		pane_add_notify(ci->home, p, "Notify:Close");
	}
	p = im->focus;

	if (!im->point)
		im->point = call_ret(mark, "doc:point", p);
	m = im->point;

	ret = call(key, p, num, m, NULL, num2);
	free(vkey);
	if (ret < 0)
		call("Message:default", ci->focus, 0, NULL,
		     "** Command Failed **");
	return 0;
}

static int tspec_diff_ms(struct timespec *a safe, struct timespec *b safe)
{
	return ((a->tv_sec - b->tv_sec) * 1000 +
		(a->tv_nsec - b->tv_nsec) / 1000000);
}

DEF_CMD(mouse_event)
{
	struct input_mode *im = ci->home->data;
	short x,y;
	int num, ex;
	struct pane *focus;
	char *key;
	struct timespec now;
	unsigned int b;
	int press;
	const char *mode;
	const char *mod = ci->str2; /* :M:C:S modifiers - optional */
	struct mouse_state *ms = NULL;

	clock_gettime(CLOCK_MONOTONIC, &now);

	if (!ci->str)
		return Enoarg;

	pane_notify("Mouse-event-notify", ci->home, ci->num, NULL, ci->str,
		    ci->num2);

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
		press = 1;
		b = 100;
	}
	if (b < 3) {
		ms = &im->buttons[b];
		if (press == ms->is_down) {
			/* No change */
			if (!press)
				ms->last_up = now;
			return 1;
		}
		ms->is_down = press;
		if (press) {
			if (tspec_diff_ms(&now, &ms->last_up) > 500)
				ms->click_count = 1;
			else if (ms->click_count < 3)
				ms->click_count += 1;
		} else {
			ms->last_up = now;
			if (ms->ignore_up) {
				ms->ignore_up = 0;
				return 1;
			}
		}
	}

	focus = ci->focus;
	num = im->num;
	ex = im->num2;
	x = ci->x; y = ci->y;
	/* FIXME is there any point in this? */
	pane_map_xy(ci->focus, focus, &x, &y);

	mode = strsave(ci->home, im->mode);
	free((void*)im->mode);
	im->mode = strdup("");
	im->num = NO_NUMERIC;
	im->num2 = 0;

	while (1) {
		struct pane *t, *chld = NULL;

		list_for_each_entry(t, &focus->children, siblings) {
			if (t->z < 0)
				continue;
			if (x < t->x || x >= t->x + t->w)
				continue;
			if (y < t->y || y >= t->y + t->h)
				continue;
			if (chld == NULL || t->z > chld->z)
				chld = t;
		}
		/* descend into chld */
		if (!chld)
			break;
		x -= chld->x;
		y -= chld->y;
		focus = chld;
	}

	if (!ms) {
		key = strconcat(ci->home, "M", mode, ci->str);
		return call(key, focus, num, NULL, NULL, ex, NULL, NULL, x, y);
	}
	if (press) {
		/* Try nPress :nClick :(n-1)Press :(n-1)Click until something
		 * gets a result. 'n' is T (triple) or D(double) or ""(Single).
		 * If a Click got a result, suppress subsequent release
		 */
		int r;

		if (!mod) {
			char *c = strrchr(ci->str, ':');
			if (c)
				mod = strnsave(ci->home, ci->str, c - ci->str);
			else
				mod = "";
		}

		ms->ignore_up = 1;
		for (r = ms->click_count; r >= 1 ; r--) {
			int ret;
			char *mult = "\0\0D\0T" + (r-1)*2;
			char n[2];
			n[0] = '1' + b;
			n[1] = 0;
			key = strconcat(ci->home, "M", mode, mod, ":", mult,
					"Press-", n);
			ret = call(key, focus, num, NULL, NULL, ex,
				   NULL, NULL, x, y);

			if (ret) {
				/* Only get a Release if you respond to a
				 * Press
				 */
				ms->ignore_up = 0;
				return ret;
			}

			key = strconcat(ci->home, "M", mode, mod, mult,
					":Click-", n);
			ret = call(key, focus, num, NULL, NULL, ex,
				   NULL, NULL, x, y);

			if (ret)
				return ret;
		}
	} else {
		/* Try nRelease (n-1)Release etc */
		int r;
		for (r = ms->click_count; r >= 1 ; r--) {
			int ret;
			char *mult = "\0\0D\0T" + (r-1)*2;
			char n[2];
			n[0] = '1' + b;
			n[1] = 0;

			key = strconcat(ci->home, "M", mode, ":", mult,
					"Release-", n);
			ret = call(key, focus, num, NULL, NULL, ex,
				   NULL, NULL, x, y);

			if (ret)
				return ret;
		}
	}
	return 0;
}

DEF_CMD(request_notify)
{
	pane_add_notify(ci->focus, ci->home, ksuffix(ci, "window:request:"));
	return 1;
}

DEF_CMD(send_notify)
{
	/* window:notify:... */
	return home_pane_notify(ci->home, ksuffix(ci, "window:notify:"),
				ci->focus,
				ci->num, ci->mark, ci->str,
				ci->num2, ci->mark2, ci->str2, ci->comm2);
}

DEF_CMD(refocus)
{
	struct input_mode *im = ci->home->data;

	im->focus = NULL;
	im->point = NULL;
	im->source = NULL;
	return 0;
}

DEF_CMD(close_focus)
{
	struct input_mode *im = ci->home->data;

	if (im->focus == ci->focus) {
		im->focus = NULL;
		im->point = NULL;
		im->source = NULL;
	}
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
	key_add_prefix(im_map, "window:request:", &request_notify);
	key_add_prefix(im_map, "window:notify:", &send_notify);
	key_add(im_map, "Free", &edlib_do_free);
}

DEF_LOOKUP_CMD(input_handle, im_map);
DEF_CMD(input_attach)
{
	struct input_mode *im;
	struct pane *p;

	register_map();

	alloc(im, pane);
	im->mode = strdup("");
	im->num = NO_NUMERIC;
	im->num2 = 0;

	p = pane_register(ci->focus, 0, &input_handle.c, im);
	if (p)
		return comm_call(ci->comm2, "callback:attach", p);
	return Efail;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &input_attach, 0, NULL, "attach-input");
}
