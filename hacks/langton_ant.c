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

#include "screenhack.h"

#ifndef HAVE_JWXYZ
# define DO_STIPPLE
#endif

#define NBITS 12

#define NANTS 5


struct state {
  Display *dpy;
  Window window;

  Pixmap pixmaps [NBITS];

  GC gc;
  int delay;
  unsigned long fg, bg, pixels [512];
  int npixels;
  int xlim, ylim;
  Bool grey_p;
  Colormap cmap;

  int ant_x[NANTS];
  int ant_y[NANTS];
};


static void *
langton_ant_init (Display *dpy, Window window)
{
  int i;
  struct state *st = (struct state *) calloc (1, sizeof(*st));
  XGCValues gcv;
  XWindowAttributes xgwa;
  st->dpy = dpy;
  st->window = window;

  XGetWindowAttributes (st->dpy, st->window, &xgwa);
  st->xlim = xgwa.width;
  st->ylim = xgwa.height;
  st->cmap = xgwa.colormap;
  st->npixels = 0;
  st->grey_p = get_boolean_resource(st->dpy, "grey", "Boolean");
  gcv.foreground= st->fg= get_pixel_resource(st->dpy, st->cmap, "foreground","Foreground");
  gcv.background= st->bg= get_pixel_resource(st->dpy, st->cmap, "background","Background");

  st->delay = get_integer_resource (st->dpy, "delay", "Integer");
  if (st->delay < 0) st->delay = 0;

  gcv.fill_style= FillOpaqueStippled;
  st->gc = XCreateGC (st->dpy, st->window, GCForeground|GCBackground|GCFillStyle, &gcv);

  for(i=0; i < NANTS; i++) {
    st->ant_x[i] = random() % st->xlim;
    st->ant_y[i] = random() % st->ylim;
  }

  return st;
}

static unsigned long
langton_ant_draw (Display *dpy, Window window, void *closure)
{
  struct state *st = (struct state *) closure;
  int x, y, w=0, h=0, i;
  XGCValues gcv;

  for (i = 0; i < 10; i++) /* minimize area, but don't try too hard */
    {
      w = 50 + random () % (st->xlim - 50);
      h = 50 + random () % (st->ylim - 50);
      if (w + h < st->xlim && w + h < st->ylim)
	break;
    }
  x = random () % (st->xlim - w);
  y = random () % (st->ylim - h);
  if (mono_p)
    {
    MONO:
      if (random () & 1)
	gcv.foreground = st->fg, gcv.background = st->bg;
      else
	gcv.foreground = st->bg, gcv.background = st->fg;
    }
  else
    {
      XColor fgc, bgc;
      if (st->npixels == sizeof (st->pixels) / sizeof (unsigned long))
	goto REUSE;
      fgc.flags = bgc.flags = DoRed|DoGreen|DoBlue;
      fgc.red = random ();
      fgc.green = random ();
      fgc.blue = random ();
      bgc.red = random ();
      bgc.green = random ();
      bgc.blue = random ();

      if (st->grey_p)
        {
          fgc.green = fgc.blue = fgc.red;
          bgc.green = bgc.blue = bgc.red;
        }

      if (! XAllocColor (st->dpy, st->cmap, &fgc))
	goto REUSE;
      st->pixels [st->npixels++] = fgc.pixel;
      gcv.foreground = fgc.pixel;
      goto DONE;
    REUSE:
      if (st->npixels <= 0)
	{
	  mono_p = 1;
	  goto MONO;
	}
      gcv.foreground = st->pixels [random () % st->npixels];
    DONE:
      ;
    }

  XChangeGC (st->dpy, st->gc, GCForeground, &gcv);
  XFillRectangle (st->dpy, st->window, st->gc, x, y, w, h);
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
  free (st);
}

XSCREENSAVER_MODULE ("Langton_Ant", langton_ant)
