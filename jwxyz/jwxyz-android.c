/* xscreensaver, Copyright (c) 2016-2017 Jamie Zawinski <jwz@jwz.org>
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation.  No representations are made about the suitability of this
 * software for any purpose.  It is provided "as is" without express or 
 * implied warranty.
 *
 * This file is three related things:
 *
 *   - It is the Android-specific C companion to jwxyz-gl.c;
 *   - It is how C calls into Java to do things that OpenGL does not
 *     have access to without Java-based APIs;
 *   - It is how the jwxyz.java class calls into C to run the hacks.
 */

#include "config.h"

#ifdef HAVE_ANDROID /* whole file */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <setjmp.h>

#include <GLES/gl.h>
#include <jni.h>
#include <android/log.h>
#include <pthread.h>

#include "screenhackI.h"
#include "jwxyzI.h"
#include "jwzglesI.h"
#include "jwxyz-android.h"
#include "textclient.h"
#include "grabscreen.h"
#include "pow2.h"


#define countof(x) (sizeof(x)/sizeof(*(x)))

extern struct xscreensaver_function_table *xscreensaver_function_table;

struct function_table_entry {
  const char *progname;
  struct xscreensaver_function_table *xsft;
  int api;
};

#include "gen/function-table.h"

struct event_queue {
  XEvent event;
  struct event_queue *next;
};

static void send_queued_events (struct running_hack *rh);

const char *progname;
const char *progclass;
int mono_p = 0;

static JavaVM *global_jvm;
static jmp_buf jmp_target;

static double current_rotation = 0;

extern void check_gl_error (const char *type);

void
jwxyz_logv(Bool error, const char *fmt, va_list args)
{
  __android_log_vprint(error ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO,
                       "xscreensaver", fmt, args);

  /* The idea here is that if the device/emulator dies shortly after a log
     message, then waiting here for a short while should increase the odds
     that adb logcat can pick up the message before everything blows up. Of
     course, doing this means dumping a ton of messages will slow things down
     significantly.
  */
# if 0
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = 25 * 1000000;
  nanosleep(&ts, NULL);
# endif
}

/* Handle an abort on Android
   TODO: Test that Android handles aborts properly
 */
void
jwxyz_abort (const char *fmt, ...)
{
  /* Send error to Android device log */
  if (!fmt || !*fmt)
    fmt = "abort";

  va_list args;
  va_start (args, fmt);
  jwxyz_logv(True, fmt, args);
  va_end (args);

  char buf[10240];
  va_start (args, fmt);
  vsprintf (buf, fmt, args);
  va_end (args);

  JNIEnv *env;
  (*global_jvm)->AttachCurrentThread (global_jvm, &env, NULL);

  if (! (*env)->ExceptionOccurred(env)) {
    // If there's already an exception queued, let's just go with that one.
    // Else, queue a Java exception to be thrown.
    (*env)->ThrowNew (env, (*env)->FindClass(env, "java/lang/RuntimeException"),
                      buf);
  }

  // Nonlocal exit out of the jwxyz code.
  longjmp (jmp_target, 1);
}


/* We get to keep live references to Java classes in use because the VM can
   unload a class that isn't being used, which invalidates field and method
   IDs.
   https://docs.oracle.com/javase/7/docs/technotes/guides/jni/spec/design.html#wp17074
*/


// #### only need one var I think
static size_t classRefCount = 0;
static jobject globalRefjwxyz, globalRefIterable, globalRefIterator,
    globalRefMapEntry;

static jfieldID runningHackField;
static jmethodID iterableIterator, iteratorHasNext, iteratorNext;
static jmethodID entryGetKey, entryGetValue;

static pthread_mutex_t mutg = PTHREAD_MUTEX_INITIALIZER;

static void screenhack_do_fps (Display *, Window, fps_state *, void *);
static char *get_string_resource_window (Window window, char *name);


/* Also creates double-buffered windows. */
static void
create_pixmap (Window win, Drawable p)
{
  // See also:
  // https://web.archive.org/web/20140213220709/http://blog.vlad1.com/2010/07/01/how-to-go-mad-while-trying-to-render-to-a-texture/
  // https://software.intel.com/en-us/articles/using-opengl-es-to-accelerate-apps-with-legacy-2d-guis
  // https://www.khronos.org/registry/egl/extensions/ANDROID/EGL_ANDROID_image_native_buffer.txt

  Assert (p->frame.width, "p->frame.width");
  Assert (p->frame.height, "p->frame.height");

  struct running_hack *rh = win->window.rh;

  if (rh->gl_fbo_p) {
    glGenTextures (1, &p->texture);
    glBindTexture (GL_TEXTURE_2D, p->texture);

    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA,
                  to_pow2(p->frame.width), to_pow2(p->frame.height),
                  0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  } else {
    EGLint attribs[5];
    attribs[0] = EGL_WIDTH;
    attribs[1] = p->frame.width;
    attribs[2] = EGL_HEIGHT;
    attribs[3] = p->frame.height;
    attribs[4] = EGL_NONE;

    p->egl_surface = eglCreatePbufferSurface(rh->egl_display, rh->egl_config,
                                           attribs);
    Assert (p->egl_surface != EGL_NO_SURFACE,
            "XCreatePixmap: got EGL_NO_SURFACE");
  }
}


static void
free_pixmap (struct running_hack *rh, Pixmap p)
{
  if (rh->gl_fbo_p) {
    glDeleteTextures (1, &p->texture);
  } else {
    eglDestroySurface(rh->egl_display, p->egl_surface);
  }
}


static void
restore_surface (struct running_hack *rh)
{
  /* This is necessary because GLSurfaceView (in Java) will call
     eglSwapBuffers under the (ordinarily reasonable) assumption that the EGL
     surface associated with the EGL context hasn't been changed.
   */
  eglMakeCurrent (rh->egl_display, rh->egl_surface, rh->egl_surface,
                  rh->egl_ctx);
  rh->current_drawable = NULL;
}


static void
get_egl_surface (struct running_hack *rh)
{
  rh->egl_surface = eglGetCurrentSurface(EGL_DRAW);
  Assert(eglGetCurrentSurface(EGL_READ) == rh->egl_surface,
         "doinit: EGL_READ != EGL_DRAW");
}


// Initialized OpenGL and runs the screenhack's init function.
//
static void
doinit (jobject jwxyz_obj, struct running_hack *rh, JNIEnv *env,
        const struct function_table_entry *chosen,
        jobject defaults, jint w, jint h)
{
  if (setjmp (jmp_target)) goto END;  // Jump here from jwxyz_abort and return.

  progname = chosen->progname;
  rh->xsft = chosen->xsft;
  rh->api = chosen->api;
  rh->jni_env = env;
  rh->jobject = jwxyz_obj;  // update this every time we call into C

  (*env)->GetJavaVM (env, &global_jvm);

# undef ya_rand_init  // This is the one and only place it is allowed
  ya_rand_init (0);

  Window wnd = (Window) calloc(1, sizeof(*wnd));
  wnd->window.rh = rh;
  wnd->frame.width = w;
  wnd->frame.height = h;
  wnd->type = WINDOW;

  rh->window = wnd;
  progclass = rh->xsft->progclass;

  if ((*env)->ExceptionOccurred(env)) abort();

  // This has to come before resource processing. It does not do graphics.
  if (rh->xsft->setup_cb)
    rh->xsft->setup_cb(rh->xsft, rh->xsft->setup_arg);

  if ((*env)->ExceptionOccurred(env)) abort();

  // Load the defaults.
  // Unceremoniously stolen from [PrefsReader defaultsToDict:].

  jclass     c = (*env)->GetObjectClass (env, defaults);
  jmethodID  m = (*env)->GetMethodID (env, c, "put",
                 "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
  if (! m) abort();
  if ((*env)->ExceptionOccurred(env)) abort();

  const struct { const char *key, *val; } default_defaults[] = {
    { "doubleBuffer", "True" },
    { "multiSample",  "False" },
    { "texFontCacheSize", "30" },
    { "textMode", "date" },
    { "textURL",
      "https://en.wikipedia.org/w/index.php?title=Special:NewPages&feed=rss" },
    { "grabDesktopImages",  "True" },
    { "chooseRandomImages", "True" },
  };

  for (int i = 0; i < countof(default_defaults); i++) {
    const char *key = default_defaults[i].key;
    const char *val = default_defaults[i].val;
    char *key2 = malloc (strlen(progname) + strlen(key) + 2);
    strcpy (key2, progname);
    strcat (key2, "_");
    strcat (key2, key);

    // defaults.put(key2, val);
    jstring jkey = (*env)->NewStringUTF (env, key2);
    jstring jval = (*env)->NewStringUTF (env, val);
    (*env)->CallObjectMethod (env, defaults, m, jkey, jval);
    (*env)->DeleteLocalRef (env, jkey);
    (*env)->DeleteLocalRef (env, jval);
    // Log ("default0: \"%s\" = \"%s\"", key2, val);
    free (key2);
  }

  const char *const *defs = rh->xsft->defaults;
  while (*defs) {
    char *line = strdup (*defs);
    char *key, *val;
    key = line;
    while (*key == '.' || *key == '*' || *key == ' ' || *key == '\t')
      key++;
    val = key;
    while (*val && *val != ':')
      val++;
    if (*val != ':') abort();
    *val++ = 0;
    while (*val == ' ' || *val == '\t')
      val++;

    unsigned long L = strlen(val);
    while (L > 0 && (val[L-1] == ' ' || val[L-1] == '\t'))
      val[--L] = 0;

    char *key2 = malloc (strlen(progname) + strlen(key) + 2);
    strcpy (key2, progname);
    strcat (key2, "_");
    strcat (key2, key);

    // defaults.put(key2, val);
    jstring jkey = (*env)->NewStringUTF (env, key2);
    jstring jval = (*env)->NewStringUTF (env, val);
    (*env)->CallObjectMethod (env, defaults, m, jkey, jval);
    (*env)->DeleteLocalRef (env, jkey);
    (*env)->DeleteLocalRef (env, jval);
    // Log ("default: \"%s\" = \"%s\"", key2, val);
    free (key2);
    free (line);
    defs++;
  }

  (*env)->DeleteLocalRef (env, c);
  if ((*env)->ExceptionOccurred(env)) abort();

  // GL init. Must come after resource processing.

  rh->egl_ctx = eglGetCurrentContext();
  Assert(rh->egl_ctx != EGL_NO_CONTEXT, "doinit: EGL_NO_CONTEXT");

  get_egl_surface (rh);

  rh->egl_display = eglGetCurrentDisplay();
  Assert(rh->egl_display != EGL_NO_DISPLAY, "doinit: EGL_NO_DISPLAY");

  unsigned egl_major, egl_minor;
  if (sscanf ((const char *)eglQueryString(rh->egl_display, EGL_VERSION),
              "%u.%u", &egl_major, &egl_minor) < 2)
  {
    egl_major = 1;
    egl_minor = 0;
  }

  EGLint config_attribs[3];
  config_attribs[0] = EGL_CONFIG_ID;
  eglQueryContext(rh->egl_display, rh->egl_ctx, EGL_CONFIG_ID,
                  &config_attribs[1]);
  config_attribs[2] = EGL_NONE;

  EGLint num_config;
  eglChooseConfig(rh->egl_display, config_attribs,
                  &rh->egl_config, 1, &num_config);
  Assert(num_config == 1, "no EGL config chosen");

  const GLubyte *extensions = glGetString (GL_EXTENSIONS);
  rh->gl_fbo_p = jwzgles_gluCheckExtension (
    (const GLubyte *)"GL_OES_framebuffer_object", extensions);

  if (rh->gl_fbo_p) {
    PFNGLGENFRAMEBUFFERSOESPROC glGenFramebuffersOES =
      (PFNGLGENFRAMEBUFFERSOESPROC)
        eglGetProcAddress ("glGenFramebuffersOES");

    rh->glBindFramebufferOES = (PFNGLBINDFRAMEBUFFEROESPROC)
        eglGetProcAddress ("glBindFramebufferOES");
    rh->glFramebufferTexture2DOES = (PFNGLFRAMEBUFFERTEXTURE2DOESPROC)
      eglGetProcAddress ("glFramebufferTexture2DOES");

    glGetIntegerv (GL_FRAMEBUFFER_BINDING_OES, &rh->fb_default);
    Assert (!rh->fb_default, "default framebuffer not current framebuffer");
    glGenFramebuffersOES (1, &rh->fb_pixmap);
    wnd->texture = 0;
  } else {
    wnd->egl_surface = rh->egl_surface;
  }

  /* TODO: Maybe ask for EGL_SWAP_BEHAVIOR_PRESERVED_BIT on the Java side of
           things via EGLConfigChooser. I (Dave) seem to automatically get
           this (n = 1), but maybe other devices won't.
   */
  rh->frontbuffer_p = False;

  if (rh->api == API_XLIB ||
      (rh->api == API_GL &&
       strcmp("True", get_string_resource_window(wnd, "doubleBuffer")))) {

    rh->frontbuffer_p = True;

# if 0 /* Might need to be 0 for Adreno...? */
    if (egl_major > 1 || (egl_major == 1 && egl_minor >= 2)) {
      EGLint surface_type;
      eglGetConfigAttrib(rh->egl_display, rh->egl_config, EGL_SURFACE_TYPE,
                         &surface_type);
      if(surface_type & EGL_SWAP_BEHAVIOR_PRESERVED_BIT) {
        eglSurfaceAttrib(rh->egl_display, rh->egl_surface, EGL_SWAP_BEHAVIOR,
                         EGL_BUFFER_PRESERVED);
        rh->frontbuffer_p = False;
      }
    }
# endif

    if (rh->frontbuffer_p) {
      /* create_pixmap needs rh->gl_fbo_p and wnd->frame. */
      create_pixmap (wnd, wnd);

      /* No preserving backbuffers, so manual blit from back to "front". */
      rh->frontbuffer.type = PIXMAP;
      rh->frontbuffer.frame = wnd->frame;
      rh->frontbuffer.pixmap.depth = visual_depth (NULL, NULL);

      if(rh->gl_fbo_p) {
        rh->frontbuffer.texture = 0;
      } else {
        Assert (wnd->egl_surface != rh->egl_surface,
                "oops: wnd->egl_surface == rh->egl_surface");
        rh->frontbuffer.egl_surface = rh->egl_surface;
      }
    }
  }

  rh->dpy = jwxyz_make_display(wnd);
  Assert(wnd == XRootWindow(rh->dpy, 0), "Wrong root window.");
  // TODO: Zero looks right, but double-check that is the right number

  /* Requires valid rh->dpy. */
  rh->copy_gc = XCreateGC (rh->dpy, &rh->frontbuffer, 0, NULL);
 
  rh->gles_state = jwzgles_make_state();
  restore_surface(rh);
 END: ;
}


#undef DEBUG_FPS

#ifdef DEBUG_FPS

static double
double_time (void)
{
  struct timeval now;
# ifdef GETTIMEOFDAY_TWO_ARGS
  struct timezone tzp;
  gettimeofday(&now, &tzp);
# else
  gettimeofday(&now);
# endif

  return (now.tv_sec + ((double) now.tv_usec * 0.000001));
}

#endif

// Animates a single frame of the current hack.
//
static jlong
drawXScreenSaver (JNIEnv *env, struct running_hack *rh)
{
# ifdef DEBUG_FPS
  double fps0=0, fps1=0, fps2=0, fps3=0, fps4=0;
  fps0 = fps1 = fps2 = fps3 = fps4 = double_time();
# endif

  unsigned long delay = 0;

  if (setjmp (jmp_target)) goto END;  // Jump here from jwxyz_abort and return.

  /* There is some kind of weird redisplay race condition between Settings
     and the launching hack: e.g., Abstractile does XClearWindow at init,
     but the screen is getting filled with random bits.  So let's wait a
     few frames before really starting up.
   */
  if (++rh->frame_count < 8) {
    /* glClearColor (1.0, 0.0, 1.0, 0.0); */
    glClear (GL_COLOR_BUFFER_BIT); /* We always need to draw *something*. */
    goto END;
  }

  prepare_context(rh);

# ifdef DEBUG_FPS
  fps1 = double_time();
# endif

  // The init function might do graphics (e.g. XClearWindow) so it has
  // to be run from inside onDrawFrame, not onSurfaceChanged.

  if (! rh->initted_p) {

    void *(*init_cb) (Display *, Window, void *) =
      (void *(*)(Display *, Window, void *)) rh->xsft->init_cb;

    unsigned int bg =
      get_pixel_resource (rh->dpy, 0, "background", "Background");
    XSetWindowBackground (rh->dpy, rh->window, bg);
    XClearWindow (rh->dpy, rh->window);

    rh->closure = init_cb (rh->dpy, rh->window, rh->xsft->setup_arg);
    rh->initted_p = True;

    /* ignore_rotation_p doesn't quite work at the moment. */
    rh->ignore_rotation_p = False;
/*
      (rh->api == API_XLIB &&
       get_boolean_resource (rh->dpy, "ignoreRotation", "IgnoreRotation"));
*/

    if (get_boolean_resource (rh->dpy, "doFPS", "DoFPS")) {
      rh->fpst = fps_init (rh->dpy, rh->window);
      if (! rh->xsft->fps_cb) rh->xsft->fps_cb = screenhack_do_fps;
    } else {
      rh->fpst = NULL;
      rh->xsft->fps_cb = 0;
    }

    if ((*env)->ExceptionOccurred(env)) abort();
  }

# ifdef DEBUG_FPS
  fps2 = double_time();
# endif

  // Apparently events don't come in on the drawing thread, and JNI flips
  // out.  So we queue them there and run them here.
  send_queued_events (rh);

# ifdef DEBUG_FPS
  fps3 = double_time();
# endif

  delay = rh->xsft->draw_cb(rh->dpy, rh->window, rh->closure);

# ifdef DEBUG_FPS
  fps4 = double_time();
# endif
  if (rh->fpst && rh->xsft->fps_cb)
    rh->xsft->fps_cb (rh->dpy, rh->window, rh->fpst, rh->closure);

  if (rh->frontbuffer_p) {
    jwxyz_copy_area (rh->dpy, rh->window, &rh->frontbuffer, rh->copy_gc,
                     0, 0, rh->window->frame.width, rh->window->frame.height,
                     0, 0);
  }

  restore_surface (rh);

 END: ;

# ifdef DEBUG_FPS
  Log("## FPS prep = %-6d init = %-6d events = %-6d draw = %-6d fps = %-6d\n",
      (int) ((fps1-fps0)*1000000),
      (int) ((fps2-fps1)*1000000),
      (int) ((fps3-fps2)*1000000),
      (int) ((fps4-fps3)*1000000),
      (int) ((double_time()-fps4)*1000000));
# endif

  return delay;
}


// Extracts the C structure that is stored in the jwxyz Java object.
static struct running_hack *
getRunningHack (JNIEnv *env, jobject thiz)
{
  jlong result = (*env)->GetLongField (env, thiz, runningHackField);
  struct running_hack *rh = (struct running_hack *)(intptr_t)result;
  if (rh)
    rh->jobject = thiz;  // update this every time we call into C
  return rh;
}

// Look up a class and mark it global in the provided variable.
static jclass
acquireClass (JNIEnv *env, const char *className, jobject *globalRef)
{
  jclass clazz = (*env)->FindClass(env, className);
  *globalRef = (*env)->NewGlobalRef(env, clazz);
  return clazz;
}


/* Note: to find signature strings for native methods:
   cd ./project/xscreensaver/build/intermediates/classes/debug/
   javap -s -p org.jwz.xscreensaver.jwxyz
 */


// Implementation of jwxyz's nativeInit Java method.
//
JNIEXPORT void JNICALL
Java_org_jwz_xscreensaver_jwxyz_nativeInit (JNIEnv *env, jobject thiz,
                                            jstring jhack, jobject defaults,
                                            jint w, jint h)
{
  pthread_mutex_lock(&mutg);

  struct running_hack *rh = calloc(1, sizeof(struct running_hack));

  if ((*env)->ExceptionOccurred(env)) abort();

  // #### simplify
  if (!classRefCount) {
    jclass classjwxyz = (*env)->GetObjectClass(env, thiz);
    globalRefjwxyz = (*env)->NewGlobalRef(env, classjwxyz);
    runningHackField = (*env)->GetFieldID
      (env, classjwxyz, "nativeRunningHackPtr", "J");
    if ((*env)->ExceptionOccurred(env)) abort();

    jclass classIterable =
      acquireClass(env, "java/lang/Iterable", &globalRefIterable);
    iterableIterator = (*env)->GetMethodID
      (env, classIterable, "iterator", "()Ljava/util/Iterator;");
    if ((*env)->ExceptionOccurred(env)) abort();

    jclass classIterator =
      acquireClass(env, "java/util/Iterator", &globalRefIterator);
    iteratorHasNext = (*env)->GetMethodID
      (env, classIterator, "hasNext", "()Z");
    iteratorNext = (*env)->GetMethodID
      (env, classIterator, "next", "()Ljava/lang/Object;");
    if ((*env)->ExceptionOccurred(env)) abort();

    jclass classMapEntry =
      acquireClass(env, "java/util/Map$Entry", &globalRefMapEntry);
    entryGetKey = (*env)->GetMethodID
      (env, classMapEntry, "getKey", "()Ljava/lang/Object;");
    entryGetValue = (*env)->GetMethodID
      (env, classMapEntry, "getValue", "()Ljava/lang/Object;");
    if ((*env)->ExceptionOccurred(env)) abort();
  }

  ++classRefCount;

  // Store the C struct into the Java object.
  (*env)->SetLongField(env, thiz, runningHackField, (jlong)(intptr_t)rh);

  // TODO: Sort the list so binary search works.
  const char *hack =(*env)->GetStringUTFChars(env, jhack, NULL);

  int chosen = 0;
  for (;;) {
    if (chosen == countof(function_table)) {
      Log ("Hack not found: %s", hack);
      abort();
    }
    if (!strcmp(function_table[chosen].progname, hack))
      break;
    chosen++;
  }

  (*env)->ReleaseStringUTFChars(env, jhack, hack);

  doinit (thiz, rh, env, &function_table[chosen], defaults, w, h);

  pthread_mutex_unlock(&mutg);
}


JNIEXPORT void JNICALL
Java_org_jwz_xscreensaver_jwxyz_nativeResize (JNIEnv *env, jobject thiz,
                                              jint w, jint h, jdouble rot)
{
  pthread_mutex_lock(&mutg);
  if (setjmp (jmp_target)) goto END;  // Jump here from jwxyz_abort and return.

  current_rotation = rot;

  Log ("native rotation: %f", current_rotation);

  struct running_hack *rh = getRunningHack(env, thiz);

  Window wnd = rh->window;
  wnd->frame.x = 0;
  wnd->frame.y = 0;
  wnd->frame.width  = w;
  wnd->frame.height = h;

  glViewport (0, 0, w, h);

  if (ignore_rotation_p(rh->dpy) &&
      rot != 0 && rot != 180 && rot != -180) {
    int swap = w;
    w = h;
    h = swap;
    wnd->frame.width  = w;
    wnd->frame.height = h;
  }

  get_egl_surface (rh);
  if (rh->frontbuffer_p) {
    free_pixmap (rh, wnd);
    create_pixmap (wnd, wnd);

    rh->frontbuffer.frame = wnd->frame;
    if (!rh->gl_fbo_p)
      rh->frontbuffer.egl_surface = rh->egl_surface;
  }

  jwxyz_window_resized (rh->dpy);
  if (rh->initted_p)
    rh->xsft->reshape_cb (rh->dpy, rh->window, rh->closure,
                          wnd->frame.width, wnd->frame.height);

  if (rh->api == API_GL) {
    glMatrixMode (GL_PROJECTION);
    glRotatef (-rot, 0, 0, 1);
    glMatrixMode (GL_MODELVIEW);
  }

  restore_surface (rh);

 END:
  pthread_mutex_unlock(&mutg);
}


JNIEXPORT jlong JNICALL
Java_org_jwz_xscreensaver_jwxyz_nativeRender (JNIEnv *env, jobject thiz)
{
  pthread_mutex_lock(&mutg);
  struct running_hack *rh = getRunningHack(env, thiz);
  jlong result = drawXScreenSaver(env, rh);
  pthread_mutex_unlock(&mutg);
  return result;
}


// TODO: Check Java side is calling this properly
JNIEXPORT void JNICALL
Java_org_jwz_xscreensaver_jwxyz_nativeDone (JNIEnv *env, jobject thiz)
{
  pthread_mutex_lock(&mutg);
  if (setjmp (jmp_target)) goto END;  // Jump here from jwxyz_abort and return.

  struct running_hack *rh = getRunningHack(env, thiz);

  prepare_context (rh);

  if (rh->initted_p)
    rh->xsft->free_cb (rh->dpy, rh->window, rh->closure);
  XFreeGC (rh->dpy, rh->copy_gc);
  jwzgles_free_state ();
  jwxyz_free_display(rh->dpy);

  free(rh);
  (*env)->SetLongField(env, thiz, runningHackField, 0);

  --classRefCount;
  if (!classRefCount) {
    (*env)->DeleteGlobalRef(env, globalRefjwxyz);
    (*env)->DeleteGlobalRef(env, globalRefIterable);
    (*env)->DeleteGlobalRef(env, globalRefIterator);
    (*env)->DeleteGlobalRef(env, globalRefMapEntry);
  }

 END:
  pthread_mutex_unlock(&mutg);
}


static int
send_event (struct running_hack *rh, XEvent *e)
{
  // Assumes mutex is locked and context is prepared

  int *xP = 0, *yP = 0;
  switch (e->xany.type) {
  case ButtonPress: case ButtonRelease:
    xP = &e->xbutton.x;
    yP = &e->xbutton.y;
    break;
  case MotionNotify:
    xP = &e->xmotion.x;
    yP = &e->xmotion.y;
    break;
  }

  // Rotate the coordinates in the events to match the pixels.
  if (xP) {
    if (ignore_rotation_p (rh->dpy)) {
      Window win = XRootWindow (rh->dpy, 0);
      int w = win->frame.width;
      int h = win->frame.height;
      int swap;
      switch ((int) current_rotation) {
      case 180: case -180:				// #### untested
        *xP = w - *xP;
        *yP = h - *yP;
        break;
      case 90: case -270:
        swap = *xP; *xP = *yP; *yP = swap;
        *yP = h - *yP;
        break;
      case -90: case 270:				// #### untested
        swap = *xP; *xP = *yP; *yP = swap;
        *xP = w - *xP;
        break;
      }
    }

    rh->window->window.last_mouse_x = *xP;
    rh->window->window.last_mouse_y = *yP;
  }

  return (rh->xsft->event_cb
          ? rh->xsft->event_cb (rh->dpy, rh->window, rh->closure, e)
          : 0);
}


static void
send_queued_events (struct running_hack *rh)
{
  struct event_queue *event, *next;
  if (! rh->event_queue) return;
  for (event = rh->event_queue, next = event->next;
       event;
       event = next, next = (event ? event->next : 0)) {
    if (! send_event (rh, &event->event)) {
      // #### flash the screen or something
    }
    free (event);
  }
  rh->event_queue = 0;
}


static void
queue_event (JNIEnv *env, jobject thiz, XEvent *e)
{
  pthread_mutex_lock (&mutg);
  struct running_hack *rh = getRunningHack (env, thiz);
  struct event_queue *q = (struct event_queue *) malloc (sizeof(*q));
  memcpy (&q->event, e, sizeof(*e));
  q->next = 0;

  // Put it at the end.
  struct event_queue *oq;
  for (oq = rh->event_queue; oq && oq->next; oq = oq->next)
    ;
  if (oq)
    oq->next = q;
  else
    rh->event_queue = q;

  pthread_mutex_unlock (&mutg);
}


JNIEXPORT void JNICALL
Java_org_jwz_xscreensaver_jwxyz_sendButtonEvent (JNIEnv *env, jobject thiz,
                                                 int x, int y, jboolean down)
{
  XEvent e;
  memset (&e, 0, sizeof(e));
  e.xany.type = (down ? ButtonPress : ButtonRelease);
  e.xbutton.button = Button1;
  e.xbutton.x = x;
  e.xbutton.y = y;
  queue_event (env, thiz, &e);
}

JNIEXPORT void JNICALL
Java_org_jwz_xscreensaver_jwxyz_sendMotionEvent (JNIEnv *env, jobject thiz,
                                                 int x, int y)
{
  XEvent e;
  memset (&e, 0, sizeof(e));
  e.xany.type = MotionNotify;
  e.xmotion.x = x;
  e.xmotion.y = y;
  queue_event (env, thiz, &e);
}

JNIEXPORT void JNICALL
Java_org_jwz_xscreensaver_jwxyz_sendKeyEvent (JNIEnv *env, jobject thiz,
                                              jboolean down_p, 
                                              int code, int mods)
{
  XEvent e;
  memset (&e, 0, sizeof(e));
  e.xkey.keycode = code;
  e.xkey.state = code;
  e.xany.type = (down_p ? KeyPress : KeyRelease);
  queue_event (env, thiz, &e);
  e.xany.type = KeyRelease;
  queue_event (env, thiz, &e);
}



/***************************************************************************
  Backend functions for jwxyz-gl.c
 */

void
prepare_context (struct running_hack *rh)
{
  /* Don't set matrices here; set them when an Xlib call triggers
     jwxyz_bind_drawable/jwxyz_set_matrices.
   */
  rh->current_drawable = NULL;
  jwzgles_make_current (rh->gles_state);
}

static void
finish_bind_drawable (Display *dpy, Drawable dst)
{
  jwxyz_assert_gl ();

  glViewport (0, 0, dst->frame.width, dst->frame.height);
  jwxyz_set_matrices (dpy, dst->frame.width, dst->frame.height, False);
}


static void
bind_drawable_fbo (struct running_hack *rh, Drawable d)
{
  rh->glBindFramebufferOES (GL_FRAMEBUFFER_OES,
                            d->texture ? rh->fb_pixmap : rh->fb_default);
  if (d->texture) {
    rh->glFramebufferTexture2DOES (GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES,
                                   GL_TEXTURE_2D, d->texture, 0);
  }
}


void
jwxyz_bind_drawable (Display *dpy, Window w, Drawable d)
{
  struct running_hack *rh = w->window.rh;
  JNIEnv *env = w->window.rh->jni_env;
  if ((*env)->ExceptionOccurred(env)) abort();
  if (rh->current_drawable != d) {
    if (rh->gl_fbo_p) {
      bind_drawable_fbo (rh, d);
    } else {
      eglMakeCurrent (rh->egl_display, d->egl_surface, d->egl_surface, rh->egl_ctx);
    }
    finish_bind_drawable (dpy, d);
    rh->current_drawable = d;
  }
}


const XRectangle *
jwxyz_frame (Drawable d)
{
  return &d->frame;
}


unsigned int
jwxyz_drawable_depth (Drawable d)
{
  return (d->type == WINDOW
          ? visual_depth (NULL, NULL)
          : d->pixmap.depth);
}


void
jwxyz_get_pos (Window w, XPoint *xvpos, XPoint *xp)
{
  xvpos->x = 0;
  xvpos->y = 0;

  if (xp) {
    xp->x = w->window.last_mouse_x;
    xp->y = w->window.last_mouse_y;
  }
}


static void
screenhack_do_fps (Display *dpy, Window w, fps_state *fpst, void *closure)
{
  fps_compute (fpst, 0, -1);
  fps_draw (fpst);
}


void
jwxyz_copy_area (Display *dpy, Drawable src, Drawable dst, GC gc,
                 int src_x, int src_y, unsigned int width, unsigned int height,
                 int dst_x, int dst_y)
{
  Window w = XRootWindow (dpy, 0);
  struct running_hack *rh = w->window.rh;

  if (rh->gl_fbo_p && src->texture && src != dst) {
    bind_drawable_fbo (rh, dst);
    finish_bind_drawable (dpy, dst);
    rh->current_drawable = NULL;

    glBindTexture (GL_TEXTURE_2D, src->texture);

    jwxyz_gl_draw_image (GL_TEXTURE_2D, to_pow2(src->frame.width),
                         to_pow2(src->frame.height),
                         src_x, src->frame.height - src_y - height,
                         width, height, dst_x, dst_y);
    return;
  }

#if 1
  // Kumppa: 0.24 FPS
  // Hilarious display corruption ahoy! (Note to self: it's on the emulator.)
  // TODO for Dave: Recheck behavior on the emulator with the better Pixmap support.

  rh->current_drawable = NULL;
  if (rh->gl_fbo_p)
    bind_drawable_fbo (rh, src);
  else
    eglMakeCurrent (rh->egl_display, dst->egl_surface, src->egl_surface, rh->egl_ctx);

  jwxyz_gl_copy_area_read_tex_image (dpy, src->frame.height, src_x, src_y,
                                     width, height, dst_x, dst_y);

  if (rh->gl_fbo_p)
    bind_drawable_fbo (rh, dst);
  finish_bind_drawable (dpy, dst);

  jwxyz_gl_copy_area_write_tex_image (dpy, gc, src_x, src_y, width, height,
                                      dst_x, dst_y);

#else
  // Kumppa: 0.17 FPS
  jwxyz_gl_copy_area_read_pixels (dpy, src, dst, gc, src_x, src_y,
                                  width, height, dst_x, dst_y);
#endif
  jwxyz_assert_gl ();
}


void
jwxyz_assert_drawable (Window main_window, Drawable d)
{
  check_gl_error("jwxyz_assert_drawable");
}


void
jwxyz_assert_gl (void)
{
  check_gl_error("jwxyz_assert_gl");
}


Pixmap
XCreatePixmap (Display *dpy, Drawable d,
               unsigned int width, unsigned int height, unsigned int depth)
{
  Window win = XRootWindow(dpy, 0);

  Pixmap p = malloc(sizeof(*p));
  p->type = PIXMAP;
  p->frame.x = 0;
  p->frame.y = 0;
  p->frame.width = width;
  p->frame.height = height;

  Assert(depth == 1 || depth == visual_depth(NULL, NULL),
         "XCreatePixmap: bad depth");
  p->pixmap.depth = depth;

  create_pixmap (win, p);

  /* For debugging. */
  jwxyz_bind_drawable (dpy, win, p);
  glClearColor (frand(1), frand(1), frand(1), 0);
  glClear (GL_COLOR_BUFFER_BIT);

  return p;
}


int
XFreePixmap (Display *d, Pixmap p)
{
  struct running_hack *rh = XRootWindow(d, 0)->window.rh;
  if (rh->current_drawable == p)
    rh->current_drawable = NULL;

  free_pixmap (rh, p);
  free (p);
  return 0;
}


double
current_device_rotation (void)
{
  return current_rotation;
}

Bool
ignore_rotation_p (Display *dpy)
{
  struct running_hack *rh = XRootWindow(dpy, 0)->window.rh;
  return rh->ignore_rotation_p;
}


static char *
jstring_dup (JNIEnv *env, jstring str)
{
  Assert (str, "expected jstring, not null");
  const char *cstr = (*env)->GetStringUTFChars (env, str, 0);
  size_t len = (*env)->GetStringUTFLength (env, str) + 1;
  char *result = malloc (len);
  if (result) {
    memcpy (result, cstr, len);
  }
  (*env)->ReleaseStringUTFChars (env, str, cstr);
  return result;
}


static char *
get_string_resource_window (Window window, char *name)
{
  JNIEnv *env = window->window.rh->jni_env;
  jobject obj = window->window.rh->jobject;

  if ((*env)->ExceptionOccurred(env)) abort();
  jstring jstr = (*env)->NewStringUTF (env, name);
  jclass     c = (*env)->GetObjectClass (env, obj);
  jmethodID  m = (*env)->GetMethodID (env, c, "getStringResource",
                           "(Ljava/lang/String;)Ljava/lang/String;");
  if ((*env)->ExceptionOccurred(env)) abort();

  jstring jvalue = (m
                  ? (*env)->CallObjectMethod (env, obj, m, jstr)
                  : NULL);
  (*env)->DeleteLocalRef (env, c);
  (*env)->DeleteLocalRef (env, jstr);
  char *ret = 0;
  if (jvalue)
    ret = jstring_dup (env, jvalue);

  Log("pref %s = %s", name, (ret ? ret : "(null)"));
  return ret;
}


char *
get_string_resource (Display *dpy, char *name, char *class)
{
  return get_string_resource_window (RootWindow (dpy, 0), name);
}


/* Returns the contents of the URL. */
char *
textclient_mobile_url_string (Display *dpy, const char *url)
{
  Window window = RootWindow (dpy, 0);
  JNIEnv *env = window->window.rh->jni_env;
  jobject obj = window->window.rh->jobject;

  jstring jstr  = (*env)->NewStringUTF (env, url);
  jclass      c = (*env)->GetObjectClass (env, obj);
  jmethodID   m = (*env)->GetMethodID (env, c, "loadURL",
                            "(Ljava/lang/String;)Ljava/nio/ByteBuffer;");
  if ((*env)->ExceptionOccurred(env)) abort();
  jobject buf = (m
                 ? (*env)->CallObjectMethod (env, obj, m, jstr)
                 : NULL);
  (*env)->DeleteLocalRef (env, c);
  (*env)->DeleteLocalRef (env, jstr);

  char *body = (char *) (buf ? (*env)->GetDirectBufferAddress (env, buf) : 0);
  char *body2;
  if (body) {
    int L = (*env)->GetDirectBufferCapacity (env, buf);
    body2 = malloc (L + 1);
    memcpy (body2, body, L);
    body2[L] = 0;
  } else {
    body2 = strdup ("ERROR");
  }

  if (buf)
    (*env)->DeleteLocalRef (env, buf);

  return body2;
}


float
jwxyz_scale (Window main_window)
{
  return 2;
}


const char *
jwxyz_default_font_family (int require)
{
  /* Font families in XLFDs are totally ignored (for now). */
  return "sans-serif";
}


void *
jwxyz_load_native_font (Window window,
                        int traits_jwxyz, int mask_jwxyz,
                        const char *font_name_ptr, size_t font_name_length,
                        int font_name_type, float size,
                        char **family_name_ret,
                        int *ascent_ret, int *descent_ret)
{
  JNIEnv *env = window->window.rh->jni_env;
  jobject obj = window->window.rh->jobject;

  jstring jname = NULL;
  if (font_name_ptr) {
    char *name_nul = malloc(font_name_length + 1);
    memcpy(name_nul, font_name_ptr, font_name_length);
    name_nul[font_name_length] = 0;
    jname = (*env)->NewStringUTF (env, name_nul);
    free(name_nul);
  }

  jclass     c = (*env)->GetObjectClass (env, obj);
  jmethodID  m = (*env)->GetMethodID (env, c, "loadFont",
                           "(IILjava/lang/String;IF)[Ljava/lang/Object;");
  if ((*env)->ExceptionOccurred(env)) abort();

  jobjectArray array = (m
                        ? (*env)->CallObjectMethod (env, obj, m, (jint)mask_jwxyz,
                                                    (jint)traits_jwxyz, jname,
                                                    (jint)font_name_type, (jfloat)size)
                        : NULL);

  (*env)->DeleteLocalRef (env, c);

  if (array) {
    jobject font = (*env)->GetObjectArrayElement (env, array, 0);
    jobject family_name =
      (jstring) ((*env)->GetObjectArrayElement (env, array, 1));
    jobject asc  = (*env)->GetObjectArrayElement (env, array, 2);
    jobject desc = (*env)->GetObjectArrayElement (env, array, 3);
    if ((*env)->ExceptionOccurred(env)) abort();

    if (family_name_ret)
      *family_name_ret = jstring_dup (env, family_name);

    jobject paint = (*env)->NewGlobalRef (env, font);
    if ((*env)->ExceptionOccurred(env)) abort();

    c = (*env)->GetObjectClass(env, asc);
    m = (*env)->GetMethodID (env, c, "floatValue", "()F");
    if ((*env)->ExceptionOccurred(env)) abort();

    *ascent_ret  = (int) (*env)->CallFloatMethod (env, asc,  m);
    *descent_ret = (int) (*env)->CallFloatMethod (env, desc, m);

    return (void *) paint;
  } else {
    return 0;
  }
}


void
jwxyz_release_native_font (Display *dpy, void *native_font)
{
  Window window = RootWindow (dpy, 0);
  JNIEnv *env = window->window.rh->jni_env;
  if ((*env)->ExceptionOccurred(env)) abort();
  (*env)->DeleteGlobalRef (env, (jobject) native_font);
  if ((*env)->ExceptionOccurred(env)) abort();
}


/* If the local reference table fills up, use this to figure out where
   you missed a call to DeleteLocalRef. */
/*
static void dump_reference_tables(JNIEnv *env)
{
  jclass c = (*env)->FindClass(env, "dalvik/system/VMDebug");
  jmethodID m = (*env)->GetStaticMethodID (env, c, "dumpReferenceTables",
                                           "()V");
  (*env)->CallStaticVoidMethod (env, c, m);
  (*env)->DeleteLocalRef (env, c);
}
*/


// Returns the metrics of the multi-character, single-line UTF8 or Latin1
// string.  If pixmap_ret is provided, also renders the text.
//
void
jwxyz_render_text (Display *dpy, void *native_font,
                   const char *str, size_t len, int utf8,
                   XCharStruct *cs, char **pixmap_ret)
{
  Window window = RootWindow (dpy, 0);
  JNIEnv *env = window->window.rh->jni_env;
  jobject obj = window->window.rh->jobject;

  char *s2;

  if (utf8) {
    s2 = malloc (len + 1);
    memcpy (s2, str, len);
    s2[len] = 0;
  } else {	// Convert Latin1 to UTF8
    s2 = malloc (len * 2 + 1);
    unsigned char *s3 = (unsigned char *) s2;
    int i;
    for (i = 0; i < len; i++) {
      unsigned char c = ((unsigned char *) str)[i];
      if (! (c & 0x80)) {
        *s3++ = c;
      } else {
        *s3++ = (0xC0 | (0x03 & (c >> 6)));
        *s3++ = (0x80 | (0x3F & c));
      }
    }
    *s3 = 0;
  }

  jstring jstr  = (*env)->NewStringUTF (env, s2);
  jclass      c = (*env)->GetObjectClass (env, obj);
  jmethodID   m = (*env)->GetMethodID (env, c, "renderText",
    "(Landroid/graphics/Paint;Ljava/lang/String;Z)Ljava/nio/ByteBuffer;");
  if ((*env)->ExceptionOccurred(env)) abort();
  jobject buf =
    (m
     ? (*env)->CallObjectMethod (env, obj, m,
                                 (jobject) native_font,
                                 jstr,
                                 (pixmap_ret ? JNI_TRUE : JNI_FALSE))
     : NULL);
  (*env)->DeleteLocalRef (env, c);
  (*env)->DeleteLocalRef (env, jstr);
  free (s2);

  if ((*env)->ExceptionOccurred(env)) abort();
  unsigned char *bits = (unsigned char *)
    (buf ? (*env)->GetDirectBufferAddress (env, buf) : 0);
  if (bits) {
    int i = 0;
    int L = (*env)->GetDirectBufferCapacity (env, buf);
    if (L < 10) abort();
    cs->lbearing = (bits[i] << 8) | (bits[i+1] & 0xFF); i += 2;
    cs->rbearing = (bits[i] << 8) | (bits[i+1] & 0xFF); i += 2;
    cs->width    = (bits[i] << 8) | (bits[i+1] & 0xFF); i += 2;
    cs->ascent   = (bits[i] << 8) | (bits[i+1] & 0xFF); i += 2;
    cs->descent  = (bits[i] << 8) | (bits[i+1] & 0xFF); i += 2;

    if (pixmap_ret) {
      char *pix = malloc (L - i);
      if (! pix) abort();
      memcpy (pix, bits + i, L - i);
      *pixmap_ret = pix;
    }
  } else {
    memset (cs, 0, sizeof(*cs));
    if (pixmap_ret)
      *pixmap_ret = 0;
  }

  if (buf)
    (*env)->DeleteLocalRef (env, buf);
}


char *
jwxyz_unicode_character_name (Display *dpy, Font fid, unsigned long uc)
{
  JNIEnv *env = XRootWindow (dpy, 0)->window.rh->jni_env;
  /* FindClass doesn't like to load classes if GetStaticMethodID fails. Huh? */
  jclass
    c = (*env)->FindClass (env, "java/lang/Character"),
    c2 = (*env)->FindClass (env, "java/lang/NoSuchMethodError");

  if ((*env)->ExceptionOccurred(env)) abort();
  jmethodID m = (*env)->GetStaticMethodID (
    env, c, "getName", "(I)Ljava/lang/String;");
  jthrowable exc = (*env)->ExceptionOccurred(env);
  if (exc) {
    if ((*env)->IsAssignableFrom(env, (*env)->GetObjectClass(env, exc), c2)) {
      (*env)->ExceptionClear (env);
      Assert (!m, "jwxyz_unicode_character_name: m?");
    } else {
      abort();
    }
  }

  char *ret = NULL;

  if (m) {
    jstring name = (*env)->CallStaticObjectMethod (env, c, m, (jint)uc);
    if (name)
     ret = jstring_dup (env, name);
  }

  if (!ret) {
    asprintf(&ret, "U+%.4lX", uc);
  }

  return ret;
}


/* Called from utils/grabclient.c */
char *
jwxyz_load_random_image (Display *dpy,
                         int *width_ret, int *height_ret,
                         char **name_ret)
{

  /* This function needs to be implemented for Android */
  return 0;

  Window window = RootWindow (dpy, 0);
  struct running_hack *rh = window->window.rh;
  JNIEnv *env = rh->jni_env;
  jobject obj = rh->jobject;

  Bool images_p =
    get_boolean_resource (rh->dpy, "chooseRandomImages", "ChooseRandomImages");
  Bool grab_p =
    get_boolean_resource (rh->dpy, "grabDesktopImages", "GrabDesktopImages");
  Bool rotate_p =
    get_boolean_resource (rh->dpy, "rotateImages", "RotateImages");

  if (!images_p && !grab_p)
    return 0;

  if (grab_p && images_p) {
    grab_p = !(random() & 5);    /* if both, screenshot 1/5th of the time */
    images_p = !grab_p;
  }

  jclass      c = (*env)->GetObjectClass (env, obj);
  jmethodID   m = (*env)->GetMethodID (env, c, 
                                       (grab_p
                                        ? "getScreenshot"
                                        : "loadRandomImage"),
                                       "(IIZ)Ljava/nio/ByteBuffer;");
  if ((*env)->ExceptionOccurred(env)) abort();
  jobject buf = (m
                 ? (*env)->CallObjectMethod (env, obj, m,
                                             window->frame.width,
                                             window->frame.height,
                                             (rotate_p ? JNI_TRUE : JNI_FALSE))
                 : NULL);
  (*env)->DeleteLocalRef (env, c);

  unsigned char *bits = (unsigned char *)
    (buf ? (*env)->GetDirectBufferAddress (env, buf) : 0);

  if (bits) {
    int i = 0;
    int L = (*env)->GetDirectBufferCapacity (env, buf);
    if (L < 100) abort();
    int width  = (bits[i] << 8) | (bits[i+1] & 0xFF); i += 2;
    int height = (bits[i] << 8) | (bits[i+1] & 0xFF); i += 2;
    char *name = (char *) bits + i;
    int L2 = strlen (name);
    i += L2 + 1;
    if (width * height * 4 != L - i) abort();
    char *pix = malloc (L - i);
    if (! pix) abort();
    memcpy (pix, bits + i, L - i);
    *width_ret  = width;
    *height_ret = height;
    *name_ret   = strdup (name);
    return (char *) pix;
  }

  if (buf)
    (*env)->DeleteLocalRef (env, buf);

  return 0;
}

#endif /* HAVE_ANDROID */
