/*
 * gui.c - GUI for eeshow
 *
 * Written 2016 by Werner Almesberger
 * Copyright 2016 by Werner Almesberger
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * Resources:
 *
 * http://zetcode.com/gfx/cairo/cairobackends/
 * https://developer.gnome.org/gtk3/stable/gtk-migrating-2-to-3.html
 */

#define	_GNU_SOURCE	/* for asprintf */
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include <cairo/cairo.h>
#include <gtk/gtk.h>

#include "util.h"
#include "style.h"
#include "cro.h"
#include "gfx.h"
#include "git-hist.h"
#include "sch.h"
#include "delta.h"
#include "diff.h"
#include "gui-aoi.h"
#include "gui-over.h"
#include "gui.h"


struct gui_ctx;

struct gui_sheet {
	const struct sheet *sch;
	struct cro_ctx *gfx_ctx;

	int w, h;
	int xmin, ymin;

	bool rendered;		/* 0 if still have to render it */

	struct aoi *aois;	/* areas of interest; in schematics coord  */

	struct gui_sheet *next;
};

struct gui_hist {
	struct gui_ctx *ctx;		/* back link */
	struct hist *hist;
	struct overlay *over;		/* current overlay */
	struct gui_sheet *sheets;	/* NULL if failed */
	struct gui_hist *next;
};

struct gui_ctx {
	GtkWidget *da;

	int curr_x;		/* last on-screen mouse position */
	int curr_y;

	unsigned zoom;		/* scale by 1.0 / (1 << zoom) */
	int x, y;		/* center, in eeschema coordinates */

	bool panning;
	int pan_x, pan_y;

	struct gui_hist *hist;	/* revision history; NULL if none */
	struct hist *vcs_hist;	/* underlying VCS data; NULL if none */

	struct overlay *sheet_overlays;
	struct overlay *hist_overlays;
	struct aoi *aois;	/* areas of interest; in canvas coord  */

	struct gui_sheet delta_a;
	struct gui_sheet delta_b;
	struct gui_sheet delta_ab;

	struct gui_sheet *curr_sheet;
				/* current sheet */
	struct gui_hist *curr_hist;
	struct gui_hist *last_hist;
};


/* ----- Helper functions -------------------------------------------------- */


static void redraw(const struct gui_ctx *ctx)
{
	gtk_widget_queue_draw(ctx->da);
}


static struct gui_sheet *find_corresponding_sheet(struct gui_sheet *pick_from,
     struct gui_sheet *ref_in, const struct gui_sheet *ref)
{
	struct gui_sheet *sheet, *plan_b;
	const char *title = ref->sch->title;

	/* plan A: try to find sheet with same name */

	if (title)
		for (sheet = pick_from; sheet; sheet = sheet->next)
			if (sheet->sch->title &&
			    !strcmp(title, sheet->sch->title))
				return sheet;

	/* plan B: use sheet in same position in sheet sequence */

	plan_b = ref_in;
	for (sheet = pick_from; sheet; sheet = sheet->next) {
		if (plan_b == ref)
			return sheet;
		plan_b = plan_b->next;
	}

	/* plan C: just go to the top */
	return pick_from;
}


/* ----- Rendering --------------------------------------------------------- */


#define	VCS_OVERLAYS_X		5
#define	VCS_OVERLAYS_Y		5

#define	SHEET_OVERLAYS_X	-10
#define	SHEET_OVERLAYS_Y	10


static void hack(const struct gui_ctx *ctx, cairo_t *cr)
{
	const struct gui_sheet *a = ctx->curr_sheet;
	const struct gui_sheet *b = find_corresponding_sheet(
	    ctx->last_hist->sheets, ctx->curr_hist->sheets, ctx->curr_sheet);

	diff_to_canvas(cr, ctx->x, ctx->y, 1.0 / (1 << ctx->zoom),
	    a->gfx_ctx, b->gfx_ctx);
}


static gboolean on_draw_event(GtkWidget *widget, cairo_t *cr,
    gpointer user_data)
{
	const struct gui_ctx *ctx = user_data;
	const struct gui_sheet *sheet = ctx->curr_sheet;
	GtkAllocation alloc;
	float f = 1.0 / (1 << ctx->zoom);
	int x, y;

	gtk_widget_get_allocation(ctx->da, &alloc);
	x = -(sheet->xmin + ctx->x) * f + alloc.width / 2;
	y = -(sheet->ymin + ctx->y) * f + alloc.height / 2;

	cro_canvas_prepare(cr);
	if (!ctx->last_hist) {
		cro_canvas_draw(sheet->gfx_ctx, cr, x, y, f);
	} else {
#if 0
		/* @@@ fix geometry later */
		cro_canvas_draw(ctx->delta_ab.gfx_ctx, cr, x, y, f);
		cro_canvas_draw(ctx->delta_a.gfx_ctx, cr, x, y, f);
		cro_canvas_draw(ctx->delta_b.gfx_ctx, cr, x, y, f);
#endif
		hack(ctx, cr);
	}

	overlay_draw_all(ctx->sheet_overlays, cr,
	    SHEET_OVERLAYS_X, SHEET_OVERLAYS_Y);
	overlay_draw_all(ctx->hist_overlays, cr,
	    VCS_OVERLAYS_X, VCS_OVERLAYS_Y);

	return FALSE;
}


static void render_sheet(struct gui_sheet *sheet)
{
	char *argv[] = { "gui", NULL };

	gfx_init(&cro_canvas_ops, 1, argv);
	sch_render(sheet->sch);
	cro_canvas_end(gfx_ctx,
	    &sheet->w, &sheet->h, &sheet->xmin, &sheet->ymin);
	sheet->gfx_ctx = gfx_ctx;
	sheet->rendered = 1;
	// gfx_end();
}


/* @@@ not nice to have this so far out */
static void mark_aois(struct gui_ctx *ctx, struct gui_sheet *sheet);


static void render_delta(struct gui_ctx *ctx)
{
#if 0
	struct sheet *sch_a, *sch_b, *sch_ab;
	const struct gui_sheet *a = ctx->curr_sheet;
	const struct gui_sheet *b = find_corresponding_sheet(
	    ctx->last_hist->sheets, ctx->curr_hist->sheets, ctx->curr_sheet);

	sch_a = alloc_type(struct sheet);
	sch_b = alloc_type(struct sheet);
	sch_ab = alloc_type(struct sheet);

	delta(a->sch, b->sch, sch_a, sch_b, sch_ab);
	ctx->delta_a.sch = sch_a,
	ctx->delta_b.sch = sch_b,
	ctx->delta_ab.sch = sch_ab,

	render_sheet(&ctx->delta_a);
	render_sheet(&ctx->delta_b);
	render_sheet(&ctx->delta_ab);

	cro_color_override(ctx->delta_ab.gfx_ctx, COLOR_LIGHT_GREY);
	cro_color_override(ctx->delta_a.gfx_ctx, COLOR_RED);
	cro_color_override(ctx->delta_b.gfx_ctx, COLOR_GREEN2);

	// @@@ clean up when leaving sheet
#endif
	struct gui_sheet *b = find_corresponding_sheet(
	    ctx->last_hist->sheets, ctx->curr_hist->sheets, ctx->curr_sheet);

	if (!b->rendered) {
		render_sheet(b);
		mark_aois(ctx, b);
	}
}


/* ----- Tools ------------------------------------------------------------- */


static void canvas_coord(const struct gui_ctx *ctx,
    int ex, int ey, int *x, int *y)
{
	GtkAllocation alloc;
	int sx, sy;

	gtk_widget_get_allocation(ctx->da, &alloc);
	sx = ex - alloc.width / 2;
	sy = ey - alloc.height / 2;
	*x = (sx << ctx->zoom) + ctx->x;
	*y = (sy << ctx->zoom) + ctx->y;
}


/* ----- Panning ----------------------------------------------------------- */


static void pan_begin(struct gui_ctx *ctx, int x, int y)
{
	if (ctx->panning)
		return;
	ctx->panning = 1;
	ctx->pan_x = x;
	ctx->pan_y = y;
}


static void pan_update(struct gui_ctx *ctx, int x, int y)
{
	if (!ctx->panning)
		return;

	ctx->x -= (x - ctx->pan_x) << ctx->zoom;
	ctx->y -= (y - ctx->pan_y) << ctx->zoom;
	ctx->pan_x = x;
	ctx->pan_y = y;

	redraw(ctx);
}


static void pan_end(struct gui_ctx *ctx, int x, int y)
{
	pan_update(ctx, x, y);
	ctx->panning = 0;
}


/* ----- Zoom -------------------------------------------------------------- */



static void zoom_in(struct gui_ctx *ctx, int x, int y)
{
	if (ctx->zoom == 0)
		return;
	ctx->zoom--;
	ctx->x = (ctx->x + x) / 2;
	ctx->y = (ctx->y + y) / 2;
	redraw(ctx);
}


static void zoom_out(struct gui_ctx *ctx, int x, int y)
{
	if (ctx->curr_sheet->w >> ctx->zoom <= 16)
		return;
	ctx->zoom++;
	ctx->x = 2 * ctx->x - x;
	ctx->y = 2 * ctx->y - y;
	redraw(ctx);
}


static void curr_sheet_size(struct gui_ctx *ctx, int *w, int *h)
{
	const struct gui_sheet *sheet = ctx->curr_sheet;

	*w = sheet->w;
	*h = sheet->h;
}


static void zoom_to_extents(struct gui_ctx *ctx)
{
	GtkAllocation alloc;
	int w, h;

	curr_sheet_size(ctx, &w, &h);
	ctx->x = w / 2;
	ctx->y = h / 2;

	gtk_widget_get_allocation(ctx->da, &alloc);
	ctx->zoom = 0;
	while (w >> ctx->zoom > alloc.width || h >> ctx->zoom > alloc.height)
		ctx->zoom++;

	redraw(ctx);
}


/* ----- Need this for jumping around -------------------------------------- */


static void go_to_sheet(struct gui_ctx *ctx, struct gui_sheet *sheet);
static bool go_up_sheet(struct gui_ctx *ctx);


/* ----- Overlay styles ---------------------------------------------------- */


static struct overlay_style style_dense;
static struct overlay_style style_dense_selected;
static struct overlay_style style_dense_diff_new;
static struct overlay_style style_dense_diff_old;


static void setup_styles(void)
{
	style_dense = overlay_style_dense;
	style_dense_selected = overlay_style_dense_selected;
	style_dense_diff_new = overlay_style_diff_new;
	style_dense_diff_old = overlay_style_diff_old;
	style_dense.wmax = style_dense_selected.wmax =
	    style_dense_diff_new.wmax = style_dense_diff_old.wmax = 400;
}


/* ----- Revision history -------------------------------------------------- */


static void hide_history(struct gui_ctx *ctx)
{
	overlay_remove_all(&ctx->hist_overlays);
	redraw(ctx);
}


static bool hover_history(void *user, bool on)
{
	struct gui_hist *h = user;
	struct gui_ctx *ctx = h->ctx;
	char *s;

	if (on) {
		s = vcs_git_long_for_pango(h->hist);
		overlay_text_raw(h->over, s);
		free(s);
	} else {
		overlay_text(h->over, "<small>%s</small>",
		    vcs_git_summary(h->hist));
	}
	redraw(ctx);
	return 1;
}


//static void do_sheet_overlays(struct gui_ctx *ctx);
static void do_revision_overlays(struct gui_ctx *ctx);


static void go_to_history(struct gui_hist *h)
{
	struct gui_ctx *ctx = h->ctx;

	if (h == ctx->curr_hist) {
		ctx->last_hist = NULL;
		do_revision_overlays(ctx);
		redraw(ctx);
	} else {
		ctx->last_hist = ctx->curr_hist;
		ctx->curr_hist = h;
		go_to_sheet(ctx, find_corresponding_sheet(h->sheets,
		    ctx->last_hist->sheets, ctx->curr_sheet));
	}
}


static void click_history(void *user)
{
	struct gui_hist *h = user;
	struct gui_ctx *ctx = h->ctx;

	hide_history(ctx);
	if (h->sheets)
		go_to_history(h);
}


static void show_history(struct gui_ctx *ctx)
{
	struct gui_hist *h = ctx->hist;

	overlay_remove_all(&ctx->hist_overlays);
	for (h = ctx->hist; h; h = h->next) {
		h->over = overlay_add(&ctx->hist_overlays, &ctx->aois,
		    hover_history, click_history, h);
		hover_history(h, 0);
		overlay_style(h->over,
		    h == ctx->curr_hist ?
		      ctx->last_hist ?
		        &style_dense_diff_new : &style_dense_selected :
		    h == ctx->last_hist ? &style_dense_diff_old : &style_dense);
	}
	redraw(ctx);
}


static void show_history_cb(void *user)
{
	struct gui_hist *h = user;
	struct gui_ctx *ctx = h->ctx;

	show_history(ctx);
}


/* ----- Navigate sheets --------------------------------------------------- */


/* @@@ find a better place for this forward declaration */
static void mark_aois(struct gui_ctx *ctx, struct gui_sheet *sheet);


static void close_subsheet(void *user)
{
	struct gui_ctx *ctx = user;

	go_up_sheet(ctx);
}


static bool show_history_details(void *user, bool on)
{
	struct gui_hist *h = user;
	struct gui_ctx *ctx = h->ctx;
	char *s;

	if (on) {
		s = vcs_git_long_for_pango(h->hist);
		overlay_text_raw(h->over, s);
		free(s);
	} else {
		overlay_text(h->over, "%.40s", vcs_git_summary(h->hist));
	}
	redraw(ctx);
	return 1;
}


static void do_revision_overlays(struct gui_ctx *ctx)
{
	overlay_remove_all(&ctx->hist_overlays);
	if (ctx->curr_hist) {
		ctx->curr_hist->over = overlay_add(&ctx->hist_overlays,
		    &ctx->aois, show_history_details, show_history_cb,
		    ctx->curr_hist);
		overlay_style(ctx->curr_hist->over, &overlay_style_default);
		show_history_details(ctx->curr_hist, 0);
	}
	if (ctx->last_hist) {
		ctx->last_hist->over = overlay_add(&ctx->hist_overlays,
		    &ctx->aois, show_history_details, click_history,
		    ctx->last_hist);
		overlay_style(ctx->curr_hist->over, &overlay_style_diff_new);
		overlay_style(ctx->last_hist->over, &overlay_style_diff_old);
		show_history_details(ctx->last_hist, 0);
	}
}


static void do_sheet_overlays(struct gui_ctx *ctx)
{
	struct overlay *over;

	overlay_remove_all(&ctx->sheet_overlays);
	if (ctx->curr_sheet->sch->title) {
		over = overlay_add(&ctx->sheet_overlays, &ctx->aois,
		    NULL, close_subsheet, ctx);
		overlay_text(over, "<b>%s</b>", ctx->curr_sheet->sch->title);
	}
}


static void go_to_sheet(struct gui_ctx *ctx, struct gui_sheet *sheet)
{
	if (!sheet->rendered) {
		render_sheet(sheet);
		mark_aois(ctx, sheet);
	}
	ctx->curr_sheet = sheet;
	if (ctx->last_hist)
		render_delta(ctx);
	do_revision_overlays(ctx);
	do_sheet_overlays(ctx);
	zoom_to_extents(ctx);
}


static bool go_up_sheet(struct gui_ctx *ctx)
{
	struct gui_sheet *sheet;
	const struct sch_obj *obj;

	for (sheet = ctx->curr_hist->sheets; sheet; sheet = sheet->next)
		for (obj = sheet->sch->objs; obj; obj = obj->next)
			if (obj->type == sch_obj_sheet &&
			    obj->u.sheet.sheet == ctx->curr_sheet->sch) {
				go_to_sheet(ctx, sheet);
				return 1;
			}
	return 0;
}


static bool go_prev_sheet(struct gui_ctx *ctx)
{
	struct gui_sheet *sheet;

	for (sheet = ctx->curr_hist->sheets; sheet; sheet = sheet->next)
		if (sheet->next && sheet->next == ctx->curr_sheet) {
			go_to_sheet(ctx, sheet);
			return 1;
		}
	return 0;
}


static bool go_next_sheet(struct gui_ctx *ctx)
{
	if (!ctx->curr_sheet->next)
		return 0;
	go_to_sheet(ctx, ctx->curr_sheet->next);
	return 1;
}


/* ----- Event handlers ---------------------------------------------------- */


static gboolean motion_notify_event(GtkWidget *widget, GdkEventMotion *event,
    gpointer data)
{
	struct gui_ctx *ctx = data;
	const struct gui_sheet *curr_sheet = ctx->curr_sheet;
	int x, y;

	ctx->curr_x = event->x;
	ctx->curr_y = event->y;

	canvas_coord(ctx, event->x, event->y, &x, &y);

	aoi_hover(ctx->aois, event->x, event->y) ||
	    aoi_hover(curr_sheet->aois,
	    x + curr_sheet->xmin, y + curr_sheet->ymin);
	pan_update(ctx, event->x, event->y);

	return TRUE;
}


static gboolean button_press_event(GtkWidget *widget, GdkEventButton *event,
    gpointer data)
{
	struct gui_ctx *ctx = data;
	const struct gui_sheet *curr_sheet = ctx->curr_sheet;
	int x, y;

	canvas_coord(ctx, event->x, event->y, &x, &y);

	switch (event->button) {
	case 1:
		if (aoi_click(ctx->aois, event->x, event->y))
			break;
		aoi_click(curr_sheet->aois,
		    x + curr_sheet->xmin, y + curr_sheet->ymin);
		break;
	case 2:
		pan_begin(ctx, event->x, event->y);
		break;
	case 3:
		break;
	}
	return TRUE;
}


static gboolean button_release_event(GtkWidget *widget, GdkEventButton *event,
    gpointer data)
{
	struct gui_ctx *ctx = data;
	int x, y;

	canvas_coord(ctx, event->x, event->y, &x, &y);

	switch (event->button) {
	case 1:
		break;
	case 2:
		pan_end(ctx, event->x, event->y);
		break;
	case 3:
		break;
	}
	return TRUE;
}


static gboolean key_press_event(GtkWidget *widget, GdkEventKey *event,
    gpointer data)
{
	struct gui_ctx *ctx = data;
	struct gui_sheet *sheet = ctx->curr_sheet;
	int x, y;

	canvas_coord(ctx, ctx->curr_x, ctx->curr_y, &x, &y);

	switch (event->keyval) {
	case '+':
	case '=':
		zoom_in(ctx, x, y);
		break;
	case '-':
		zoom_out(ctx, x, y);
		break;
	case '*':
		zoom_to_extents(ctx);
		break;
	case GDK_KEY_Home:
		if (sheet != ctx->curr_hist->sheets)
			go_to_sheet(ctx, ctx->curr_hist->sheets);
		break;
	case GDK_KEY_BackSpace:
	case GDK_KEY_Delete:
		go_up_sheet(ctx);
		break;
	case GDK_KEY_Page_Up:
	case GDK_KEY_KP_Page_Up:
		go_prev_sheet(ctx);
		break;
	case GDK_KEY_Page_Down:
	case GDK_KEY_KP_Page_Down:
		go_next_sheet(ctx);
		break;
	case GDK_KEY_Up:
	case GDK_KEY_KP_Up:
	case GDK_KEY_Down:
	case GDK_KEY_KP_Down:
		show_history(ctx);
		break;
	case GDK_KEY_q:
		gtk_main_quit();
	}
	return TRUE;
}


static gboolean scroll_event(GtkWidget *widget, GdkEventScroll *event,
    gpointer data)
{
	struct gui_ctx *ctx = data;
	int x, y;

	canvas_coord(ctx, event->x, event->y, &x, &y);
	switch (event->direction) {
	case GDK_SCROLL_UP:
		zoom_in(ctx, x, y);
		break;
	case GDK_SCROLL_DOWN:
		zoom_out(ctx, x, y);
		break;
	default:
		/* ignore */;
	}
	return TRUE;
}


static void size_allocate_event(GtkWidget *widget, GdkRectangle *allocation,
    gpointer data)
{
	struct gui_ctx *ctx = data;

	zoom_to_extents(ctx);
}


/* ----- AoI callbacks ----------------------------------------------------- */


struct sheet_aoi_ctx {
	struct gui_ctx *gui_ctx;
	const struct sch_obj *obj;
};


static void select_subsheet(void *user)
{
	const struct sheet_aoi_ctx *aoi_ctx = user;
	struct gui_ctx *ctx = aoi_ctx->gui_ctx;
	const struct sch_obj *obj = aoi_ctx->obj;
	struct gui_sheet *sheet;

	for (sheet = ctx->curr_hist->sheets; sheet; sheet = sheet->next)
		if (sheet->sch == obj->u.sheet.sheet) {
			go_to_sheet(ctx, sheet);
			return;
		}
	abort();
}


/* ----- Initialization ---------------------------------------------------- */


static void add_sheet_aoi(struct gui_ctx *ctx, struct gui_sheet *parent,
    const struct sch_obj *obj)
{
	struct sheet_aoi_ctx *aoi_ctx = alloc_type(struct sheet_aoi_ctx);

	aoi_ctx->gui_ctx = ctx;
	aoi_ctx->obj = obj;

	struct aoi aoi = {
		.x	= obj->x,
		.y	= obj->y,
		.w	= obj->u.sheet.w,
		.h	= obj->u.sheet.h,
		.click	= select_subsheet,
		.user	= aoi_ctx,
	};

	aoi_add(&parent->aois, &aoi);
}


static void mark_aois(struct gui_ctx *ctx, struct gui_sheet *sheet)
{
	const struct sch_obj *obj;

	sheet->aois = NULL;
	for (obj = sheet->sch->objs; obj; obj = obj->next)
		switch (obj->type) {
		case sch_obj_sheet:
			add_sheet_aoi(ctx, sheet, obj);
			break;
		default:
			break;
		}
}


static struct gui_sheet *get_sheets(struct gui_ctx *ctx,
    const struct sheet *sheets)
{
	const struct sheet *sheet;
	struct gui_sheet *gui_sheets = NULL;
	struct gui_sheet **next = &gui_sheets;
	struct gui_sheet *new;

	for (sheet = sheets; sheet; sheet = sheet->next) {
		new = alloc_type(struct gui_sheet);
		new->sch = sheet;
		new->rendered = 0;

		*next = new;
		next = &new->next;
	}
	*next = NULL;
	return gui_sheets;
}


static const struct sheet *parse_sheets(struct hist *h,
    int n_args, char **args, bool recurse)
{
	char *rev = NULL;
	struct sch_ctx sch_ctx;
	struct lib lib;
	struct file sch_file;
	int i;
	bool ok;

	if (h)
		rev = vcs_git_get_rev(h);

	sch_init(&sch_ctx, recurse);
	ok = file_open_revision(&sch_file, rev, args[n_args - 1], NULL);

	if (rev)
		free(rev);
	if (!ok) {
		sch_free(&sch_ctx);
		return NULL;
	}

	lib_init(&lib);
	for (i = 0; i != n_args - 1; i++)
		if (!lib_parse(&lib, args[i], &sch_file))
			goto fail;
	if (!sch_parse(&sch_ctx, &sch_file, &lib))
		goto fail;
	file_close(&sch_file);

	/*
	 * @@@ we have a major memory leak for the component library.
	 * We should record parsed schematics and libraries separately, so
	 * that we can clean them up, without having to rely on the history,
	 * with - in the future, when sharing of unchanged item is
	 * implemented - possibly many duplicate pointers.
	 */
	return sch_ctx.sheets;

fail:
	sch_free(&sch_ctx);
	lib_free(&lib);
	file_close(&sch_file);
	return NULL;
}


struct add_hist_ctx {
	struct gui_ctx *ctx;
	int n_args;
	char **args;
	bool recurse;
unsigned limit;
};


static void add_hist(void *user, struct hist *h)
{
	struct add_hist_ctx *ahc = user;
	struct gui_ctx *ctx = ahc->ctx;
	struct gui_hist **anchor;
	const struct sheet *sch;

if (!ahc->limit)
	return;
ahc->limit--;
	for (anchor = &ctx->hist; *anchor; anchor = &(*anchor)->next);
	*anchor = alloc_type(struct gui_hist);
	(*anchor)->ctx = ctx;
	(*anchor)->hist = h;
	sch = parse_sheets(h, ahc->n_args, ahc->args, ahc->recurse);
	(*anchor)->sheets = sch ? get_sheets(ctx, sch) : NULL;
	(*anchor)->next = NULL;
}


static void get_revisions(struct gui_ctx *ctx,
    int n_args, char **args, bool recurse)
{
	const char *sch_name = args[n_args - 1];
	struct add_hist_ctx add_hist_ctx = {
		.ctx		= ctx,
		.n_args		= n_args,
		.args		= args,
		.recurse	= recurse,
.limit = 30,
	};

	if (!vcs_git_try(sch_name)) {
		ctx->vcs_hist = NULL;
		add_hist(&add_hist_ctx, NULL);
	} else {
		ctx->vcs_hist = vcs_git_hist(sch_name);
		hist_iterate(ctx->vcs_hist, add_hist, &add_hist_ctx);
	}
}


int gui(unsigned n_args, char **args, bool recurse)
{
	GtkWidget *window;
	struct gui_ctx ctx = {
		.zoom		= 4,	/* scale by 1 / 16 */
		.panning	= 0,
		.hist		= NULL,
		.vcs_hist	= NULL,
		.sheet_overlays	= NULL,
		.hist_overlays	= NULL,
		.aois		= NULL,
		.last_hist	= NULL,
	};

	get_revisions(&ctx, n_args, args, recurse);
	for (ctx.curr_hist = ctx.hist; ctx.curr_hist && !ctx.curr_hist->sheets;
	    ctx.curr_hist = ctx.curr_hist->next);
	if (!ctx.curr_hist) {
		fprintf(stderr, "no valid sheets\n");
		return 1;
	}

	setup_styles();

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);

	ctx.da = gtk_drawing_area_new();
	gtk_container_add(GTK_CONTAINER(window), ctx.da);

	g_signal_connect(G_OBJECT(ctx.da), "draw",
	    G_CALLBACK(on_draw_event), &ctx);
	g_signal_connect(G_OBJECT(ctx.da), "motion_notify_event",
	    G_CALLBACK(motion_notify_event), &ctx);
	g_signal_connect(G_OBJECT(ctx.da), "button_press_event",
	    G_CALLBACK(button_press_event), &ctx);
	g_signal_connect(G_OBJECT(ctx.da), "button_release_event",
	    G_CALLBACK(button_release_event), &ctx);
	g_signal_connect(G_OBJECT(ctx.da), "scroll_event",
	    G_CALLBACK(scroll_event), &ctx);
	g_signal_connect(G_OBJECT(ctx.da), "key_press_event",
	    G_CALLBACK(key_press_event), &ctx);
	g_signal_connect(G_OBJECT(ctx.da), "size_allocate",
	    G_CALLBACK(size_allocate_event), &ctx);

	g_signal_connect(window, "destroy",
	    G_CALLBACK(gtk_main_quit), NULL);

	gtk_widget_set_can_focus(ctx.da, TRUE);

	gtk_widget_set_events(ctx.da,
	    GDK_EXPOSE | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK |
	    GDK_KEY_PRESS_MASK |
	    GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
	    GDK_SCROLL_MASK |
	    GDK_POINTER_MOTION_MASK);

//	gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
	gtk_window_set_default_size(GTK_WINDOW(window), 640, 480);
	gtk_window_set_title(GTK_WINDOW(window), "eeshow");

	go_to_sheet(&ctx, ctx.curr_hist->sheets);
	gtk_widget_show_all(window);

	gtk_main();

	return 0;
}
