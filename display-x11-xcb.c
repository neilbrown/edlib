/*
 * Copyright Neil Brown ©2021-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * X11 display driver for edlib, using xcb, cairopango, libxkbcommon etc.
 *
 * A different connection to the server will be created for each
 * display.  Maybe that can be optimised one day.
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <string.h>
#include <xcb/xcb.h>
#include <stdarg.h>
#include <sys/wait.h>
#ifndef __CHECKER__
#include <xcb/xkb.h>
#else
/* xkb.h has a 'long' in an enum :-( */
enum {
	XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY,
	XCB_XKB_EVENT_TYPE_MAP_NOTIFY,
	XCB_XKB_NEW_KEYBOARD_NOTIFY,
	XCB_XKB_EVENT_TYPE_STATE_NOTIFY,
	XCB_XKB_MAP_PART_MODIFIER_MAP,
	XCB_XKB_STATE_PART_MODIFIER_LOCK,
	XCB_XKB_MAP_PART_EXPLICIT_COMPONENTS,
	XCB_XKB_STATE_PART_GROUP_BASE,
	XCB_XKB_MAP_PART_KEY_ACTIONS,
	XCB_XKB_STATE_PART_GROUP_LATCH,
	XCB_XKB_MAP_PART_VIRTUAL_MODS,
	XCB_XKB_STATE_PART_GROUP_LOCK,
	XCB_XKB_MAP_PART_VIRTUAL_MOD_MAP,
	XCB_XKB_NKN_DETAIL_KEYCODES,
	XCB_XKB_MAP_PART_KEY_TYPES,
	XCB_XKB_MAP_PART_KEY_SYMS,
	XCB_XKB_STATE_PART_MODIFIER_BASE,
	XCB_XKB_STATE_PART_MODIFIER_LATCH,
	XCB_XKB_MAP_NOTIFY,
	XCB_XKB_STATE_NOTIFY,
};
typedef uint16_t xcb_xkb_device_spec_t;
typedef struct xcb_xkb_select_events_details_t {
	uint16_t affectNewKeyboard;
	uint16_t newKeyboardDetails;
	uint16_t affectState;
	uint16_t stateDetails;
	/* and other fields */
} xcb_xkb_select_events_details_t;
typedef struct xcb_xkb_new_keyboard_notify_event_t {
	uint8_t		deviceID;
	uint16_t	changed;
	/* and other fields */
} xcb_xkb_new_keyboard_notify_event_t;
typedef struct xcb_xkb_state_notify_event_t {
	uint8_t		deviceID;
	uint8_t		baseMods;
	uint8_t		latchedMods;
	uint8_t		lockedMods;
	int16_t		baseGroup;
	int16_t		latchedGroup;
	uint8_t		lockedGroup;
	/* and other fields */
} xcb_xkb_state_notify_event_t;
typedef struct xcb_xkb_map_notify_event_t {
	uint8_t		deviceID;
} xcb_xkb_map_notify_event_t;
xcb_void_cookie_t
xcb_xkb_select_events_aux_checked(xcb_connection_t		*c,
				  xcb_xkb_device_spec_t		deviceSpec,
				  uint16_t			affectWhich,
				  uint16_t			clear,
				  uint16_t			selectAll,
				  uint16_t			affectMap,
				  uint16_t			map,
				  const xcb_xkb_select_events_details_t *details);

#endif
#include <xcb/xcbext.h>
#include <ctype.h>
#include <math.h>
#include <locale.h>

#include <cairo.h>
#include <cairo-xcb.h>

#include <wand/MagickWand.h>
#ifdef __CHECKER__
// enums confuse sparse...
#define MagickBooleanType int
#endif

#ifndef __CHECKER__
#include <pango/pango.h>
#include <pango/pangocairo.h>
#else
typedef struct PangoFontDescription {} PangoFontDescription;
typedef struct PangoLayout {} PangoLayout;
typedef struct PangoContext {} PangoContext;
typedef struct PangoFontMetrics {} PangoFontMetrics;
typedef struct PangoRectangle { int x,y,width,height;} PangoRectangle;
typedef enum { PANGO_STYLE_NORMAL, PANGO_STYLE_OBLIQUE, PANGO_STYLE_ITALIC
} PangoStyle;
typedef enum { PANGO_VARIANT_NORMAL, PANGO_VARIANT_SMALL_CAPS } PangoVariant;
typedef enum { PANGO_WEIGHT_NORMAL, PANGO_WEIGHT_BOLD } PangoWeight;
PangoFontDescription *pango_font_description_new(void);
void pango_font_description_set_family_static(PangoFontDescription*, char*);
void pango_font_description_set_family(PangoFontDescription*, char*);
void pango_font_description_set_size(PangoFontDescription*, int);
void pango_font_description_set_style(PangoFontDescription*, PangoStyle);
void pango_font_description_set_variant(PangoFontDescription*, PangoVariant);
void pango_font_description_set_weight(PangoFontDescription*, PangoWeight);
#define PANGO_SCALE (1024)

PangoLayout *pango_cairo_create_layout(cairo_t*);
void g_object_unref(PangoLayout*);
PangoContext *pango_cairo_create_context(cairo_t *);
void pango_cairo_show_layout(cairo_t *, PangoLayout *);
PangoFontMetrics *pango_context_get_metrics(PangoContext*, PangoFontDescription*, void*);
void pango_font_description_free(PangoFontDescription*);
int pango_font_metrics_get_approximate_char_width(PangoFontMetrics *);
int pango_font_metrics_get_ascent(PangoFontMetrics *);
int pango_font_metrics_get_descent(PangoFontMetrics *);
void pango_font_metrics_unref(PangoFontMetrics *);
PangoContext* pango_layout_get_context(PangoLayout *);
int pango_layout_get_baseline(PangoLayout *);
void pango_layout_get_extents(PangoLayout *, PangoRectangle *, PangoRectangle *);
void pango_layout_get_pixel_extents(PangoLayout *, PangoRectangle *, PangoRectangle *);
void pango_layout_set_font_description(PangoLayout *, PangoFontDescription *);
void pango_layout_set_text(PangoLayout*, const char *, int);
void pango_layout_xy_to_index(PangoLayout*, int, int, int*, int*);
void pango_layout_index_to_pos(PangoLayout*, int, PangoRectangle*);
#endif

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon-x11.h>

#include "xcb.h"

#undef True
#undef False

#define PANE_DATA_TYPE struct xcb_data
#include "core.h"

enum my_atoms {
	a_NONE = 0,
	a_WM_STATE, a_STATE_FULLSCREEN,
	a_WM_NAME, a_NET_WM_NAME,
	a_WM_ICON_NAME, a_NET_WM_ICON_NAME,
	a_WM_PROTOCOLS, a_WM_DELETE_WINDOW,
	a_NET_WM_PING,
	a_NET_WM_ICON,
	a_WM_CLIENT_MACHINE,
	a_UTF8_STRING,
	NR_ATOMS
};
static const char *atom_names[NR_ATOMS] = {
	[a_NONE]		= "NONE",
	[a_WM_STATE]		= "_NET_WM_STATE",
	[a_STATE_FULLSCREEN]	= "_NET_WM_STATE_FULLSCREEN",
	[a_WM_NAME]		= "WM_NAME",
	[a_NET_WM_NAME]		= "_NET_WM_NAME",
	[a_WM_ICON_NAME]	= "WM_ICON_NAME",
	[a_NET_WM_ICON_NAME]	= "_NET_WM_ICON_NAME",
	[a_WM_PROTOCOLS]	= "WM_PROTOCOLS",
	[a_WM_DELETE_WINDOW]	= "WM_DELETE_WINDOW",
	[a_NET_WM_PING]		= "_NET_WM_PING",
	[a_NET_WM_ICON]		= "_NET_WM_ICON",
	[a_WM_CLIENT_MACHINE]	= "WM_CLIENT_MACHINE",
	[a_UTF8_STRING]		= "UTF8_STRING",
};

struct rgb {
	double r,g,b;
};

struct xcb_data {
	xcb_connection_t	*conn safe;
	char			*display safe;
	char			*disp_auth;

	const xcb_setup_t	*setup safe;
	const xcb_screen_t	*screen safe;
	xcb_atom_t		atoms[NR_ATOMS];

	long			last_event;
	xcb_window_t		win;
	xcb_visualtype_t	*visual;
	cairo_t			*cairo safe;
	cairo_surface_t		*surface safe;
	PangoFontDescription	*fd safe;
	int			charwidth, lineheight;
	cairo_region_t		*need_update;

	bool			motion_blocked;
	bool			in_focus;

	struct xkb_context	*xkb;
	uint8_t			first_xkb_event;
	int32_t			xkb_device_id;
	struct xkb_state	*xkb_state;
	struct xkb_compose_state *compose_state;
	struct xkb_compose_table *compose_table;
	struct xkb_keymap	*xkb_keymap;

	struct pids {
		pid_t		pid;
		struct pids	*next;
	}			*pids;

	/* FIXME use hash?? */
	struct panes {
		struct panes	*next;
		struct pane	*p safe;
		cairo_rectangle_int_t r;
		cairo_t		*ctx;
		struct rgb	bg;
		xcb_pixmap_t	draw;
		cairo_surface_t	*surface;
		cairo_region_t	*need_update;
	} *panes;
};
#include "core-pane.h"

/* panes->r.x is NEVER_DRAWN if the pane has not been drawn */
#define NEVER_DRAWN (-60000)

static struct map *xcb_map;
DEF_LOOKUP_CMD(xcb_handle, xcb_map);

static struct panes *get_pixmap(struct pane *home safe,
				struct pane *p safe)
{
	struct xcb_data *xd = home->data;
	struct panes **pp, *ps;

	for (pp = &xd->panes; (ps = *pp) != NULL; pp = &(*pp)->next) {
		if (ps->p != p)
			continue;
		if (ps->r.width == p->w && ps->r.height == p->h)
			return ps;
		*pp = ps->next;
		if (ps->r.x != NEVER_DRAWN) {
			if (!xd->need_update)
				xd->need_update = cairo_region_create();
			cairo_region_union_rectangle(xd->need_update, &ps->r);
		}
		if (ps->ctx)
			cairo_destroy(ps->ctx);
		if (ps->surface)
			cairo_surface_destroy(ps->surface);
		if (ps->draw)
			xcb_free_pixmap(xd->conn, ps->draw);
		free(ps);
		break;
	}
	alloc(ps, pane);
	ps->p = p;
	ps->r.x = ps->r.y = NEVER_DRAWN;
	ps->r.width = p->w;
	ps->r.height = p->h;
	ps->bg.r = ps->bg.g = ps->bg.b = 0;

	pane_add_notify(home, p, "Notify:Close");
	ps->next = *pp;
	*pp = ps;
	return ps;
}

static void instantiate_pixmap(struct xcb_data *xd safe,
			  struct panes *ps safe)
{
	ps->draw = xcb_generate_id(xd->conn);
	xcb_create_pixmap(xd->conn, xd->screen->root_depth, ps->draw,
			  xd->win, ps->r.width, ps->r.height);
	ps->surface = cairo_xcb_surface_create(
		xd->conn, ps->draw, xd->visual, ps->r.width, ps->r.height);
	if (!ps->surface)
		goto free_ps;
	ps->ctx = cairo_create(ps->surface);
	if (!ps->ctx)
		goto free_surface;
	cairo_set_source_rgb(ps->ctx, ps->bg.r, ps->bg.g, ps->bg.b);
	cairo_paint(ps->ctx);
	return;

free_surface:
	cairo_surface_destroy(ps->surface);
	ps->surface = NULL;
free_ps:
	xcb_free_pixmap(xd->conn, ps->draw);
	ps->draw = 0;
}

static struct panes *find_pixmap(struct xcb_data *xd safe, struct pane *p safe,
				 int *xp safe, int *yp safe)
{
	int x = 0, y = 0;
	struct panes *ret = NULL;

	while (!ret && p->parent != p) {
		struct panes *ps;
		for (ps = xd->panes; ps ; ps = ps->next)
			if (ps->p == p) {
				ret = ps;
				break;
			}
		if (!ret) {
			x += p->x;
			y += p->y;
			p = p->parent;
		}
	}
	*xp = x;
	*yp = y;
	return ret;
}

static inline double cvt(int i)
{
	return (float)i / 1000.0;
}

static void parse_attrs(
	struct pane *home safe, const char *cattrs, int scale,
	struct rgb *fgp, struct rgb *bgp, bool *underline,
	PangoFontDescription **fdp)
{
	char *attrs = strdup(cattrs ?: "");
	char *ap = attrs;
	char *word;
	char *fg = NULL, *bg = NULL;
	bool ul = False;
	bool inv = False;
	int size = 12*1000;
	PangoFontDescription *fd = NULL;
	PangoStyle style = PANGO_STYLE_NORMAL;
	PangoVariant variant = PANGO_VARIANT_NORMAL;
	PangoWeight weight = PANGO_WEIGHT_NORMAL;

	if (fdp) {
		fd = pango_font_description_new();
		*fdp = fd;
		pango_font_description_set_family_static(fd, "monospace");
	}

	while ((word = strsep(&ap, ",")) != NULL) {
		if (fd && strstarts(word, "family:"))
			pango_font_description_set_family(fd, word+7);
		if (strcmp(word, "large") == 0)
			size = 14 * 1000;
		if (strcmp(word, "small") == 0)
			size = 9 * 1000;
		if (isdigit(word[0])) {
			char *end = NULL;
			double s = strtod(word, &end);
			if (end && end != word && !*end)
				size = trunc(s * 1000.0);
			else
				size = 10*1000;
		}
		if (strcmp(word, "oblique") == 0)
			style = PANGO_STYLE_OBLIQUE;
		if (strcmp(word, "italic") == 0)
			style = PANGO_STYLE_ITALIC;
		if (strcmp(word, "normal") == 0)
			style = PANGO_STYLE_NORMAL;
		if (strcmp(word, "small-caps") == 0)
			variant = PANGO_VARIANT_SMALL_CAPS;

		if (strcmp(word, "bold") == 0)
			weight = PANGO_WEIGHT_BOLD;
		if (strcmp(word, "nobold") == 0)
			weight = PANGO_WEIGHT_NORMAL;

		if (strstarts(word, "fg:"))
			fg = word + 3;
		if (strstarts(word, "bg:"))
			bg = word + 3;
		if (strcmp(word, "inverse") == 0)
			inv = True;
		if (strcmp(word, "noinverse") == 0)
			inv = False;
		if (strcmp(word, "underline") == 0)
			ul = True;
		if (strcmp(word, "nounderline") == 0)
			ul = False;
	}

	if (inv) {
		char *t = bg;
		bg = fg;
		fg = t;
		if (!fg)
			fg = "white";
		if (!bg)
			bg = "black";
	} else if (!fg)
		fg = "black";

	if (fg && fgp) {
		struct call_return ret = call_ret(all, "colour:map", home,
						  0, NULL, fg);
		fgp->r = cvt(ret.i);
		fgp->g = cvt(ret.i2);
		fgp->b = cvt(ret.x);
	} else if (fgp)
		fgp->g = -1;
	if (bg && bgp) {
		struct call_return ret = call_ret(all, "colour:map", home,
						  0, NULL, bg);
		bgp->r = cvt(ret.i);
		bgp->g = cvt(ret.i2);
		bgp->b = cvt(ret.x);
	} else if (bgp)
		bgp->g = -1;
	if (fd) {
		pango_font_description_set_size(fd, PANGO_SCALE * size /1000 * scale / 1000);
		if (style != PANGO_STYLE_NORMAL)
			pango_font_description_set_style(fd, style);
		if (variant != PANGO_VARIANT_NORMAL)
			pango_font_description_set_variant(fd, variant);
		if (weight != PANGO_WEIGHT_NORMAL)
			pango_font_description_set_weight(fd, weight);
	}
	if (underline)
		*underline = ul;
	free(attrs);
}

DEF_CB(cnt_disp)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);

	cr->i += 1;
	return 1;
}

DEF_CMD_CLOSED(xcb_close_display)
{
	/* If this is only display, then refuse to close this one */
	struct call_return cr;
	char *nc = pane_attr_get(ci->home, "no-close");

	if (nc) {
		call("Message", ci->focus, 0, NULL, nc);
		return 1;
	}
	cr.c = cnt_disp;
	cr.i = 0;
	call_comm("editor:notify:all-displays", ci->focus, &cr.c);
	if (cr.i > 1)
		return Efallthrough;
	else
		call("Message", ci->focus, 0, NULL,
		     "Cannot close only window.");
	return 1;
}

static void wait_for(struct xcb_data *xd safe)
{
	struct pids **pp = &xd->pids;

	while (*pp) {
		struct pids *p = *pp;
		if (waitpid(p->pid, NULL, WNOHANG) > 0) {
			*pp = p->next;
			free(p);
		} else
			pp = &p->next;
	}
}

DEF_CMD(xcb_external_viewer)
{
	struct xcb_data *xd = ci->home->data;
	const char *path = ci->str;
	struct pids *p;
	int pid;
	int fd;

	if (!path)
		return Enoarg;
	switch (pid = fork()) {
	case -1:
		return Efail;
	case 0: /* Child */
		setenv("DISPLAY", xd->display, 1);
		if (xd->disp_auth)
			setenv("XAUTHORITY", xd->disp_auth, 1);
		fd = open("/dev/null", O_RDWR);
		if (fd) {
			dup2(fd, 0);
			dup2(fd, 1);
			dup2(fd, 2);
			if (fd > 2)
				close(fd);
		}
		execlp("xdg-open", "xdg-open", path, NULL);
		exit(1);
	default: /* parent */
		p = malloc(sizeof(*p));
		p->pid = pid;
		p->next = xd->pids;
		xd->pids = p;
		break;
	}
	wait_for(xd);
	return 1;
}

DEF_CMD(xcb_fullscreen)
{
	struct xcb_data *xd = ci->home->data;
	xcb_client_message_event_t msg = {};

	msg.response_type = XCB_CLIENT_MESSAGE;
	msg.format = 32;
	msg.window = xd->win;
	msg.type = xd->atoms[a_WM_STATE];
	if (ci->num > 0)
		msg.data.data32[0] = 1; /* ADD */
	else
		msg.data.data32[0] = 0; /* REMOVE */
	msg.data.data32[1] = xd->atoms[a_STATE_FULLSCREEN];
	msg.data.data32[2] = 0;
	msg.data.data32[3] = 1; /* source indicator */

	xcb_send_event(xd->conn, 0, xd->screen->root,
		       XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
		       XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
		       (void*)&msg);
	xcb_flush(xd->conn);
	return 1;
}

static void panes_free(struct xcb_data *xd safe)
{
	while (xd->panes) {
		struct panes *ps = xd->panes;
		xd->panes = ps->next;
		if (ps->ctx)
			cairo_destroy(ps->ctx);
		if (ps->surface)
			cairo_surface_destroy(ps->surface);
		if (ps->draw)
			xcb_free_pixmap(xd->conn, ps->draw);
		free(ps);
	}
}

static void kbd_free(struct xcb_data *xd safe);

DEF_CMD_CLOSED(xcb_close)
{
	struct xcb_data *xd = ci->home->data;

	xcb_destroy_window(xd->conn, xd->win);
	kbd_free(xd);
	panes_free(xd);

	pango_font_description_free(xd->fd);
	cairo_destroy(xd->cairo);
	cairo_device_finish(cairo_surface_get_device(xd->surface));
	cairo_surface_destroy(xd->surface);
	free(xd->display);
	free(xd->disp_auth);
	xcb_disconnect(xd->conn);
	if (xd->need_update)
		cairo_region_destroy(xd->need_update);
	return 1;
}

DEF_CMD(xcb_clear)
{
	struct xcb_data *xd = ci->home->data;
	const char *attr = ci->str;
	struct panes *src = NULL, *dest;
	struct rgb bg;
	int x=0, y=0;
	cairo_rectangle_int_t r;

	if (attr) {
		parse_attrs(ci->home, attr, PANGO_SCALE, NULL, &bg, NULL, NULL);
		if (bg.g < 0)
			bg.r = bg.g = bg.b = 1.0;
	} else {
		src = find_pixmap(xd, ci->focus->parent, &x, &y);
		x += ci->focus->x;
		y += ci->focus->y;
		if (!src)
			bg.r = bg.g = bg.b = 1.0;
		else if (src->bg.g >= 0)
			bg = src->bg;
		else if (src->surface == NULL)
			bg.r = bg.g = bg.b = 1.0;
		else
			bg.g = -1;
	}

	dest = get_pixmap(ci->home, ci->focus);
	if (!dest)
		return 1;
	if (bg.g >= 0) {
		if (dest->ctx) {
			cairo_set_source_rgb(dest->ctx, bg.r, bg.g, bg.b);
			cairo_paint(dest->ctx);
		}
		dest->bg = bg;
	} else if (src) {
		if (!dest->ctx)
			instantiate_pixmap(xd, dest);
		if (dest->ctx) {
			cairo_set_source_surface(dest->ctx, src->surface, -x, -y);
			cairo_paint(dest->ctx);
			dest->bg.g = -1;
		}
	}
	pane_damaged(ci->home, DAMAGED_POSTORDER);

	if (!dest->need_update)
		dest->need_update = cairo_region_create();
	r.x = 0;
	r.y = 0;
	r.width = ci->focus->w;
	r.height = ci->focus->h;
	cairo_region_union_rectangle(dest->need_update, &r);
	return 1;
}

DEF_CMD(xcb_text_size)
{
	struct xcb_data *xd = ci->home->data;
	const char *attr = ci->str2 ?: "";
	const char *str = ci->str ?: "";
	int scale = ci->num2;
	PangoLayout *layout;
	PangoFontDescription *fd;
	PangoRectangle log;
	int baseline;
	int max_bytes;

	if (scale <= 0)
		scale = 1000;
	if (!utf8_valid(str))
		str = "*INV*";
	parse_attrs(ci->home, attr, scale, NULL, NULL, NULL, &fd);
	/* If we use an empty string, line-height is wrong */
	layout = pango_cairo_create_layout(xd->cairo);
	pango_layout_set_text(layout, *str ? str : "M", -1);
	pango_layout_set_font_description(layout, fd);
	pango_layout_get_pixel_extents(layout, NULL, &log);
	baseline = pango_layout_get_baseline(layout) / PANGO_SCALE;

	if (ci->num < 0)
		max_bytes = 0;
	else if (log.width <= ci->num)
		max_bytes = strlen(str);
	else
		pango_layout_xy_to_index(layout, PANGO_SCALE*ci->num,
					 baseline, &max_bytes, NULL);

	comm_call(ci->comm2, "cb", ci->focus, max_bytes, NULL, NULL,
		  baseline, NULL, NULL,
		  str && *str ? log.width : 0,
		  log.height);

	pango_font_description_free(fd);
	g_object_unref(layout);
	return 1;
}

DEF_CMD(xcb_draw_text)
{
	struct xcb_data *xd = ci->home->data;
	const char *str = ci->str;
	const char *attr = ci->str2;
	int scale = 1000;
	struct panes *ps;
	cairo_t *ctx;
	PangoLayout *layout;
	PangoFontDescription *fd;
	PangoRectangle log;
	struct rgb fg, bg;
	bool ul;
	int baseline;
	int xo = 0, yo = 0;
	int x,y;

	if (!str)
		return Enoarg;
	ps = find_pixmap(xd, ci->focus, &xo, &yo);
	if (!ps)
		return Einval;
	if (!ps->ctx)
		instantiate_pixmap(xd, ps);
	ps->bg.g = -1;
	ctx = ps->ctx;
	if (!ctx)
		return Efail;

	if (!utf8_valid(str))
		str = "*INV*";

	pane_damaged(ci->home, DAMAGED_POSTORDER);

	if (ci->num2 > 0)
		scale = ci->num2 * 10 / xd->charwidth;

	parse_attrs(ci->home, attr, scale, &fg, &bg, &ul, &fd);

	x = ci->x + xo;
	y = ci->y + yo;
	layout = pango_cairo_create_layout(ctx);
	pango_layout_set_text(layout, str, -1);
	pango_layout_set_font_description(layout, fd);
	pango_layout_get_pixel_extents(layout, NULL, &log);
	baseline = pango_layout_get_baseline(layout) / PANGO_SCALE;
	cairo_save(ctx);
	if (bg.g >= 0) {
		cairo_set_source_rgb(ctx, bg.r, bg.g, bg.b);
		cairo_rectangle(ctx, x+log.x, y - baseline + log.y,
				log.width, log.height);
		cairo_fill(ctx);
	}
	cairo_set_source_rgb(ctx, fg.r, fg.g, fg.b);
	if (ul) {
		/* Draw an underline */
		cairo_rectangle(ctx, x+log.x, y+2+log.y,
				log.width, 1);
		cairo_fill(ctx);
	}

	cairo_move_to(ctx, x, y - baseline);
	pango_cairo_show_layout(ctx, layout);
	cairo_stroke(ctx);

	if (ci->num >= 0) {
		/* draw a cursor - outline box if not in-focus,
		 * inverse-video if it is.
		 */
		PangoRectangle curs;
		bool in_focus = xd->in_focus;
		struct pane *f = ci->focus;
		double cx, cy, cw, ch;

		pango_layout_index_to_pos(layout, ci->num, &curs);
		if (curs.width <= 0) {
			/* EOL?*/
			pango_layout_set_text(layout, "M", 1);
			pango_layout_get_extents(layout, NULL, &log);
			curs.width = log.width;
		}

		while (in_focus && f->parent->parent != f &&
		       f->parent != ci->home) {
			if (f->parent->focus != f && f->z >= 0)
				in_focus = False;
			f = f->parent;
		}
		if (!in_focus) {
			/* Just an fg:rectangle around the fg:text */
			/* Add half to x,y as stroke is either side of the line */
			cx = x * PANGO_SCALE + curs.x + PANGO_SCALE/2;
			cy = (y - baseline) * PANGO_SCALE + curs.y + PANGO_SCALE/2;
			ch = curs.height - PANGO_SCALE;
			cw = curs.width - PANGO_SCALE;
			cairo_rectangle(ctx, cx/PANGO_SCALE, cy/PANGO_SCALE,
					cw/PANGO_SCALE, ch/PANGO_SCALE);
			cairo_set_line_width(ctx, 1.0);
			cairo_stroke(ctx);
		} else {
			/* solid fd:block with txt in bg color */
			cairo_rectangle(ctx,
					x+curs.x/PANGO_SCALE,
					y-baseline+curs.y/PANGO_SCALE,
					curs.width / PANGO_SCALE,
					curs.height / PANGO_SCALE);
			cairo_fill(ctx);
			if (ci->num < (int)strlen(str)) {
				const char *cp = str + ci->num;
				get_utf8(&cp, NULL);
				pango_layout_set_text(layout, str + ci->num,
						      cp - (str + ci->num));
				if (bg.g >= 0)
					cairo_set_source_rgb(ctx, bg.r, bg.g, bg.b);
				else
					cairo_set_source_rgb(ctx, 1.0, 1.0, 1.0);
				cairo_move_to(ctx,
					      x + curs.x / PANGO_SCALE,
					      y - baseline + curs.y / PANGO_SCALE);
				pango_cairo_show_layout(ctx, layout);
			}
		}
	}
	cairo_restore(ctx);
	pango_font_description_free(fd);
	g_object_unref(layout);
	return 1;
}

DEF_CMD(xcb_draw_image)
{
	/* 'str' identifies the image. Options are:
	 *     file:filename  - load file from fs
	 *     comm:command   - run command collecting bytes
	 * 'str2' container 'mode' information.
	 *     By default the image is placed centrally in the pane
	 *     and scaled to use either fully height or fully width.
	 *     Various letters modify this:
	 *     'S' - stretch to use full height *and* full width
	 *     'L' - place on left if full width isn't used
	 *     'R' - place on right if full width isn't used
	 *     'T' - place at top if full height isn't used
	 *     'B' - place at bottom if full height isn't used.
	 *
	 *    Also a suffix ":NNxNN" will be parse and the two numbers used
	 *    to give number of rows and cols to overlay on the image for
	 *    the purpose of cursor positioning.  If these are present and
	 *    p->cx,cy are not negative, draw a cursor at p->cx,cy highlighting
	 *    the relevant cell.
	 *
	 * num,num2, if both positive, override the automatic scaling.
	 *    The image is scaled to this many pixels.
	 * x,y is top-left pixel in the scaled image to start display at.
	 *    Negative values allow a margin between pane edge and this image.
	 */
	struct xcb_data *xd = ci->home->data;
	const char *mode = ci->str2 ?: "";
	bool stretch = strchr(mode, 'S');
	int w, h;
	int x = 0, y = 0;
	int pw, ph;
	int xo, yo;
	int cix, ciy;
	int stride;
	struct panes *ps;
	MagickBooleanType status;
	MagickWand *wd;
	int fmt[2];
	unsigned char *buf;
	cairo_surface_t *surface;

	if (!ci->str)
		return Enoarg;
	ps = find_pixmap(xd, ci->focus, &xo, &yo);
	if (!ps)
		return Einval;
	if (!ps->ctx)
		instantiate_pixmap(xd, ps);
	ps->bg.g = -1;
	if (!ps->ctx)
		return Efail;
	if (strstarts(ci->str, "file:")) {
		wd = NewMagickWand();
		status = MagickReadImage(wd, ci->str + 5);
		if (status == MagickFalse) {
			DestroyMagickWand(wd);
			return Efail;
		}
	} else if (strstarts(ci->str, "comm:")) {
		struct call_return cr;
		wd = NewMagickWand();
		cr = call_ret(bytes, ci->str+5, ci->focus);
		if (!cr.s) {
			DestroyMagickWand(wd);
			return Efail;
		}
		status = MagickReadImageBlob(wd, cr.s, cr.i);
		free(cr.s);
		if (status == MagickFalse) {
			DestroyMagickWand(wd);
			return Efail;
		}
	} else
		return Einval;

	MagickAutoOrientImage(wd);
	w = ci->focus->w;
	h = ci->focus->h;
	if (ci->num > 0 && ci->num2 > 0) {
		w = ci->num;
		h = ci->num2;
	} else if (ci->num > 0) {
		int ih = MagickGetImageHeight(wd);
		int iw = MagickGetImageWidth(wd);

		if (iw <= 0 || iw <= 0) {
			DestroyMagickWand(wd);
			return Efail;
		}
		w = iw * ci->num / 1024;
		h = ih * ci->num / 1024;
	} else if (!stretch) {
		int ih = MagickGetImageHeight(wd);
		int iw = MagickGetImageWidth(wd);

		if (iw <= 0 || iw <= 0) {
			DestroyMagickWand(wd);
			return Efail;
		}
		if (iw * h > ih * w) {
			/* Image is wider than space, use less height */
			ih = ih * w / iw;
			if (strchr(mode, 'B'))
				/* bottom */
				y = h - ih;
			else if (!strchr(mode, 'T'))
				/* center */
				y = (h - ih) / 2;
			h = ih;
		} else {
			/* image is too tall, use less width */
			iw = iw * h / ih;
			if (strchr(mode, 'R'))
				/* right */
				x = w - iw;
			else if (!strchr(mode, 'L'))
				x = (w - iw) / 2;
			w = iw;
		}
	}
	MagickAdaptiveResizeImage(wd, w, h);
	pw = ci->focus->w;
	ph = ci->focus->h;
	cix = ci->x;
	ciy = ci->y;
	if (cix < 0) {
		xo -= cix;
		pw += cix;
		cix = 0;
	}
	if (ciy < 0) {
		yo -= ciy;
		ph += ciy;
		ciy = 0;
	}
	if (w - cix <= pw)
		w -= cix;
	else
		w = pw;
	if (h - ciy <= ph)
		h -= ciy;
	else
		h = ph;
	stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, w);
	buf = malloc(h * stride);
	// Cairo expects 32bit values with A in the high byte, then RGB.
	// Magick provides 8bit values in the order requests.
	// So depending on byte order, a different string is needed

	fmt[0] = ('A'<<24) | ('R' << 16) | ('G' << 8) | ('B' << 0);
	fmt[1] = 0;
	MagickExportImagePixels(wd, cix, ciy, w, h,
				(char*)fmt, CharPixel, buf);
	surface = cairo_image_surface_create_for_data(buf, CAIRO_FORMAT_ARGB32,
						      w, h, stride);
	cairo_set_source_surface(ps->ctx, surface, x + xo, y + yo);
	cairo_paint(ps->ctx);
	cairo_surface_destroy(surface);
	free(buf);

	if (ci->focus->cx >= 0) {
		struct pane *p = ci->focus;
		int rows, cols;
		char *cl = strchr(mode, ':');
		if (cl && sscanf(cl, ":%dx%d", &cols, &rows) == 2) {
			cairo_rectangle(ps->ctx, p->cx + xo, p->cy + yo,
					w/cols, h/rows);
			cairo_set_line_width(ps->ctx, 1.0);
			cairo_set_source_rgb(ps->ctx, 1.0, 0.0, 0.0);
			cairo_stroke(ps->ctx);
		}
	}
	DestroyMagickWand(wd);

	pane_damaged(ci->home, DAMAGED_POSTORDER);

	return 1;
}

DEF_CMD(xcb_image_size)
{
	MagickBooleanType status;
	MagickWand *wd;
	int ih, iw;

	if (!ci->str)
		return Enoarg;
	if (strstarts(ci->str, "file:")) {
		wd = NewMagickWand();
		status = MagickReadImage(wd, ci->str + 5);
		if (status == MagickFalse) {
			DestroyMagickWand(wd);
			return Efail;
		}
	} else if (strstarts(ci->str, "comm:")) {
		struct call_return cr;
		wd = NewMagickWand();
		cr = call_ret(bytes, ci->str+5, ci->focus);
		if (!cr.s) {
			DestroyMagickWand(wd);
			return Efail;
		}
		status = MagickReadImageBlob(wd, cr.s, cr.i);
		free(cr.s);
		if (status == MagickFalse) {
			DestroyMagickWand(wd);
			return Efail;
		}
	} else
		return Einval;

	MagickAutoOrientImage(wd);
	ih = MagickGetImageHeight(wd);
	iw = MagickGetImageWidth(wd);

	DestroyMagickWand(wd);
	comm_call(ci->comm2, "callback:size", ci->focus,
		  0, NULL, NULL, 0, NULL, NULL,
		  iw, ih);
	return 1;
}

static struct panes *sort_split(struct panes *p)
{
	/* consider 'p' to be a list of panes with
	 * ordered subsets (ordered by p->abs_z).
	 * Remove every other such subset and return them
	 * linked together.
	 * If p is ordered, this means we return NULL.
	 */
	struct panes *ret, **end = &ret;
	struct panes *next;

	for (; p && p->next; p = next) {
		/* If these are not ordered, attach p->next at
		 * 'end', and make 'end' point to &p->next.
		 */
		next = p->next;
		if (p->p->abs_z < next->p->abs_z) {
			*end = next;
			end = &p->next;
		}
	}
	*end = NULL;
	return ret;
}

static struct panes *sort_merge(struct panes *p1, struct panes *p2)
{
	/* merge p1 and p2 and return result */
	struct panes *ret, **end = &ret;
	struct panes *prev = NULL;

	while (p1 && p2) {
		/* Make p1 the largest (or be added first.
		 * Then in prev is between them add p2, else p1
		 */
		if (p1->p->abs_z < p2->p->abs_z) {
			struct panes *t = p1;
			p1 = p2;
			p2 = t;
		}
		if (prev &&
		    p1->p->abs_z > prev->p->abs_z &&
		    prev->p->abs_z >= p2->p->abs_z) {
			/* p2 is the better choice */
			prev = p2;
			p2 = p2->next;
		} else {
			prev = p1;
			p1 = p1->next;
		}
		*end = prev;
		end = &prev->next;
	}
	if (p1)
		*end = p1;
	else
		*end = p2;
	return ret;
}

DEF_CMD(xcb_refresh_post)
{
	struct xcb_data *xd = ci->home->data;
	struct panes *ps;

	time_start(TIME_WINDOW);
	/* First: ensure panes are sorted */
	while ((ps = sort_split(xd->panes)) != NULL)
		xd->panes = sort_merge(xd->panes, ps);

	/* Then merge all update rectanges, checking for movement */
	if (!xd->need_update)
		xd->need_update = cairo_region_create();
	for (ps = xd->panes; ps ; ps = ps->next)
	{
		struct xy rel;

		rel = pane_mapxy(ps->p, ci->home, 0, 0, False);
		if (ps->r.x == NEVER_DRAWN) {
			ps->r.x = rel.x;
			ps->r.y = rel.y;
			cairo_region_union_rectangle(xd->need_update, &ps->r);
		} else if (rel.x != ps->r.x || rel.y != ps->r.y) {
			/* Moved, so refresh all.
			 * This rectangle might be too big if it is clipped,
			 * but that doesn't really matter.
			 */
			cairo_region_union_rectangle(xd->need_update, &ps->r);
			ps->r.x = rel.x;
			ps->r.y = rel.y;
			cairo_region_union_rectangle(xd->need_update, &ps->r);
		} else if (ps->need_update) {
			cairo_region_translate(ps->need_update, rel.x, rel.y);
			cairo_region_union(xd->need_update, ps->need_update);
		}
		if (ps->need_update)
			cairo_region_destroy(ps->need_update);
		ps->need_update = NULL;
	}
	/* Now copy all panes onto the window where an update is needed */
	for (ps = xd->panes; ps ; ps = ps->next) {
		struct xy rel, lo, hi;
		cairo_region_t *cr;
		cairo_rectangle_int_t r;
		int nr, i;

		cr = cairo_region_copy(xd->need_update);

		rel = pane_mapxy(ps->p, ci->home, 0, 0, False);

		cairo_save(xd->cairo);
		if (ps->bg.g >= 0)
			cairo_set_source_rgb(xd->cairo,
					     ps->bg.r, ps->bg.g, ps->bg.b);
		else
			cairo_set_source_surface(xd->cairo, ps->surface,
						 rel.x, rel.y);

		lo = pane_mapxy(ps->p, ci->home, 0, 0, True);
		hi = pane_mapxy(ps->p, ci->home, ps->r.width, ps->r.height, True);
		r.x = lo.x; r.y = lo.y;
		r.width = hi.x - lo.x; r.height = hi.y - lo.y;
		cairo_region_intersect_rectangle(cr, &r);
		cairo_region_subtract_rectangle(xd->need_update, &r);
		nr = cairo_region_num_rectangles(cr);
		for (i = 0; i < nr; i++) {
			cairo_region_get_rectangle(cr, i, &r);
			cairo_rectangle(xd->cairo, r.x, r.y, r.width, r.height);
			cairo_fill(xd->cairo);
		}
		cairo_restore(xd->cairo);
	}

	cairo_region_destroy(xd->need_update);
	xd->need_update = NULL;
	time_stop(TIME_WINDOW);
	xcb_flush(xd->conn);
	return 1;
}

DEF_CMD(xcb_refresh_size)
{
	/* FIXME: should I consider resizing the window?
	 * For now, just ensure we redraw everything.
	 */
	struct xcb_data *xd = ci->home->data;
	cairo_rectangle_int_t r = {
		.x = 0,
		.y = 0,
		.width = ci->home->w,
		.height = ci->home->h,
	};

	if (!xd->need_update)
		xd->need_update = cairo_region_create();
	cairo_region_union_rectangle(xd->need_update, &r);
	/* Ask common code to notify children */
	return Efallthrough;
}

DEF_CMD(xcb_pane_close)
{
	struct xcb_data *xd = ci->home->data;
	struct panes **pp, *ps;

	for (pp = &xd->panes; (ps = *pp) != NULL; pp = &(*pp)->next) {
		if (ps->p != ci->focus)
			continue;

		if (!xd->need_update)
			xd->need_update = cairo_region_create();
		if (ps->r.x != NEVER_DRAWN)
			cairo_region_union_rectangle(xd->need_update, &ps->r);

		*pp = ps->next;
		ps->next = NULL;
		if (ps->need_update)
			cairo_region_destroy(ps->need_update);
		cairo_destroy(ps->ctx);
		cairo_surface_destroy(ps->surface);
		xcb_free_pixmap(xd->conn, ps->draw);
		free(ps);
		pane_damaged(ci->home, DAMAGED_POSTORDER);
		break;
	}
	return 1;
}

DEF_CMD(xcb_notify_display)
{
	struct xcb_data *xd = ci->home->data;
	comm_call(ci->comm2, "callback:display", ci->home, xd->last_event);
	return 1;
}

static void handle_button(struct pane *home safe,
			  xcb_button_press_event_t *be safe)
{
	struct xcb_data *xd = home->data;
	bool press = (be->response_type & 0x7f) == XCB_BUTTON_PRESS;
	char mod[2+2+2+1];
	char key[2+2+2+9+1+1];

	xcb_set_input_focus(xd->conn, XCB_INPUT_FOCUS_POINTER_ROOT,
			    xd->win, XCB_CURRENT_TIME);
	mod[0] = 0;
	if (press) {
		xd->motion_blocked = False;
		if (be->state & XCB_KEY_BUT_MASK_MOD_1)
			strcat(mod, ":A");
		if (be->state & XCB_KEY_BUT_MASK_CONTROL)
			strcat(mod, ":C");
		if (be->state & XCB_KEY_BUT_MASK_SHIFT)
			strcat(mod, ":S");
		strcpy(key, mod);
		strcat(key, ":Press-X");
	} else if (be->detail >= 4)
		/* ignore 'release' for scroll wheel */
		return;
	else
		strcpy(key, ":Release-X");

	key[strlen(key) - 1] = '0' + be->detail;
	xd->last_event = time(NULL);
	call("Mouse-event", home, be->detail, NULL, key,
	     press?1:2, NULL, mod,
	     be->event_x, be->event_y);
}

static void handle_motion(struct pane *home safe,
			  xcb_motion_notify_event_t *mne safe)
{
	struct xcb_data *xd = home->data;
	xcb_query_pointer_cookie_t c;
	xcb_query_pointer_reply_t *qpr;
	int ret;
	int x = mne->event_x, y = mne->event_y;

	if (xd->motion_blocked)
		return;
	ret = call("Mouse-event", home, 0, NULL, ":Motion",
		   3, NULL, NULL, x, y);
	if (ret <= 0)
		xd->motion_blocked = True;

	/* This doesn't seem to be needed, but the spec says
	 * I should do this when using POINTER_MOTION_HINT
	 */
	c = xcb_query_pointer(xd->conn, xd->win);
	qpr = xcb_query_pointer_reply(xd->conn, c, NULL);
	free(qpr);
}

static void handle_focus(struct pane *home safe, xcb_focus_in_event_t *fie safe)
{
	struct xcb_data *xd = home->data;
	bool in = (fie->response_type & 0x7f) == XCB_FOCUS_IN;
	struct pane *p;
	struct mark *pt;

	xd->in_focus = in;
	p = pane_focus(home);
	pt = call_ret(mark, "doc:point", p);
	if (pt)
		call("view:changed", p, 0, pt);
	if (in)
		call("pane:refocus", home);
}

static bool select_xkb_events_for_device(xcb_connection_t *conn,
					 int32_t device_id)
{
	xcb_generic_error_t *error;
	xcb_void_cookie_t cookie;

	enum {
		required_events =
			(XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY |
			 XCB_XKB_EVENT_TYPE_MAP_NOTIFY |
			 XCB_XKB_EVENT_TYPE_STATE_NOTIFY),

		required_nkn_details =
			(XCB_XKB_NKN_DETAIL_KEYCODES),

		required_map_parts =
			(XCB_XKB_MAP_PART_KEY_TYPES |
			 XCB_XKB_MAP_PART_KEY_SYMS |
			 XCB_XKB_MAP_PART_MODIFIER_MAP |
			 XCB_XKB_MAP_PART_EXPLICIT_COMPONENTS |
			 XCB_XKB_MAP_PART_KEY_ACTIONS |
			 XCB_XKB_MAP_PART_VIRTUAL_MODS |
			 XCB_XKB_MAP_PART_VIRTUAL_MOD_MAP),

		required_state_details =
			(XCB_XKB_STATE_PART_MODIFIER_BASE |
			 XCB_XKB_STATE_PART_MODIFIER_LATCH |
			 XCB_XKB_STATE_PART_MODIFIER_LOCK |
			 XCB_XKB_STATE_PART_GROUP_BASE |
			 XCB_XKB_STATE_PART_GROUP_LATCH |
			 XCB_XKB_STATE_PART_GROUP_LOCK),
	};

	static const xcb_xkb_select_events_details_t details = {
		.affectNewKeyboard = required_nkn_details,
		.newKeyboardDetails = required_nkn_details,
		.affectState = required_state_details,
		.stateDetails = required_state_details,
	};

	cookie = xcb_xkb_select_events_aux_checked(
		conn,
		device_id,
		required_events,	/* affectWhich */
		0,			/* clear */
		0,			/* selectAll */
		required_map_parts,	/* affectMap */
		required_map_parts,	/* map */
		&details);		/* details */

	error = xcb_request_check(conn, cookie);
	if (error) {
		free(error);
		return False;
	}

	return True;
}

static bool update_keymap(struct xcb_data *xd safe)
{
	struct xkb_keymap *new_keymap;
	struct xkb_state *new_state;

	new_keymap = xkb_x11_keymap_new_from_device(xd->xkb, xd->conn,
						    xd->xkb_device_id,
						    XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!new_keymap)
		return False;

	new_state = xkb_x11_state_new_from_device(new_keymap, xd->conn,
						  xd->xkb_device_id);
	if (!new_state) {
		xkb_keymap_unref(new_keymap);
		return False;
	}

	xkb_state_unref(xd->xkb_state);
	xkb_keymap_unref(xd->xkb_keymap);
	xd->xkb_keymap = new_keymap;
	xd->xkb_state = new_state;
	return True;
}

static bool kbd_setup(struct xcb_data *xd safe)
{
	int ret;
	const char *locale;

	ret = xkb_x11_setup_xkb_extension(xd->conn,
					  XKB_X11_MIN_MAJOR_XKB_VERSION,
					  XKB_X11_MIN_MINOR_XKB_VERSION,
					  XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
					  NULL, NULL, &xd->first_xkb_event,
					  NULL);

	if (!ret)
		return False;
	xd->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!xd->xkb)
		return False;
	xd->xkb_device_id = xkb_x11_get_core_keyboard_device_id(xd->conn);
	if (xd->xkb_device_id == -1)
		return False;

	if (!update_keymap(xd))
		return False;

	if (!select_xkb_events_for_device(xd->conn, xd->xkb_device_id))
		return False;

	locale = setlocale(LC_CTYPE, NULL);
	xd->compose_table =
		xkb_compose_table_new_from_locale(xd->xkb, locale,
						  XKB_COMPOSE_COMPILE_NO_FLAGS);
	if (xd->compose_table)
		xd->compose_state =
			xkb_compose_state_new(xd->compose_table,
					      XKB_COMPOSE_STATE_NO_FLAGS);
	return True;
}

static void kbd_free(struct xcb_data *xd safe)
{
	if (xd->compose_table)
		xkb_compose_table_unref(xd->compose_table);
	if (xd->xkb_keymap)
		xkb_keymap_unref(xd->xkb_keymap);
	if (xd->xkb)
		xkb_context_unref(xd->xkb);
}

static struct {
	char *from safe, *to safe;
} key_map[] = {
	{ "Return",	 ":Enter"},
	{ "Tab",	 ":Tab"},
	{ "ISO_Left_Tab",":Tab"},
	{ "Escape",	 ":ESC"},
	{ "Linefeed",	 ":LF"},
	{ "Down",	 ":Down"},
	{ "Up",		 ":Up"},
	{ "Left",	 ":Left"},
	{ "Right",	 ":Right"},
	{ "Home",	 ":Home"},
	{ "End",	 ":End"},
	{ "BackSpace",	 ":Backspace"},
	{ "Delete",	 ":Del"},
	{ "Insert",	 ":Ins"},
	{ "Next",	 ":Prior"},
	{ "Prior",	 ":Next"},
	{ "F1",		 ":F1"},
	{ "F2",		 ":F2"},
	{ "F3",		 ":F3"},
	{ "F4",		 ":F4"},
	{ "F5",		 ":F5"},
	{ "F6",		 ":F6"},
	{ "F7",		 ":F7"},
	{ "F8",		 ":F8"},
	{ "F9",		 ":F9"},
	{ "F10",	 ":F11"},
	{ "F11",	 ":F11"},
	{ "F12",	 ":F12"},
};

static void handle_key_press(struct pane *home safe,
			     xcb_key_press_event_t *kpe safe)
{
	struct xcb_data			*xd = home->data;
	xkb_keycode_t			keycode = kpe->detail;
	xcb_keysym_t			keysym;
	xkb_keysym_t			sym;
	const xkb_keysym_t		*syms;
	int				nsyms;
	enum xkb_compose_status		status;
	xkb_mod_index_t			mod;
	char				s[16];
	char				key[16];
	char				mods[32];
	bool				shift=False, ctrl=False, alt=False;

	xd->last_event = time(NULL);

	keysym = xkb_state_key_get_one_sym(xd->xkb_state,
					   keycode);
	if (xd->compose_state)
		xkb_compose_state_feed(xd->compose_state, keysym);
	nsyms = xkb_state_key_get_syms(xd->xkb_state, keycode,
				       &syms);
	if (nsyms <= 0)
		return;
	status = XKB_COMPOSE_NOTHING;
	if (xd->compose_state)
		status = xkb_compose_state_get_status(xd->compose_state);
	if (status == XKB_COMPOSE_COMPOSING ||
	    status == XKB_COMPOSE_CANCELLED)
		return;

	for (mod = 0; mod < xkb_keymap_num_mods(xd->xkb_keymap); mod++) {
		const char *n;
		if (xkb_state_mod_index_is_active(
			    xd->xkb_state, mod,
			    XKB_STATE_MODS_EFFECTIVE) <= 0)
			continue;
		/* This does tells me "shift" is consumed for :C:S-l ...
		if (xkb_state_mod_index_is_consumed2(
			    xd->xkb_state, keycode, mod,
			    XKB_CONSUMED_MODE_XKB))
			continue;
		 */
		n = xkb_keymap_mod_get_name(xd->xkb_keymap, mod);
		if (n && strcmp(n, "Shift") == 0)
			shift = True;
		if (n && strcmp(n, "Control") == 0)
			ctrl = True;
		if (n && strcmp(n, "Mod1") == 0)
			alt = True;
	}

	if (status == XKB_COMPOSE_COMPOSED) {
		sym = xkb_compose_state_get_one_sym(xd->compose_state);
		syms = &sym;
		nsyms = 1;
		s[0] = '-';
		xkb_compose_state_get_utf8(xd->compose_state,
					   s+1, sizeof(s)-1);
		key[0] = 0;
		shift = False;
		ctrl = False;
		/* Mod1 can still apply to a composed char */
	} else if (nsyms == 1) {
		unsigned int i;
		sym = xkb_state_key_get_one_sym(xd->xkb_state, keycode);
		syms = &sym;
		s[0] = '-';
		xkb_state_key_get_utf8(xd->xkb_state, keycode,
				       s+1, sizeof(s)-1);
		xkb_keysym_get_name(syms[0], key, sizeof(key));
		for (i = 0; i < ARRAY_SIZE(key_map); i++) {
			if (strcmp(key, key_map[i].from) == 0) {
				strcpy(s, key_map[i].to);
				break;
			}
		}
		if (s[0] == '-' && s[1] >= ' ' && s[1] < 0x7f)
			/* Shift is included */
			shift = False;
		if (s[0] == '-' && s[1] && (unsigned char)(s[1]) < ' ') {
			ctrl = True;
			s[1] += '@';
			if (s[1] < 'A' || s[1] > 'Z')
				shift = False;
		} else if (s[0] == '-' && !s[1] && strcmp(key, "space") == 0) {
			/* 'nul' becomes "C- " (ctrl-space) */
			ctrl = True;
			s[1] = ' ';
			s[2] = 0;
		}
	}

	if (xd->compose_state &&
	    (status == XKB_COMPOSE_CANCELLED ||
	     status == XKB_COMPOSE_COMPOSED))
		xkb_compose_state_reset(xd->compose_state);

	if (s[1]) {
		mods[0] = 0;
		if (alt)
			strcat(mods, ":A");
		if (ctrl)
			strcat(mods, ":C");
		if (shift)
			strcat(mods, ":S");
		strcat(mods, s);
		call("Keystroke", home, 0, NULL, mods);
	}
}

static void handle_xkb_event(struct pane *home safe,
			     xcb_generic_event_t *ev safe)
{
	struct xcb_data *xd = home->data;

	switch (ev->pad0) {
		xcb_xkb_new_keyboard_notify_event_t	*nkne;
		xcb_xkb_state_notify_event_t		*sne;
		xcb_xkb_map_notify_event_t		*mne;
	case XCB_XKB_NEW_KEYBOARD_NOTIFY:
		nkne = (void*)ev;
		if (nkne->deviceID == xd->xkb_device_id &&
		    nkne->changed & XCB_XKB_NKN_DETAIL_KEYCODES)
			update_keymap(xd);
		break;
	case XCB_XKB_MAP_NOTIFY:
		mne = (void*)ev;
		if (mne->deviceID == xd->xkb_device_id)
			update_keymap(xd);
		break;
	case XCB_XKB_STATE_NOTIFY:
		sne = (void*)ev;
		if (sne->deviceID == xd->xkb_device_id)
			xkb_state_update_mask(xd->xkb_state,
					      sne->baseMods,
					      sne->latchedMods,
					      sne->lockedMods,
					      sne->baseGroup,
					      sne->latchedGroup,
					      sne->lockedGroup);
		break;
	}
}

static void handle_configure(struct pane *home safe,
			     xcb_configure_notify_event_t *cne safe)
{
	struct xcb_data *xd = home->data;

	pane_resize(home, 0, 0, cne->width, cne->height);
	cairo_xcb_surface_set_size(xd->surface, cne->width, cne->height);
}

static void handle_expose(struct pane *home safe,
			  xcb_expose_event_t *ee safe)
{
	struct xcb_data *xd = home->data;
	cairo_rectangle_int_t r = {
		.x = ee->x,
		.y = ee->y,
		.width = ee->width,
		.height = ee->height,
	};

	if (!xd->need_update)
		xd->need_update = cairo_region_create();
	cairo_region_union_rectangle(xd->need_update, &r);
	if (ee->count == 0)
		pane_damaged(home, DAMAGED_POSTORDER);
}

static void handle_client_message(struct pane *home safe,
				  xcb_client_message_event_t *cme safe)
{
	struct xcb_data *xd = home->data;

	if (cme->type == xd->atoms[a_WM_PROTOCOLS] &&
	    cme->format == 32 &&
	    cme->window == xd->win &&
	    cme->data.data32[0] == xd->atoms[a_WM_DELETE_WINDOW]) {
		call("window:close", pane_focus(home));
		return;
	}

	if (cme->type == xd->atoms[a_WM_PROTOCOLS] &&
	    cme->format == 32 &&
	    cme->window == xd->win &&
	    cme->data.data32[0] == xd->atoms[a_NET_WM_PING]) {
		cme->window = xd->screen->root;
		xcb_send_event(xd->conn, 0, xd->screen->root,
			       XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
			       XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
			       (void*)cme);
		return;
	}
	LOG("x11 %s got unexpected client message type=%d/%d win=%x data=%d",
	    xd->display,
	    cme->type, cme->format, cme->window, cme->data.data32[0]);

}

DEF_CMD(xcb_input)
{
	struct xcb_data *xd = ci->home->data;
	xcb_generic_event_t *ev;
	int ret = 1;

	wait_for(xd);
	if (ci->num < 0)
		/* This is a poll - only return 1 on something happening */
		ret = Efalse;

	while ((ev = xcb_poll_for_event(xd->conn)) != NULL) {
		ret = 1;
		switch (ev->response_type & 0x7f) {
		case XCB_KEY_PRESS:
			time_start(TIME_KEY);
			handle_key_press(ci->home, safe_cast (void*)ev);
			time_stop(TIME_KEY);
			break;
		case XCB_KEY_RELEASE:
			/* Ignore for now */
			break;
		case XCB_BUTTON_PRESS:
		case XCB_BUTTON_RELEASE:
			time_start(TIME_KEY);
			handle_button(ci->home, (void*)ev);
			time_stop(TIME_KEY);
			break;
		case XCB_MOTION_NOTIFY:
			time_start(TIME_KEY);
			handle_motion(ci->home, (void*)ev);
			time_stop(TIME_KEY);
			break;
		case XCB_FOCUS_IN:
		case XCB_FOCUS_OUT:
			time_start(TIME_WINDOW);
			handle_focus(ci->home, (void*)ev);
			time_stop(TIME_WINDOW);
			break;
		case XCB_EXPOSE:
			time_start(TIME_WINDOW);
			handle_expose(ci->home, (void*)ev);
			time_stop(TIME_WINDOW);
			break;
		case XCB_CONFIGURE_NOTIFY:
			time_start(TIME_WINDOW);
			handle_configure(ci->home, (void*)ev);
			time_stop(TIME_WINDOW);
			break;
		case XCB_CLIENT_MESSAGE:
			time_start(TIME_WINDOW);
			handle_client_message(ci->home, (void*)ev);
			time_stop(TIME_WINDOW);
			break;
		case XCB_REPARENT_NOTIFY:
			/* Not interested */
			break;
		case XCB_MAP_NOTIFY:
		case XCB_UNMAP_NOTIFY:
		case XCB_MAPPING_NOTIFY:
			/* FIXME what to do?? */
			break;
		case 0:
			/* Don't know what this means, but I get a lot
			 * of them so I don't want to log that it was
			 * ignored.
			 */
			break;
		default:
			if ((ev->response_type & 0x7f) ==
			    xd->first_xkb_event) {
				handle_xkb_event(ci->home, ev);
				break;
			}
			LOG("Ignored X11 event %d", ev->response_type);
		}
		xcb_flush(xd->conn);
	}
	if (xcb_connection_has_error(xd->conn)) {
		call("window:close", ci->home->parent);
		pane_close(ci->home);
	}
	return ret;
}

static void set_str_prop(struct xcb_data *xd safe,
			 enum my_atoms a, const char *str safe)
{
	xcb_change_property(xd->conn,
			    XCB_PROP_MODE_REPLACE,
			    xd->win, xd->atoms[a], XCB_ATOM_STRING,
			    8, strlen(str), str);
}

static void set_utf8_prop(struct xcb_data *xd safe,
			 enum my_atoms a, const char *str safe)
{
	xcb_change_property(xd->conn,
			    XCB_PROP_MODE_REPLACE,
			    xd->win, xd->atoms[a],
			    xd->atoms[a_UTF8_STRING],
			    8, strlen(str), str);
}

static void set_card32_property(struct xcb_data *xd safe,
				enum my_atoms a,
				const uint32_t *data, int cnt)
{
	xcb_change_property(xd->conn,
			    XCB_PROP_MODE_REPLACE,
			    xd->win, xd->atoms[a],
			    XCB_ATOM_CARDINAL, 32,
			    cnt, data);
}

static void set_atom_prop(struct xcb_data *xd safe,
			  enum my_atoms prop, enum my_atoms alist, ...)
{
	uint32_t atoms[16];
	int anum = 0;
	va_list ap;
	enum my_atoms a;

	atoms[anum++] = xd->atoms[alist];
	va_start(ap, alist);
	while ((a = va_arg(ap, enum my_atoms)) != a_NONE)
		if (anum < 16)
			atoms[anum++] = xd->atoms[a];
	va_end(ap);
	xcb_change_property(xd->conn,
			    XCB_PROP_MODE_REPLACE,
			    xd->win, xd->atoms[prop],
			    XCB_ATOM_ATOM,
			    32, anum, atoms);
}

static void xcb_load_icon(struct pane *p safe,
			  struct xcb_data *xd safe,
			  char *file safe)
{
	char *path;
	int h, w, n;
	unsigned int *data;
	MagickBooleanType status;
	MagickWand *wd;
	uint32_t fmt[2];

	path = call_ret(str, "xdg-find-edlib-file", p, 0, NULL,
			file, 0, NULL, "data");
	if (!path)
		return;

	wd = NewMagickWand();
	status = MagickReadImage(wd, path);
	free(path);
	if (status == MagickFalse)
		goto done;

	h = MagickGetImageHeight(wd);
	w = MagickGetImageWidth(wd);
	n = 2 + w*h;
	data = malloc(sizeof(data[0]) * n);
	if (!data)
		goto done;
	data[0] = w;
	data[1] = h;
	/* Need host-endian ARGB data */
	fmt[0] = ('A'<<24) | ('R' << 16) | ('G' << 8) | ('B' << 0);
	fmt[1] = 0;
	MagickExportImagePixels(wd, 0, 0, w, h, (char*)fmt,
				CharPixel, data+2);
	set_card32_property(xd, a_NET_WM_ICON, data, n);
	free(data);
done:
	DestroyMagickWand(wd);
	return;
}

static struct pane *xcb_display_init(const char *d safe,
				     const char *disp_auth,
				     struct pane *focus safe)
{
	struct xcb_data *xd;
	struct pane *p;
	xcb_connection_t *conn;
	xcb_intern_atom_cookie_t cookies[NR_ATOMS];
	xcb_screen_iterator_t iter;
	xcb_depth_iterator_t di;
	char scale[20];
	char hostname[128];
	uint32_t valwin[2];
	int screen = 0;
	int i;
	PangoLayout *layout;
	PangoRectangle log;
	cairo_t *cairo;
	cairo_surface_t *surface;
	PangoFontDescription *fd;
	// FIXME SCALE from environ?? or pango_cairo_context_set_resolution dpi
	// 254 * width_in_pixels / width_in_millimeters / 10

	conn = safe_cast xcb_connect_auth(d, disp_auth, &screen);
	if (xcb_connection_has_error(conn))
		return NULL;

	p = pane_register(pane_root(focus), 1, &xcb_handle.c);
	if (!p)
		return NULL;
	xd = p->data;

	xd->motion_blocked = True;
	xd->in_focus = True;

	xd->conn = conn;
	xd->display = strdup(d);
	if (disp_auth)
		xd->disp_auth = strdup(disp_auth);
	xd->setup = safe_cast xcb_get_setup(conn);
	iter = xcb_setup_roots_iterator(xd->setup);
	for (i = 0; i < screen; i++)
		xcb_screen_next(&iter);
	xd->screen = safe_cast iter.data;

	di = xcb_screen_allowed_depths_iterator(xd->screen);
	while (di.data && di.data->depth < 24)
		xcb_depth_next(&di);
	//?? look for class = TrueColor??
	xd->visual = xcb_depth_visuals(di.data);

	for (i = 0; i < NR_ATOMS; i++) {
		const char *n = atom_names[i];
		if (!n)
			continue;
		cookies[i] = xcb_intern_atom(conn, 0, strlen(n), n);
	}

	xd->win = xcb_generate_id(conn);
	valwin[0] = xd->screen->white_pixel;
	valwin[1] = (XCB_EVENT_MASK_KEY_PRESS |
		     XCB_EVENT_MASK_KEY_RELEASE |
		     XCB_EVENT_MASK_BUTTON_PRESS |
		     XCB_EVENT_MASK_BUTTON_RELEASE |
		     // XCB_EVENT_MASK_ENTER_WINDOW |
		     // XCB_EVENT_MASK_LEAVE_WINDOW |
		     XCB_EVENT_MASK_FOCUS_CHANGE |
		     XCB_EVENT_MASK_STRUCTURE_NOTIFY |
		     XCB_EVENT_MASK_EXPOSURE |
		     XCB_EVENT_MASK_BUTTON_MOTION |
		     XCB_EVENT_MASK_POINTER_MOTION_HINT |
		     0);

	xcb_create_window(conn, XCB_COPY_FROM_PARENT, xd->win,
			  xd->screen->root,
			  0, 0,
			  100, 100,
			  0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  xd->screen->root_visual,
			  XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK,
			  valwin);
	xcb_flush(conn);
	kbd_setup(xd);

	surface = cairo_xcb_surface_create(
		conn, xd->win, xd->visual, 100, 100);
	if (!surface)
		goto abort;
	xd->surface = surface;
	cairo = safe_cast cairo_create(xd->surface);
	if (cairo_status(cairo) != CAIRO_STATUS_SUCCESS)
		goto abort;
	xd->cairo = cairo;
	fd = pango_font_description_new();
	if (!fd)
		goto abort;
	xd->fd = fd;
	pango_font_description_set_family(xd->fd, "monospace");
	pango_font_description_set_size(xd->fd, 12 * PANGO_SCALE);

	layout = pango_cairo_create_layout(xd->cairo);
	pango_layout_set_font_description(layout, fd);
	pango_layout_set_text(layout, "M", 1);
	pango_layout_get_pixel_extents(layout, NULL, &log);
	g_object_unref(layout);
	xd->lineheight = log.height;
	xd->charwidth = log.width;

	valwin[0] = xd->charwidth * 80;
	valwin[1] = xd->lineheight * 26;
	xcb_configure_window(conn, xd->win,
			     XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
			     valwin);
	cairo_xcb_surface_set_size(xd->surface, valwin[0], valwin[1]);

	/* Now resolve all those cookies */
	for (i = 0; i < NR_ATOMS; i++) {
		xcb_intern_atom_reply_t *r;
		r = xcb_intern_atom_reply(conn, cookies[i], NULL);
		if (!r)
			goto abort;
		xd->atoms[i] = r->atom;
		free(r);
	}

	/* FIXME set:
	 *
	 * WM_PROTOCOLS _NET_WM_SYN_REQUEST??
	 * WM_NORMAL_HINTS WM_HINTS
	 * WM_CLIENT_MACHINE
	 */
	set_str_prop(xd, a_WM_NAME, "EdLib");
	set_utf8_prop(xd, a_NET_WM_NAME, "EdLib");
	set_str_prop(xd, a_WM_ICON_NAME, "EdLib");
	set_utf8_prop(xd, a_NET_WM_ICON_NAME, "EdLib");
	gethostname(hostname, sizeof(hostname));
	set_str_prop(xd, a_WM_CLIENT_MACHINE, hostname);
	set_atom_prop(xd, a_WM_PROTOCOLS, a_WM_DELETE_WINDOW, a_NET_WM_PING, 0);

	/* Configure passive grabs - shift, lock, and control only */
	xcb_grab_button(xd->conn, 0, xd->win,
			XCB_EVENT_MASK_BUTTON_PRESS |
			XCB_EVENT_MASK_BUTTON_RELEASE |
			XCB_EVENT_MASK_BUTTON_MOTION,
			XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
			XCB_WINDOW_NONE, XCB_CURSOR_NONE,
			XCB_BUTTON_INDEX_ANY,
			XCB_MOD_MASK_SHIFT |
			XCB_MOD_MASK_LOCK |
			XCB_MOD_MASK_CONTROL);

	xcb_load_icon(focus, xd, "{COMM}-icon.png");
	xcb_map_window(conn, xd->win);
	xcb_flush(conn);
	pane_resize(p, 0, 0, xd->charwidth*80, xd->lineheight*26);
	call_comm("event:read", p, &xcb_input, xcb_get_file_descriptor(conn));
	call_comm("event:poll", p, &xcb_input);
	attr_set_str(&p->attrs, "DISPLAY", d);
	attr_set_str(&p->attrs, "XAUTHORITY", disp_auth);
	snprintf(scale, sizeof(scale), "%dx%d", xd->charwidth, xd->lineheight);
	attr_set_str(&p->attrs, "scale:M", scale);
	xd->last_event = time(NULL);
	call("editor:request:all-displays", p);
	p = call_ret(pane, "editor:activate-display", p);
	return p;
abort:
	kbd_free(xd);
	cairo_destroy(xd->cairo);
	cairo_surface_destroy(xd->surface);
	xcb_disconnect(conn);
	free(xd->display);
	free(xd->disp_auth);
	return NULL;
}

DEF_CMD(xcb_new_display)
{
	struct pane *p;
	const char *d = ci->str;
	const char *disp_auth = ci->str2;

	if (!d)
		d = pane_attr_get(ci->focus, "DISPLAY");
	if (!disp_auth)
		disp_auth = pane_attr_get(ci->focus, "XAUTHORITY");
	if (!disp_auth)
		disp_auth = getenv("XAUTHORITY");

	if (!d)
		return Enoarg;
	p = xcb_display_init(d, disp_auth, ci->focus);
	if (p)
		home_call_ret(pane, ci->focus, "doc:attach-view", p, 1);
	if (p)
		comm_call(ci->comm2, "cb", p);
	return 1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &xcb_new_display, 0, NULL,
		  "attach-display-x11");
	call_comm("global-set-command", ed, &xcb_new_display, 0, NULL,
		  "interactive-cmd-x11window");

	xcb_map = key_alloc();

	key_add(xcb_map, "window:close", &xcb_close_display);
	key_add(xcb_map, "window:external-viewer", &xcb_external_viewer);
	key_add(xcb_map, "window:fullscreen", &xcb_fullscreen);
	key_add(xcb_map, "window:new", &xcb_new_display);

	key_add(xcb_map, "Close", &xcb_close);
	key_add(xcb_map, "Draw:clear", &xcb_clear);
	key_add(xcb_map, "Draw:text-size", &xcb_text_size);
	key_add(xcb_map, "Draw:text", &xcb_draw_text);
	key_add(xcb_map, "Draw:image", &xcb_draw_image);
	key_add(xcb_map, "Draw:image-size", &xcb_image_size);
	key_add(xcb_map, "Refresh:size", &xcb_refresh_size);
	key_add(xcb_map, "Refresh:postorder", &xcb_refresh_post);
	key_add(xcb_map, "all-displays", &xcb_notify_display);
	key_add(xcb_map, "Notify:Close", &xcb_pane_close);
}
