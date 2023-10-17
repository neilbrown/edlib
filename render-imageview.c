/*
 * Copyright Neil Brown ©2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Display an image and allow it to be scaled and panned.
 */

#include <stdio.h>
#define PANE_DATA_TYPE struct imageview_data
#include "core.h"

struct imageview_data {
	char *image;
	int w,h;	/* size of image */
	int scale;	/* 1024 * displayed-size / actual size */
	int cx,cy;	/* image co-ordinates at center of pane.
			 * Kept stable during zoom
			 */
	short px,py;	/* number of pixels in each pane cell */
	bool integral;	/* Use integral scales */
};
#include "core-pane.h"

DEF_CMD_CLOSED(imageview_close)
{
	struct imageview_data *ivd = ci->home->data;

	free(ivd->image);
	ivd->image = NULL;
	return 1;
}

static int fix_scale(struct imageview_data *ivd safe, int scale)
{
	if (!ivd->integral)
		return scale;

	if (scale >= 1024)
		return scale & ~1023;
	if (scale > 0)
		return 1024 / (1024 / scale);
	return scale;
}

DEF_CMD(imageview_refresh)
{
	struct imageview_data *ivd = ci->home->data;
	char *img = ivd->image;
	int x,y;
	int pw = ci->home->w * ivd->px;
	int ph = ci->home->h * ivd->py;

	call("Draw:clear", ci->focus, 0, NULL, "bg:black");

	if (!img)
		img = pane_attr_get(ci->focus, "imageview:image-source");
	if (!img)
		img = "comm:doc:get-bytes";
	if (!ivd->image)
		ivd->image = strdup(img);

	if (ivd->w <= 0) {
		char *i;
		struct call_return cr = call_ret(all, "Draw:image-size",
						 ci->focus,
						 0, NULL, img);
		ivd->w = cr.x;
		ivd->h = cr.y;

		i = pane_attr_get(ci->focus, "imageview:integral");
		ivd->integral = (i && strcmp(i, "yes") == 0);
	}
	if (ivd->w <= 0 || ivd->h <= 0)
		return 1;

	if (ivd->scale <= 0) {
		int xs = pw * 1024 / ivd->w;
		int ys = ph * 1024 / ivd->h;
		ivd->scale = fix_scale(ivd, xs > ys ? ys : xs);
	}

	x = (ivd->cx * ivd->scale) - pw * 1024 / 2;
	y = (ivd->cy * ivd->scale) - ph * 1024 / 2;

	if (ivd->scale * ivd->w < pw * 1024)
		/* Doesn't use full width, so centre */
		x = -(pw * 1024 - ivd->scale * ivd->w) / 2;
	else {
		/* Does use full width, so avoid margins */
		if (x < 0)
			x = 0;
		if (x > ivd->w * ivd->scale - pw * 1024)
			x = ivd->w * ivd->scale - pw * 1024;
	}
	if (ivd->scale * ivd->h < ph * 1024)
		y = -(ph * 1024 - ivd->scale * ivd->h) / 2;
	else {
		if (y < 0)
			y = 0;
		if (y > ivd->h * ivd->scale - ph * 1024)
			y = ivd->h * ivd->scale - ph * 1024;
	}

	ivd->cx = (pw * 1024 / 2 + x) / ivd->scale;
	ivd->cy = (ph * 1024 / 2 + y) / ivd->scale;

	call("Draw:image", ci->focus, ivd->scale, NULL, img,
	     0, NULL, NULL, x / 1024, y / 1024);

	return 1;
}

DEF_CMD(imageview_refresh_size)
{
	struct imageview_data *ivd = ci->home->data;
	int pw = ci->home->w * ivd->px;
	int ph = ci->home->h * ivd->py;

	if (ivd->scale * ivd->w < pw * 1024 &&
	    ivd->scale * ivd->h < ph * 1024)
		/* Scale it too small to make use of space - reset */
		ivd->scale = 0;
	pane_damaged(ci->home, DAMAGED_REFRESH);

	return Efallthrough;
}

DEF_CMD(imageview_zoom)
{
	/* Keep the centre of the pane at the same pixel when
	 * zooming.
	 */
	struct imageview_data *ivd = ci->home->data;
	int scale = ivd->scale;

	if (strcmp(ci->key, "K-+") == 0) {
		/* zoom up */
		ivd->scale = fix_scale(ivd, scale + scale / 10);
		if (ivd->scale == scale)
			ivd->scale += 1024;
	} else {
		/* zoom down */
		ivd->scale = fix_scale(ivd, scale - scale / 11);
		if (ivd->scale == scale && scale > 1 && scale <= 1024)
			ivd->scale = 1024 / (1024 / scale + 1);
	}

	pane_damaged(ci->home, DAMAGED_REFRESH);
	return 1;
}

DEF_CMD(imageview_pan)
{
	struct imageview_data *ivd = ci->home->data;
	int pw = ci->home->w * ivd->px;
	int ph = ci->home->h * ivd->py;

	switch (ci->key[2]) {
	case 'L':
		ivd->cx -= pw * 1024 / ivd->scale / 10;
		break;
	case 'R':
		ivd->cx += pw * 1024 / ivd->scale / 10;
		break;
	case 'U':
		ivd->cy -= ph * 1024 / ivd->scale / 10;
		break;
	case 'D':
		ivd->cy += ph * 1024 / ivd->scale / 10;
		break;
	}
	pane_damaged(ci->home, DAMAGED_REFRESH);
	return 1;
}

DEF_CMD(imageview_reset)
{
	struct imageview_data *ivd = ci->home->data;

	ivd->scale = 0;

	pane_damaged(ci->home, DAMAGED_REFRESH);
	return 1;
}

DEF_CMD(imageview_quit)
{
	call("Tile:close", ci->focus);
	return 1;
}

static struct map *iv_map;
DEF_LOOKUP_CMD(iv_handle, iv_map);

DEF_CMD(imageview_attach)
{
	struct pane *p;
	struct imageview_data *ivd;
	char *pxl;

	p = pane_register(ci->focus, 0, &iv_handle.c);
	if (!p)
		return Efail;
	ivd = p->data;
	if (ci->str)
		ivd->image = strdup(ci->str);
	ivd->scale = 0;
	ivd->integral = False;
	pxl = pane_attr_get(p, "Display:pixels");
	if (sscanf(pxl ?: "1x1", "%hdx%hx", &ivd->px, &ivd->py) != 2)
		ivd->px = ivd->py = 1;

	pane_damaged(p, DAMAGED_REFRESH);

	return comm_call(ci->comm2, "cb", p);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &imageview_attach, 0, NULL,
		  "attach-render-imageview");
	iv_map = key_alloc();
	key_add(iv_map, "Close", &imageview_close);
	key_add(iv_map, "Refresh", &imageview_refresh);
	key_add(iv_map, "Refresh:size", &imageview_refresh_size);

	key_add(iv_map, "K-+", &imageview_zoom);
	key_add(iv_map, "K--", &imageview_zoom);

	key_add(iv_map, "K:Left", &imageview_pan);
	key_add(iv_map, "K:Right", &imageview_pan);
	key_add(iv_map, "K:Up", &imageview_pan);
	key_add(iv_map, "K:Down", &imageview_pan);
	key_add(iv_map, "K:Home", &imageview_reset);
	key_add(iv_map, "K-.", &imageview_reset);

	key_add(iv_map, "K:ESC", &imageview_quit);
	key_add(iv_map, "K-q", &imageview_quit);
}
