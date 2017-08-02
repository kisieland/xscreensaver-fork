/* xscreensaver, Copyright (c) 1992-2008 Jamie Zawinski <jwz@jwz.org>
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation.  No representations are made about the suitability of this
 * software for any purpose.  It is provided "as is" without express or
 * implied warranty.
 */

#include <stdio.h>
#include "screenhack.h"

#ifndef HAVE_JWXYZ
# define DO_STIPPLE
#endif

#define NBITS 12

#define NANTS 5

#define steps 25

struct state {
  Display *dpy;
  Window window;

  GC gc;
  int delay;
  int xlim, ylim;
  Bool grey_p;
  Colormap cmap;

  short **map;
  int ant_x[NANTS];
  int ant_y[NANTS];
  short ant_direction[NANTS];
  XColor ant_color[NANTS];
  XColor background;
};

int magic_modulo_two(int a)
{
  if (a == 1)
    return 1;
  if (a == 3)
    return -1;
  return 0;
}


static void *
langton_ant_init (Display *dpy, Window window)
{
  int i, j;
  struct state *st = (struct state *) calloc (1, sizeof(*st));
  XGCValues gcv;
  XWindowAttributes xgwa;
  st->dpy = dpy;
  st->window = window;

  XGetWindowAttributes (st->dpy, st->window, &xgwa);
  st->xlim = xgwa.width;
  st->ylim = xgwa.height;
  st->cmap = xgwa.colormap;
  st->grey_p = get_boolean_resource(st->dpy, "grey", "Boolean");
  gcv.foreground = get_pixel_resource(st->dpy, st->cmap, "foreground","Foreground");
  gcv.background = get_pixel_resource(st->dpy, st->cmap, "background","Background");

  st->delay = get_integer_resource (st->dpy, "delay", "Integer");
  if (st->delay < 0) st->delay = 0;

  gcv.fill_style= FillOpaqueStippled;
  st->gc = XCreateGC (st->dpy, st->window, GCForeground|GCBackground|GCFillStyle, &gcv);

  for (i = 0; i < NANTS; i++) {
    st->ant_x[i] = random() % st->xlim;
    st->ant_y[i] = random() % st->ylim;
    st->ant_direction[i] = random() % 4;

    st->ant_color[i].flags = DoRed|DoGreen|DoBlue;
    st->ant_color[i].red = random();
    st->ant_color[i].green = random();
    st->ant_color[i].blue = random();
    if (! XAllocColor (st->dpy, st->cmap, &(st->ant_color[i])))
      printf("ERROR: should crash now.\n");
  }

  st->background.flags = DoRed|DoGreen|DoBlue;
  st->background.red = 0;
  st->background.green = 0;
  st->background.blue = 0;
  if (! XAllocColor (st->dpy, st->cmap, &(st->background)))
    printf("ERROR: should crash now.\n");

  st->map = (short **) malloc (st->xlim * sizeof(short *));
  for (i = 0; i < st->xlim; i++) {
    st->map[i] = (short *) malloc (st->ylim * sizeof(short));
    for (j = 0; j < st->ylim; j++) {
      st->map[i][j] = 0;
    }
  }

  return st;
}

static unsigned long
langton_ant_draw (Display *dpy, Window window, void *closure)
{
  struct state *st = (struct state *) closure;
  int i = 0, j, x, y;
  short direc, state, ant_direc;
  XGCValues gcv;

  for (j = 0; j < steps; j++) {
    for (i = 0; i < NANTS; i++) {
      x = st->ant_x[i];
      y = st->ant_y[i];
      ant_direc = st->ant_direction[i];
      state = st->map[x][y];
  		if (state) {
        direc = 1;
        gcv.foreground = st->background.pixel;
      } else {
        direc = -1;
        gcv.foreground = st->ant_color[i].pixel;
      }

      gcv.background = st->background.pixel;
      XChangeGC (st->dpy, st->gc, GCForeground, &gcv);
      XDrawPoint (st->dpy, st->window, st->gc, x, y);

      st->map[x][y] = (state + 1) % 2;
      st->ant_x[i] = x + magic_modulo_two(ant_direc + 1) * direc;
      st->ant_x[i] = (st->ant_x[i] + st->xlim) % st->xlim;
      st->ant_y[i] = y + magic_modulo_two(ant_direc) * direc;
      st->ant_y[i] = (st->ant_y[i] + st->ylim) % st->ylim;
      st->ant_direction[i] = (ant_direc + direc + 4) % 4;
    }
  }

  return st->delay;
}


static const char *langton_ant_defaults [] = {
  ".background:	black",
  ".foreground:	white",
  "*fpsSolid:	true",
  "*delay:	10000",
  "*grey:	false",
#ifdef HAVE_MOBILE
  "*ignoreRotation: True",
#endif
  0
};

static XrmOptionDescRec langton_ant_options [] = {
  { "-delay",		".delay",	XrmoptionSepArg, 0 },
  { "-grey",		".grey",	XrmoptionNoArg, "True" },
  { 0, 0, 0, 0 }
};

static void
langton_ant_reshape (Display *dpy, Window window, void *closure,
                 unsigned int w, unsigned int h)
{
  struct state *st = (struct state *) closure;
  st->xlim = w;
  st->ylim = h;
}

static Bool
langton_ant_event (Display *dpy, Window window, void *closure, XEvent *event)
{
  return False;
}

static void
langton_ant_free (Display *dpy, Window window, void *closure)
{
  struct state *st = (struct state *) closure;
  XFreeGC (st->dpy, st->gc);
  free (st->map);
  free (st);
}

XSCREENSAVER_MODULE ("Langton_Ant", langton_ant)
