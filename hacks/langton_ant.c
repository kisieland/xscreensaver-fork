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

#include <stdbool.h>
#include "screenhack.h"

#ifndef HAVE_JWXYZ
# define DO_STIPPLE
#endif

#define NBITS 12

#define NANTS 5

#define steps 500


struct state {
  Display *dpy;
  Window window;

  GC gc;
  int delay;
  int xlim, ylim;
  Bool grey_p;
  Colormap cmap;

  bool **map;
  int ant_x[NANTS];
  int ant_y[NANTS];
  short ant_red[NANTS];
  short ant_green[NANTS];
  short ant_blue[NANTS];
};


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

  for (i=0; i < NANTS; i++) {
    st->ant_x[i] = random() % st->xlim;
    st->ant_y[i] = random() % st->ylim;
    st->ant_red[i] = random();
    st->ant_green[i] = random();
    st->ant_blue[i] = random();
  }

  st->map = (bool **)malloc(st->xlim * st->ylim * sizeof(bool));

  for (i=0; i < st->xlim; i++) {
    for (j=0; j < st->ylim; j++) {
      st->map[i][j] = false;
    }
  }

  return st;
}

static unsigned long
langton_ant_draw (Display *dpy, Window window, void *closure)
{
  struct state *st = (struct state *) closure;
  int n=0, i=0, j;
  XGCValues gcv;
  XColor fgc, bgc;
  XPoint points[steps];


  for (j=0; j < steps; j++) {
    points[j].x = random () % st->xlim;
    points[j].y = random () % st->ylim;
    n++;
  }

  fgc.flags = bgc.flags = DoRed|DoGreen|DoBlue;
  fgc.red = random ();
  fgc.green = random ();
  fgc.blue = random ();
  bgc.red = st->ant_red[i];
  bgc.green = st->ant_green[i];
  bgc.blue = st->ant_blue[i];

  if (st->grey_p)
  {
    fgc.green = fgc.blue = fgc.red;
    bgc.green = bgc.blue = bgc.red;
  }

  if (! XAllocColor (st->dpy, st->cmap, &fgc))
    printf("xd1\n");
  if (! XAllocColor (st->dpy, st->cmap, &bgc))
    printf("xd2\n");

  gcv.foreground = fgc.pixel;
  gcv.background = bgc.pixel;
  XChangeGC (st->dpy, st->gc, GCForeground, &gcv);
  XDrawPoints (st->dpy, st->window, st->gc, points, n, CoordModeOrigin);
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
