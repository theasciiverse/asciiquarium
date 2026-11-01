/*
 * Asciiquarium - An aquarium animation in ASCII art
 *
 * A C port of asciiquarium 1.1 by Kirk Baucom, for the Asciiverse. The
 * original is a Perl program built on the Term::Animation module (also
 * by Kirk Baucom); this file carries both over: first a small engine
 * that behaves like Term::Animation 2.6, then the aquarium itself,
 * following the original sub by sub. The ASCII art, most of it by Joan
 * Stark, is taken from the original unchanged by tools/extract-art.pl
 * into art.h.
 *
 * Original: Copyright (C) 2013 Kirk Baucom (kbaucom@schizoid.com)
 *   http://robobunny.com/projects/asciiquarium
 * C port:   Copyright (C) 2026 Asciiverse
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define _XOPEN_SOURCE 700
#include <curses.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "art.h"

#define VERSION "1.1"

/* ------------------------------------------------------------------ */
/* Small helpers that behave like the Perl the original relies on      */
/* ------------------------------------------------------------------ */

static void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p) { endwin(); fputs("asciiquarium: out of memory\n", stderr); exit(1); }
	return p;
}

static void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p) { endwin(); fputs("asciiquarium: out of memory\n", stderr); exit(1); }
	return p;
}

static char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	return memcpy(xmalloc(n), s, n);
}

/* Perl's rand(n): a number in [0, n); rand(0) is rand(1) */
static double prand(double n)
{
	if (n == 0) n = 1;
	return drand48() * n;
}

/* Perl's int(rand(n)) */
static int irand(double n)
{
	return (int)prand(n);
}

/* A growable string */
typedef struct { char *s; size_t len, cap; } Str;

static void str_add(Str *b, const char *s, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		b->cap = (b->len + n + 1) * 2;
		b->s = xrealloc(b->s, b->cap);
	}
	memcpy(b->s + b->len, s, n);
	b->len += n;
	b->s[b->len] = '\0';
}

static void str_cat(Str *b, const char *s) { str_add(b, s, strlen(s)); }

static char *str_take(Str *b)
{
	if (!b->s) return xstrdup("");
	return b->s;
}

/* Perl's split("\n", $s): the fields between newlines, with trailing
 * empty fields removed (a leading empty field is kept) */
typedef struct { int n; char **f; } Fields;

static Fields split_lines(const char *s)
{
	Fields r = { 0, NULL };
	const char *p = s;
	for (;;) {
		const char *e = strchr(p, '\n');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		if (!e && n == 0) break;
		r.f = xrealloc(r.f, sizeof(char *) * (r.n + 1));
		r.f[r.n] = xmalloc(n + 1);
		memcpy(r.f[r.n], p, n);
		r.f[r.n][n] = '\0';
		r.n++;
		if (!e) break;
		p = e + 1;
	}
	while (r.n > 0 && r.f[r.n - 1][0] == '\0')
		free(r.f[--r.n]);
	return r;
}

static void free_fields(Fields *f)
{
	for (int i = 0; i < f->n; i++) free(f->f[i]);
	free(f->f);
	f->f = NULL; f->n = 0;
}

/* $s x $n */
static char *repeat(const char *s, int n)
{
	Str b = { 0 };
	for (int i = 0; i < n; i++) str_cat(&b, s);
	return str_take(&b);
}

/* ------------------------------------------------------------------ */
/* The engine: what Term::Animation and Term::Animation::Entity do     */
/* ------------------------------------------------------------------ */

/* the colour ids, in the order Term::Animation allocates its pairs */
static const char color_ids[] = "wrgbcmyk";
static const short curses_colors[] = {
	COLOR_WHITE, COLOR_RED, COLOR_GREEN, COLOR_BLUE,
	COLOR_CYAN, COLOR_MAGENTA, COLOR_YELLOW, COLOR_BLACK
};

#define NOCOLOR 0xff
#define BOLD    0x80

/* 'w' -> pair index 0, 'W' -> 0 | BOLD; anything else -> NOCOLOR */
static unsigned char color_code(char c)
{
	const char *p;
	if (c >= 'A' && c <= 'Z') {
		p = strchr(color_ids, c - 'A' + 'a');
		return p ? (unsigned char)((p - color_ids) | BOLD) : NOCOLOR;
	}
	p = c ? strchr(color_ids, c) : NULL;
	return p ? (unsigned char)(p - color_ids) : NOCOLOR;
}

typedef struct { int n; char **line; } Frame;

enum callback { CB_NONE, CB_MOVE, CB_FISH, CB_FISHHOOK };
enum args { ARGS_UNDEF, ARGS_VEC, ARGS_PATH, ARGS_HOOKED };
enum death { DEATH_NONE, DEATH_SEAWEED, DEATH_FISH, DEATH_RANDOM,
             DEATH_SHARK, DEATH_FISHHOOK, DEATH_LEAD_DOLPHIN };
enum collision { COLL_NONE, COLL_BUBBLE, COLL_FISH };

typedef struct Entity Entity;
struct Entity {
	long id;
	const char *type;
	int nframes;
	Frame *frame;
	int height, width;
	char trans;
	unsigned char *color;         /* nframes x height x width */
	double x, y, z;
	int depth, physical;
	enum collision coll;
	enum callback cb;
	enum args args;
	double vec[4];                /* callback_args [dx, dy, dz, frame speed] */
	int has_fspeed;
	int path_idx, path_len;       /* callback_args [index, path] */
	const double (*path)[4];
	double curr_frame;
	int die_offscreen;
	time_t die_time;
	int has_die_frame, die_frame;
	enum death death;
	long buddy[2];                /* the lead dolphin's followers */
	int dying;
	Entity **cols;
	int ncols, capcols;
};

static Entity **ents;
static int nents, capents;
static long next_id = 1;
static Entity **del_queue;
static int ndel, capdel;
static int W, H;

/* What new_entity takes: the parts of Term::Animation::Entity->new that
 * asciiquarium uses. Shapes and colour masks are lists of frames. */
typedef struct {
	const char *type;
	const char **shape; int nshape;
	const char **mask; int nmask;
	int auto_trans;
	char transparent;
	double x, y, z;
	const char *default_color;
	int depth, has_depth;
	int physical;
	enum collision coll;
	enum callback cb;
	enum args args;
	double vec[4]; int has_fspeed;
	int path_idx, path_len; const double (*path)[4];
	int die_offscreen;
	time_t die_time;
	int has_die_frame, die_frame;
	enum death death;
} Spec;

/* Term::Animation::Entity::_trans_fill_string */
static char *trans_fill(const char *shape, char trans)
{
	Fields f = split_lines(shape);
	Str b = { 0 };
	for (int i = 0; i < f.n; i++) {
		char *l = f.f[i];
		size_t lead = 0;
		while (l[lead] == ' ' || l[lead] == '\t' || l[lead] == '\r' ||
		       l[lead] == '\f' || l[lead] == '\v')
			l[lead++] = trans;
		str_cat(&b, l);
		str_cat(&b, "\n");
	}
	free_fields(&f);
	return str_take(&b);
}

/* Term::Animation::Entity::_build_shape, for one frame */
static Frame build_frame(const char *s, int *height, int *width)
{
	Frame fr;
	if (s[0] == '\n') s++;
	Fields f = split_lines(s);
	fr.n = f.n;
	fr.line = f.f;
	for (int i = 0; i < fr.n; i++) {
		int len = (int)strlen(fr.line[i]);
		if (len > *width) *width = len;
	}
	if (fr.n > *height) *height = fr.n;
	return fr;
}

static unsigned char *color_at(Entity *e, int f, int i, int j)
{
	return &e->color[((size_t)f * e->height + i) * e->width + j];
}

/* Term::Animation::Entity::_build_mask */
static void build_mask(Entity *e, const Spec *sp)
{
	int mh = 0, mw = 0;
	int nmask = sp->mask ? sp->nmask : 0;
	Frame *mask = xmalloc(sizeof(Frame) * (nmask > e->nframes ? nmask : e->nframes));
	for (int f = 0; f < nmask; f++)
		mask[f] = build_frame(sp->mask[f], &mh, &mw);
	/* fewer mask frames than shape frames: repeat from the start */
	int have = nmask;
	for (int k = 1; have < e->nframes; k++) {
		if (k - 1 < have) mask[have] = mask[k - 1];
		else { mask[have].n = 0; mask[have].line = NULL; }
		have++;
	}
	unsigned char def = color_code(sp->default_color ? sp->default_color[0] : 'w');
	if (sp->default_color && strlen(sp->default_color) > 1) {
		/* a colour name: 'cyan' -> 'c', 'CYAN' -> 'C' */
		static const char *names[] = { "white", "red", "green", "blue",
		                               "cyan", "magenta", "yellow", "black" };
		for (int c = 0; c < 8; c++) {
			if (!strcasecmp(sp->default_color, names[c])) {
				int upper = sp->default_color[0] >= 'A' && sp->default_color[0] <= 'Z';
				def = (unsigned char)(c | (upper ? BOLD : 0));
			}
		}
	}
	e->color = xmalloc((size_t)e->nframes * e->height * e->width);
	for (int f = 0; f < e->nframes; f++)
		for (int i = 0; i < e->height; i++)
			for (int j = 0; j < e->width; j++) {
				char m = 0;
				if (i < mask[f].n && j < (int)strlen(mask[f].line[i]))
					m = mask[f].line[i][j];
				*color_at(e, f, i, j) = (m == 0 || m == ' ') ? def : color_code(m);
			}
	for (int f = 0; f < nmask; f++) {
		for (int i = 0; i < mask[f].n; i++) free(mask[f].line[i]);
		free(mask[f].line);
	}
	free(mask);
}

static void add_entity(Entity *e)
{
	if (nents == capents) {
		capents = capents ? capents * 2 : 64;
		ents = xrealloc(ents, sizeof(Entity *) * capents);
	}
	ents[nents++] = e;
}

/* Term::Animation->new_entity */
static Entity *new_entity(const Spec *sp)
{
	Entity *e = xmalloc(sizeof(Entity));
	memset(e, 0, sizeof(Entity));
	e->id = next_id++;
	e->type = sp->type ? sp->type : "self";
	e->trans = sp->transparent ? sp->transparent : '?';
	e->nframes = sp->nshape;
	e->frame = xmalloc(sizeof(Frame) * e->nframes);
	for (int f = 0; f < e->nframes; f++) {
		char *s = sp->auto_trans ? trans_fill(sp->shape[f], e->trans)
		                         : xstrdup(sp->shape[f]);
		e->frame[f] = build_frame(s, &e->height, &e->width);
		free(s);
	}
	e->x = sp->x; e->y = sp->y; e->z = sp->z;
	build_mask(e, sp);
	e->depth = sp->has_depth ? sp->depth : 1;
	e->physical = sp->physical;
	e->coll = sp->coll;
	e->args = sp->args;
	memcpy(e->vec, sp->vec, sizeof e->vec);
	e->has_fspeed = sp->has_fspeed;
	e->path_idx = sp->path_idx; e->path_len = sp->path_len; e->path = sp->path;
	if (sp->cb != CB_NONE) e->cb = sp->cb;
	else if (sp->args != ARGS_UNDEF) e->cb = CB_MOVE;
	else e->cb = CB_NONE;
	e->die_offscreen = sp->die_offscreen;
	e->die_time = sp->die_time;
	e->has_die_frame = sp->has_die_frame; e->die_frame = sp->die_frame;
	e->death = sp->death;
	add_entity(e);
	return e;
}

static void free_entity(Entity *e)
{
	for (int f = 0; f < e->nframes; f++) {
		for (int i = 0; i < e->frame[f].n; i++) free(e->frame[f].line[i]);
		free(e->frame[f].line);
	}
	free(e->frame);
	free(e->color);
	free(e->cols);
	free(e);
}

static Entity *entity_by_id(long id)
{
	for (int i = 0; i < nents; i++)
		if (ents[i]->id == id) return ents[i];
	return NULL;
}

static Entity *first_of_type(const char *type)
{
	for (int i = 0; i < nents; i++)
		if (!strcmp(ents[i]->type, type)) return ents[i];
	return NULL;
}

/* $entity->kill / $anim->del_entity: removed at the end of the frame */
static void kill_entity(Entity *e)
{
	if (e->dying) return;
	e->dying = 1;
	if (ndel == capdel) {
		capdel = capdel ? capdel * 2 : 16;
		del_queue = xrealloc(del_queue, sizeof(Entity *) * capdel);
	}
	del_queue[ndel++] = e;
}

static void remove_all_entities(void)
{
	for (int i = 0; i < nents; i++) free_entity(ents[i]);
	nents = 0;
	ndel = 0;
}

static void set_colors(void)
{
	start_color();
	for (int c = 0; c < 8; c++)
		init_pair((short)(c + 1), curses_colors[c], COLOR_BLACK);
	bkgdset(COLOR_PAIR(1));
}

/* Term::Animation::Entity::move_entity, the default callback */
static void move_entity(Entity *e, double *x, double *y, double *z, double *f, int *has_f)
{
	const double *a;
	*has_f = 0;
	if (e->args == ARGS_PATH) {
		a = e->path[e->path_idx];
		e->path_idx++;
		if (e->path_idx > e->path_len - 1) e->path_idx = 0;
		*f = a[3];
		*has_f = 1;
	} else {
		a = e->vec;
		if (e->has_fspeed && a[3] != 0) {
			double nf = e->curr_frame + a[3];
			*f = (nf - (long)nf) + (double)((long)nf % e->nframes);
			*has_f = 1;
		}
	}
	*x = e->x + a[0];
	*y = e->y + a[1];
	*z = e->z + a[2];
}

/* the aquarium's callbacks, collision handlers and death callbacks */
static void fish_callback(Entity *e, double *x, double *y, double *z, double *f, int *has_f);
static void fishhook_cb(Entity *e, double *x, double *y, double *z);
static void bubble_collision(Entity *e);
static void fish_collision(Entity *e);
static void run_death(Entity *e);

static void do_callbacks(void)
{
	int n = nents;      /* entities added now start moving next frame */
	time_t now = time(NULL);
	for (int i = 0; i < n; i++) {
		Entity *e = ents[i];
		if (e->die_time && e->die_time <= now) { kill_entity(e); continue; }
		if (e->has_die_frame && (e->die_frame--) <= 0) { kill_entity(e); continue; }
		if (e->die_offscreen &&
		    (e->x >= W || e->y >= H || e->x < 0 - e->width || e->y < 0 - e->height)) {
			kill_entity(e); continue;
		}
		double x = e->x, y = e->y, z = e->z, f = 0;
		int has_f = 0;
		switch (e->cb) {
		case CB_NONE: continue;
		case CB_MOVE: move_entity(e, &x, &y, &z, &f, &has_f); break;
		case CB_FISH: fish_callback(e, &x, &y, &z, &f, &has_f); break;
		case CB_FISHHOOK: fishhook_cb(e, &x, &y, &z); break;
		}
		e->x = x; e->y = y; e->z = z;
		if (has_f) e->curr_frame = f;
	}
}

static int overlaps(double a, double asize, double b, double bsize)
{
	return (a <= b && b < a + asize) || (b <= a && a < b + bsize);
}

static void add_collision(Entity *e, Entity *with)
{
	if (e->ncols == e->capcols) {
		e->capcols = e->capcols ? e->capcols * 2 : 4;
		e->cols = xrealloc(e->cols, sizeof(Entity *) * e->capcols);
	}
	e->cols[e->ncols++] = with;
}

static void find_collisions(void)
{
	Entity **phys = xmalloc(sizeof(Entity *) * (nents + 1));
	int np = 0;
	for (int i = 0; i < nents; i++) {
		Entity *e = ents[i];
		if (!e->physical) continue;
		for (int k = 0; k < np; k++) {
			Entity *o = phys[k];
			if (overlaps(o->x, o->width, e->x, e->width) &&
			    overlaps(o->y, o->height, e->y, e->height) &&
			    overlaps(o->z, o->depth, e->z, e->depth)) {
				add_collision(e, o);
				add_collision(o, e);
			}
		}
		phys[np++] = e;
	}
	free(phys);
}

static void collision_handlers(void)
{
	for (int i = 0; i < nents; i++) {
		Entity *e = ents[i];
		if (e->ncols > 0) {
			if (e->coll == COLL_BUBBLE) bubble_collision(e);
			else if (e->coll == COLL_FISH) fish_collision(e);
		}
	}
	for (int i = 0; i < nents; i++) ents[i]->ncols = 0;
}

static void remove_deleted_entities(void)
{
	for (int q = 0; q < ndel; q++) {   /* death callbacks may queue more */
		Entity *e = del_queue[q];
		run_death(e);
		for (int i = 0; i < nents; i++)
			if (ents[i] == e) {
				memmove(&ents[i], &ents[i + 1], sizeof(Entity *) * (nents - i - 1));
				nents--;
				break;
			}
		/* drop it from anyone's collision list */
		for (int i = 0; i < nents; i++)
			for (int c = 0; c < ents[i]->ncols; c++)
				if (ents[i]->cols[c] == e) ents[i]->cols[c] = NULL;
		free_entity(e);
	}
	ndel = 0;
}

static int by_depth(const void *a, const void *b)
{
	const Entity *x = *(Entity *const *)a, *y = *(Entity *const *)b;
	if (x->z != y->z) return x->z < y->z ? 1 : -1;   /* far ones first */
	return x->id < y->id ? -1 : (x->id > y->id);
}

static void draw_entity(Entity *e)
{
	int f = (int)e->curr_frame;
	if (f < 0 || f >= e->nframes) f = 0;
	Frame *fr = &e->frame[f];
	for (int i = 0; i < fr->n; i++) {
		double ypos = e->y + i;
		const char *l = fr->line[i];
		for (int j = 0; l[j]; j++) {
			if (l[j] == e->trans) continue;
			double xpos = e->x + j;
			if (xpos >= W || ypos >= H) continue;
			if (xpos < 0 || ypos < 0) continue;
			unsigned char c = *color_at(e, f, i, j);
			attr_t attr = c == NOCOLOR ? 0 :
				(COLOR_PAIR((c & 7) + 1) | ((c & BOLD) ? A_BOLD : 0));
			attron(attr);
			mvaddch((int)ypos, (int)xpos, (unsigned char)l[j]);
			attroff(attr);
		}
	}
}

static void build_screen(void)
{
	erase();
	if (!nents) return;
	Entity **order = xmalloc(sizeof(Entity *) * nents);
	memcpy(order, ents, sizeof(Entity *) * nents);
	qsort(order, nents, sizeof(Entity *), by_depth);
	for (int i = 0; i < nents; i++) draw_entity(order[i]);
	free(order);
}

static void display_screen(void)
{
	move(H - 1, W - 1);
	refresh();
}

/* Term::Animation->animate: one animation cycle */
static void animate(void)
{
	do_callbacks();
	int physical = 0;
	for (int i = 0; i < nents; i++) physical |= ents[i]->physical;
	if (physical) {
		find_collisions();
		collision_handlers();
	}
	remove_deleted_entities();
	build_screen();
	display_screen();
}

static void redraw_screen(void)
{
	clear();
	refresh();
	build_screen();
	move(H - 1, W - 1);
	refresh();
}

static void update_term_size(void)
{
	endwin();
	refresh();
	getmaxyx(stdscr, H, W);
	if (W <= 0 || H <= 0) { W = 80; H = 24; }
}

/* ------------------------------------------------------------------ */
/* The aquarium                                                        */
/* ------------------------------------------------------------------ */

/* the Z depth at which certain items occur */
enum {
	D_SHARK = 2, D_FISH_START = 3, D_FISH_END = 20, D_SEAWEED = 21, D_CASTLE = 22,
	D_WATER_LINE3 = 2, D_WATER_GAP3 = 3, D_WATER_LINE2 = 4, D_WATER_GAP2 = 5,
	D_WATER_LINE1 = 6, D_WATER_GAP1 = 7, D_WATER_LINE0 = 8, D_WATER_GAP0 = 9,
};
static const int water_line_depth[4] = { D_WATER_LINE0, D_WATER_LINE1, D_WATER_LINE2, D_WATER_LINE3 };

static void random_object(void);

static void add_environment(void)
{
	const char *segment[4] = { water_line_segment_0, water_line_segment_1,
	                           water_line_segment_2, water_line_segment_3 };
	/* tile the segments so they stretch across the screen */
	int segment_size = (int)strlen(segment[0]);
	int segment_repeat = W / segment_size + 1;
	for (int i = 0; i < 4; i++) {
		char *line = repeat(segment[i], segment_repeat);
		const char *shape[1] = { line };
		Spec sp = { 0 };
		sp.type = "waterline";
		sp.shape = shape; sp.nshape = 1;
		sp.x = 0; sp.y = i + 5; sp.z = water_line_depth[i];
		sp.default_color = "cyan";
		sp.depth = 22; sp.has_depth = 1;
		sp.physical = 1;
		new_entity(&sp);
		free(line);
	}
}

static void add_castle(void)
{
	const char *shape[1] = { castle_image };
	const char *mask[1] = { castle_mask };
	Spec sp = { 0 };
	sp.shape = shape; sp.nshape = 1;
	sp.mask = mask; sp.nmask = 1;
	sp.x = W - 32; sp.y = H - 13; sp.z = D_CASTLE;
	sp.default_color = "BLACK";
	new_entity(&sp);
}

static void add_seaweed(void)
{
	Str image[2] = { { 0 }, { 0 } };
	int height = irand(4) + 3;
	for (int i = 1; i <= height; i++) {
		int left_side = i % 2;
		int right_side = !left_side;
		str_cat(&image[left_side], "(\n");
		str_cat(&image[right_side], " )\n");
	}
	char *im0 = str_take(&image[0]), *im1 = str_take(&image[1]);
	const char *shape[2] = { im0, im1 };
	int x = irand(W - 2) + 1;
	int y = H - height;
	double anim_speed = prand(.05) + .25;
	Spec sp = { 0 };
	sp.shape = shape; sp.nshape = 2;
	sp.x = x; sp.y = y; sp.z = D_SEAWEED;
	sp.args = ARGS_VEC; sp.vec[3] = anim_speed; sp.has_fspeed = 1;
	/* seaweed lives for 8 to 12 minutes */
	sp.die_time = time(NULL) + irand(4 * 60) + (8 * 60);
	sp.death = DEATH_SEAWEED;
	sp.default_color = "green";
	new_entity(&sp);
	free(im0); free(im1);
}

static void add_all_seaweed(void)
{
	/* figure out how many seaweed to add by the width of the screen */
	int seaweed_count = W / 15;
	for (int i = 0; i < seaweed_count; i++) add_seaweed();
}

/* add an air bubble to a fish */
static void add_bubble(Entity *fish)
{
	double pos[3] = { fish->x, fish->y, fish->z };
	/* moving right */
	if (fish->vec[0] > 0) pos[0] += fish->width;
	pos[1] += fish->height / 2;
	/* bubble always goes on top of the fish */
	pos[2]--;
	static const char *shape[5] = { ".", "o", "O", "O", "O" };
	Spec sp = { 0 };
	sp.shape = shape; sp.nshape = 5;
	sp.type = "bubble";
	sp.x = pos[0]; sp.y = pos[1]; sp.z = pos[2];
	sp.args = ARGS_VEC; sp.vec[1] = -1; sp.vec[3] = .1; sp.has_fspeed = 1;
	sp.die_offscreen = 1;
	sp.physical = 1;
	sp.coll = COLL_BUBBLE;
	sp.default_color = "CYAN";
	new_entity(&sp);
}

static void bubble_collision(Entity *bubble)
{
	for (int c = 0; c < bubble->ncols; c++) {
		Entity *o = bubble->cols[c];
		if (o && !strcmp(o->type, "waterline")) {
			kill_entity(bubble);
			break;
		}
	}
}

/* replace the digits 1-9 in a colour mask with random colours */
static char *rand_color(const char *color_mask)
{
	static const char colors[] = { 'c','C','r','R','y','Y','b','B','g','G','m','M' };
	char *m = xstrdup(color_mask);
	for (char i = '1'; i <= '9'; i++) {
		char color = colors[irand(sizeof colors - 1)];
		for (char *p = m; *p; p++)
			if (*p == i) *p = color;
	}
	return m;
}

static void add_fish(void)
{
	static const char *fish_image[32] = {
		fish_0, fish_1, fish_2, fish_3, fish_4, fish_5, fish_6, fish_7,
		fish_8, fish_9, fish_10, fish_11, fish_12, fish_13, fish_14, fish_15,
		fish_16, fish_17, fish_18, fish_19, fish_20, fish_21, fish_22, fish_23,
		fish_24, fish_25, fish_26, fish_27, fish_28, fish_29, fish_30, fish_31,
	};
	/* 1: body  2: dorsal fin  3: flippers  4: eye  5: mouth
	 * 6: tailfin  7: gills */
	int fish_num = irand((32 - 1) / 2.0);
	int fish_index = fish_num * 2;
	double speed = prand(2) + .25;
	int depth = irand(D_FISH_END - D_FISH_START) + D_FISH_START;
	char *color_mask = xstrdup(fish_image[fish_index + 1]);
	for (char *p = color_mask; *p; p++)
		if (*p == '4') *p = 'W';
	char *mask = rand_color(color_mask);
	free(color_mask);

	if (fish_num % 2) speed *= -1;
	const char *shape[1] = { fish_image[fish_index] };
	const char *masks[1] = { mask };
	Spec sp = { 0 };
	sp.type = "fish";
	sp.shape = shape; sp.nshape = 1;
	sp.auto_trans = 1;
	sp.mask = masks; sp.nmask = 1;
	sp.x = 0; sp.y = 0; sp.z = depth;
	sp.cb = CB_FISH;
	sp.args = ARGS_VEC; sp.vec[0] = speed;
	sp.die_offscreen = 1;
	sp.death = DEATH_FISH;
	sp.physical = 1;
	sp.coll = COLL_FISH;
	Entity *fish = new_entity(&sp);
	free(mask);

	int max_height = 9;
	int min_height = H - fish->height;
	fish->y = irand(min_height - max_height) + max_height;
	if (fish_num % 2) fish->x = W - 2;
	else fish->x = 1 - fish->width;
}

static void add_all_fish(void)
{
	/* figure out how many fish to add by the size of the screen,
	 * minus the stuff above the water */
	int screen_size = (H - 9) * W;
	int fish_count = screen_size / 350;
	for (int i = 0; i < fish_count; i++) add_fish();
}

static void fish_callback(Entity *fish, double *x, double *y, double *z, double *f, int *has_f)
{
	if (irand(100) > 97) add_bubble(fish);
	move_entity(fish, x, y, z, f, has_f);
}

static void add_splat(double x, double y, double z)
{
	static const char *shape[4] = { splat_0, splat_1, splat_2, splat_3 };
	Spec sp = { 0 };
	sp.shape = shape; sp.nshape = 4;
	sp.x = x - 4; sp.y = y - 2; sp.z = z - 2;
	sp.default_color = "RED";
	sp.args = ARGS_VEC; sp.vec[3] = .25; sp.has_fspeed = 1;
	sp.transparent = ' ';
	sp.has_die_frame = 1; sp.die_frame = 15;
	new_entity(&sp);
}

/* pull the fishhook, line and whatever got caught back to the surface */
static void retract(Entity *e)
{
	if (!e) return;
	e->physical = 0;
	if (!strcmp(e->type, "fish")) {
		e->z = D_WATER_GAP2;
		e->cb = CB_FISHHOOK;
	} else {
		e->args = ARGS_HOOKED;
	}
}

static void fish_collision(Entity *fish)
{
	for (int c = 0; c < fish->ncols; c++) {
		Entity *o = fish->cols[c];
		if (!o) continue;
		if (!strcmp(o->type, "teeth")) {
			add_splat(o->x, o->y, o->z);
			kill_entity(fish);
			break;
		} else if (!strcmp(o->type, "hook_point")) {
			retract(o);
			retract(fish);
			/* get the hook and line */
			retract(first_of_type("fishhook"));
			retract(first_of_type("fishline"));
			break;
		}
	}
}

/* move the fishhook */
static void fishhook_cb(Entity *e, double *x, double *y, double *z)
{
	*x = e->x; *y = e->y; *z = e->z;
	/* this means we hooked something, reel it in */
	if (e->args != ARGS_UNDEF) {
		(*y)--;
	/* otherwise, just lower until we reach 1/4 from the bottom */
	} else if ((*y + e->height) < H * .75) {
		(*y)++;
	}
}

static void add_shark(void)
{
	static const char *shark_image[2] = { shark_image_0, shark_image_1 };
	static const char *shark_mask[2] = { shark_mask_0, shark_mask_1 };
	int dir = irand(2);
	double x = -53;
	double y = irand(H - (10 + 9)) + 9;
	double teeth_x = -9;
	double teeth_y = y + 7;
	double speed = 2;
	if (dir) {
		speed *= -1;
		x = W - 2;
		teeth_x = x + 9;
	}

	static const char *teeth[1] = { "*" };
	Spec t = { 0 };
	t.type = "teeth";
	t.shape = teeth; t.nshape = 1;
	t.x = teeth_x; t.y = teeth_y; t.z = D_SHARK + 1;
	t.depth = D_FISH_END - D_FISH_START; t.has_depth = 1;
	t.args = ARGS_VEC; t.vec[0] = speed;
	t.physical = 1;
	new_entity(&t);

	const char *shape[1] = { shark_image[dir] };
	const char *mask[1] = { shark_mask[dir] };
	Spec sp = { 0 };
	sp.type = "shark";
	sp.mask = mask; sp.nmask = 1;
	sp.shape = shape; sp.nshape = 1;
	sp.auto_trans = 1;
	sp.x = x; sp.y = y; sp.z = D_SHARK;
	sp.args = ARGS_VEC; sp.vec[0] = speed;
	sp.die_offscreen = 1;
	sp.death = DEATH_SHARK;
	/* the original gives default_color twice, WHITE then CYAN; the
	 * last one counts */
	sp.default_color = "CYAN";
	new_entity(&sp);
}

/* when a shark dies, kill the "teeth" too, the associated entity that
 * does the actual collision */
static void group_death(const char *const *bound_types, int ntypes)
{
	for (int t = 0; t < ntypes; t++)
		for (int i = 0; i < nents; i++)
			if (!strcmp(ents[i]->type, bound_types[t])) kill_entity(ents[i]);
	random_object();
}

static void add_fishhook(void)
{
	char *line_top = repeat("|\n", 50), *line_bottom = repeat(" \n", 6);
	Str line = { 0 };
	str_cat(&line, line_top);
	str_cat(&line, line_bottom);
	char *line_image = str_take(&line);
	free(line_top); free(line_bottom);

	int x = 10 + irand(W - 20);
	int y = -4;
	int point_x = x + 1;
	int point_y = y + 2;

	const char *line_shape[1] = { line_image };
	Spec l = { 0 };
	l.type = "fishline";
	l.shape = line_shape; l.nshape = 1;
	l.x = x + 7; l.y = y - 50; l.z = D_WATER_LINE1;
	l.auto_trans = 1;
	l.cb = CB_FISHHOOK;
	new_entity(&l);
	free(line_image);

	/* (the original also passes trans_char => ' ', which
	 * Term::Animation doesn't know, so it has no effect) */
	const char *hook_shape[1] = { hook_image };
	Spec h = { 0 };
	h.type = "fishhook";
	h.shape = hook_shape; h.nshape = 1;
	h.x = x; h.y = y; h.z = D_WATER_LINE1;
	h.auto_trans = 1;
	h.die_offscreen = 1;
	h.death = DEATH_FISHHOOK;
	h.default_color = "GREEN";
	h.cb = CB_FISHHOOK;
	new_entity(&h);

	const char *point_shape[1] = { point_image };
	Spec p = { 0 };
	p.type = "hook_point";
	p.shape = point_shape; p.nshape = 1;
	p.x = point_x; p.y = point_y; p.z = D_SHARK + 1;
	p.depth = D_FISH_END - D_FISH_START; p.has_depth = 1;
	p.physical = 1;
	p.default_color = "GREEN";
	p.cb = CB_FISHHOOK;
	new_entity(&p);
}

static void add_ship(void)
{
	static const char *ship_image[2] = { ship_image_0, ship_image_1 };
	static const char *ship_mask[2] = { ship_mask_0, ship_mask_1 };
	int dir = irand(2);
	double x = -24, speed = 1;
	if (dir) { speed *= -1; x = W - 2; }
	const char *shape[1] = { ship_image[dir] };
	const char *mask[1] = { ship_mask[dir] };
	Spec sp = { 0 };
	sp.mask = mask; sp.nmask = 1;
	sp.shape = shape; sp.nshape = 1;
	sp.auto_trans = 1;
	sp.x = x; sp.y = 0; sp.z = D_WATER_GAP1;
	sp.default_color = "WHITE";
	sp.args = ARGS_VEC; sp.vec[0] = speed;
	sp.die_offscreen = 1;
	sp.death = DEATH_RANDOM;
	new_entity(&sp);
}

/* join("\n" . ' ' x $align, split("\n", $frame)) */
static char *align_spout(const char *frame, int align)
{
	Fields f = split_lines(frame);
	char *pad = repeat(" ", align);
	Str b = { 0 };
	str_cat(&b, "");
	for (int i = 0; i < f.n; i++) {
		if (i) { str_cat(&b, "\n"); str_cat(&b, pad); }
		str_cat(&b, f.f[i]);
	}
	free(pad);
	free_fields(&f);
	return str_take(&b);
}

static void add_whale(void)
{
	static const char *whale_image[2] = { whale_image_0, whale_image_1 };
	static const char *whale_mask[2] = { whale_mask_0, whale_mask_1 };
	static const char *water_spout[7] = {
		water_spout_0, water_spout_1, water_spout_2, water_spout_3,
		water_spout_4, water_spout_5, water_spout_6,
	};
	int dir = irand(2);
	double x, speed = 1;
	int spout_align;
	char *whale_anim[12];
	const char *whale_anim_mask[12];
	int n = 0;

	if (dir) { spout_align = 1; speed *= -1; x = W - 2; }
	else { spout_align = 11; x = -18; }

	/* no water spout */
	for (int i = 0; i < 5; i++) {
		Str b = { 0 };
		str_cat(&b, "\n\n\n");
		str_cat(&b, whale_image[dir]);
		whale_anim[n] = str_take(&b);
		whale_anim_mask[n++] = whale_mask[dir];
	}
	/* animate water spout */
	for (int s = 0; s < 7; s++) {
		char *aligned = align_spout(water_spout[s], spout_align);
		Str b = { 0 };
		str_cat(&b, aligned);
		str_cat(&b, whale_image[dir]);
		free(aligned);
		whale_anim[n] = str_take(&b);
		whale_anim_mask[n++] = whale_mask[dir];
	}

	Spec sp = { 0 };
	sp.mask = whale_anim_mask; sp.nmask = n;
	sp.shape = (const char **)whale_anim; sp.nshape = n;
	sp.auto_trans = 1;
	sp.x = x; sp.y = 0; sp.z = D_WATER_GAP2;
	sp.default_color = "WHITE";
	sp.args = ARGS_VEC; sp.vec[0] = speed; sp.vec[3] = 1; sp.has_fspeed = 1;
	sp.die_offscreen = 1;
	sp.death = DEATH_RANDOM;
	new_entity(&sp);
	for (int i = 0; i < n; i++) free(whale_anim[i]);
}

static void add_monster(void)
{
	static const char *monster_image[2][4] = {
		{ monster_0_0, monster_0_1, monster_0_2, monster_0_3 },
		{ monster_1_0, monster_1_1, monster_1_2, monster_1_3 },
	};
	static const char *monster_mask[2] = { monster_mask_0, monster_mask_1 };
	int dir = irand(2);
	double x, speed = 2;
	if (dir) { speed *= -1; x = W - 2; }
	else x = -64;
	const char *monster_anim_mask[4];
	for (int i = 0; i < 4; i++) monster_anim_mask[i] = monster_mask[dir];

	Spec sp = { 0 };
	sp.shape = monster_image[dir]; sp.nshape = 4;
	sp.auto_trans = 1;
	sp.mask = monster_anim_mask; sp.nmask = 4;
	sp.x = x; sp.y = 2; sp.z = D_WATER_GAP2;
	sp.args = ARGS_VEC; sp.vec[0] = speed; sp.vec[3] = .25; sp.has_fspeed = 1;
	sp.death = DEATH_RANDOM;
	sp.die_offscreen = 1;
	sp.default_color = "GREEN";
	new_entity(&sp);
}

static void add_big_fish(void)
{
	static const char *big_fish_image[2] = { big_fish_image_0, big_fish_image_1 };
	static const char *big_fish_mask[2] = { big_fish_mask_0, big_fish_mask_1 };
	int dir = irand(2);
	double x, speed = 3;
	if (dir) { x = W - 1; speed *= -1; }
	else x = -34;
	int max_height = 9;
	int min_height = H - 15;
	int y = irand(min_height - max_height) + max_height;
	char *color_mask = rand_color(big_fish_mask[dir]);
	const char *shape[1] = { big_fish_image[dir] };
	const char *mask[1] = { color_mask };
	Spec sp = { 0 };
	sp.shape = shape; sp.nshape = 1;
	sp.auto_trans = 1;
	sp.mask = mask; sp.nmask = 1;
	sp.x = x; sp.y = y; sp.z = D_SHARK;
	sp.args = ARGS_VEC; sp.vec[0] = speed;
	sp.death = DEATH_RANDOM;
	sp.die_offscreen = 1;
	sp.default_color = "YELLOW";
	new_entity(&sp);
	free(color_mask);
}

static void add_ducks(void)
{
	static const char *duck_image[2][3] = {
		{ duck_0_0, duck_0_1, duck_0_2 },
		{ duck_1_0, duck_1_1, duck_1_2 },
	};
	static const char *duck_mask[2] = { duck_mask_0, duck_mask_1 };
	int dir = irand(2);
	double x, speed = 1;
	if (dir) { speed *= -1; x = W - 2; }
	else x = -30;
	const char *mask[1] = { duck_mask[dir] };
	Spec sp = { 0 };
	sp.shape = duck_image[dir]; sp.nshape = 3;
	sp.auto_trans = 1;
	sp.mask = mask; sp.nmask = 1;
	sp.x = x; sp.y = 5; sp.z = D_WATER_GAP3;
	sp.args = ARGS_VEC; sp.vec[0] = speed; sp.vec[3] = .25; sp.has_fspeed = 1;
	sp.death = DEATH_RANDOM;
	sp.die_offscreen = 1;
	sp.default_color = "WHITE";
	new_entity(&sp);
}

static double dolphin_path[2][36][4];

static void add_dolphins(void)
{
	static const char *dolphin_image[2][2] = {
		{ dolphin_0_0, dolphin_0_1 },
		{ dolphin_1_0, dolphin_1_1 },
	};
	static const char *dolphin_mask[2] = { dolphin_mask_0, dolphin_mask_1 };
	int dir = irand(2);
	double x, speed = 1, distance = 15;   /* how far apart the dolphins are */

	if (dir) {                    /* right to left */
		speed *= -1;
		distance *= -1;
		x = W - 2;
	} else {                      /* left to right */
		x = -13;
	}

	double (*path)[4] = dolphin_path[dir];
	int n = 0;
	for (int i = 0; i < 14; i++) { path[n][0] = speed; path[n][1] = -.5; path[n][2] = 0; path[n++][3] = .5; }
	for (int i = 0; i < 2; i++)  { path[n][0] = speed; path[n][1] = 0;   path[n][2] = 0; path[n++][3] = .5; }
	for (int i = 0; i < 14; i++) { path[n][0] = speed; path[n][1] = .5;  path[n][2] = 0; path[n++][3] = .5; }
	for (int i = 0; i < 6; i++)  { path[n][0] = speed; path[n][1] = 0;   path[n][2] = 0; path[n++][3] = .5; }

	const char *mask[1] = { dolphin_mask[dir] };
	Spec sp = { 0 };
	sp.shape = dolphin_image[dir]; sp.nshape = 2;
	sp.auto_trans = 1;
	sp.mask = mask; sp.nmask = 1;
	sp.z = D_WATER_GAP3;
	sp.args = ARGS_PATH; sp.path = (const double (*)[4])path; sp.path_len = n;

	Spec d3 = sp;
	d3.x = x - (distance * 2); d3.y = 8;
	d3.path_idx = 0;
	d3.death = DEATH_RANDOM;
	d3.die_offscreen = 0;
	d3.default_color = "blue";
	Entity *dolphin3 = new_entity(&d3);

	Spec d2 = sp;
	d2.x = x - distance; d2.y = 2;
	d2.path_idx = 12;
	d2.die_offscreen = 0;
	d2.default_color = "BLUE";
	Entity *dolphin2 = new_entity(&d2);

	Spec d1 = sp;
	d1.x = x; d1.y = 5;
	d1.path_idx = 24;
	/* have the lead dolphin tell the others to die offscreen, since
	 * they start offscreen */
	d1.death = DEATH_LEAD_DOLPHIN;
	d1.die_offscreen = 1;
	d1.default_color = "CYAN";
	Entity *dolphin1 = new_entity(&d1);
	dolphin1->buddy[0] = dolphin2->id;
	dolphin1->buddy[1] = dolphin3->id;
}

static void add_swan(void)
{
	static const char *swan_image[2] = { swan_image_0, swan_image_1 };
	static const char *swan_mask[2] = { swan_mask_0, swan_mask_1 };
	int dir = irand(2);
	double x, speed = 1;
	if (dir) { speed *= -1; x = W - 2; }
	else x = -10;
	const char *shape[1] = { swan_image[dir] };
	const char *mask[1] = { swan_mask[dir] };
	Spec sp = { 0 };
	sp.shape = shape; sp.nshape = 1;
	sp.auto_trans = 1;
	sp.mask = mask; sp.nmask = 1;
	sp.x = x; sp.y = 1; sp.z = D_WATER_GAP3;
	sp.args = ARGS_VEC; sp.vec[0] = speed; sp.vec[3] = .25; sp.has_fspeed = 1;
	sp.death = DEATH_RANDOM;
	sp.die_offscreen = 1;
	sp.default_color = "WHITE";
	new_entity(&sp);
}

/* add one of the random objects to the screen */
static void random_object(void)
{
	static void (*const random_objects[])(void) = {
		add_ship, add_whale, add_monster, add_big_fish, add_shark,
		add_fishhook, add_swan, add_ducks, add_dolphins,
	};
	int n = (int)(sizeof random_objects / sizeof random_objects[0]);
#ifdef AQ_TEST_OBJECT   /* testing only: always this one (0-8) */
	(void)n; random_objects[AQ_TEST_OBJECT]();
#else
	random_objects[irand(n)]();
#endif
}

static void run_death(Entity *e)
{
	static const char *const shark_types[] = { "teeth" };
	static const char *const hook_types[] = { "teeth", "fishline" };
	switch (e->death) {
	case DEATH_NONE: break;
	case DEATH_SEAWEED: add_seaweed(); break;
	case DEATH_FISH: add_fish(); break;
	case DEATH_RANDOM: random_object(); break;
	case DEATH_SHARK: group_death(shark_types, 1); break;
	case DEATH_FISHHOOK: group_death(hook_types, 2); break;
	case DEATH_LEAD_DOLPHIN:
		for (int b = 0; b < 2; b++) {
			Entity *d = entity_by_id(e->buddy[b]);
			if (d) d->die_offscreen = 1;
		}
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t quit_signal = 0;

static void sighandler(int sig)
{
	quit_signal = sig;
}

static void quit(void)
{
	endwin();
	exit(0);
}

int main(int argc, char *argv[])
{
	if (argc > 1) {
		if (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version")) {
			printf("asciiquarium %s (C port)\n", VERSION);
			return 0;
		}
		fprintf(stderr, "usage: asciiquarium\n"
		        "  q quit, p pause, r redraw\n");
		return 2;
	}

	srand48((long)time(NULL) ^ ((long)getpid() << 16));
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGHUP, sighandler);

	initscr();
	noecho();
	curs_set(0);
	keypad(stdscr, TRUE);
	getmaxyx(stdscr, H, W);
	if (W <= 0 || H <= 0) { W = 80; H = 24; }

	/* set the wait time for getch */
	halfdelay(1);
	set_colors();

	int paused = 0;
	for (;;) {
		add_environment();
		add_castle();
		add_all_seaweed();
		add_all_fish();
		random_object();

		redraw_screen();

		for (;;) {
			int in = getch();
			if (quit_signal) quit();

			if (in == 'q') quit();                          /* Exit */
			else if (in == 'r' || in == KEY_RESIZE) break;  /* Redraw (will recreate all objects) */
			else if (in == 'p') paused = !paused;

			if (!paused) animate();
		}
		update_term_size();
		remove_all_entities();
	}
}
