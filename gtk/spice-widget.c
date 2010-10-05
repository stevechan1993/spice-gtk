#include "spice-widget.h"
#include "spice-common.h"

#include "vncdisplaykeymap.h"

#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>

#include <gdk/gdkx.h>

#include <spice/vd_agent.h>

#define SPICE_DISPLAY_GET_PRIVATE(obj)                                  \
    (G_TYPE_INSTANCE_GET_PRIVATE((obj), SPICE_TYPE_DISPLAY, spice_display))

struct spice_display {
    gint                    channel_id;

    /* options */
    bool                    keyboard_grab_enable;
    bool                    mouse_grab_enable;
    bool                    resize_guest_enable;
    bool                    auto_clipboard_enable;

    /* state */
    enum SpiceSurfaceFmt    format;
    gint                    width, height, stride;
    gint                    shmid;
    gpointer                data;

    gint                    ww, wh, mx, my;

    bool                    convert;
    bool                    have_mitshm;
    Display                 *dpy;
    XVisualInfo             *vi;
    XImage                  *ximage;
    XShmSegmentInfo         *shminfo;
    GC                      gc;

    GtkClipboard            *clipboard;
    bool                    clip_hasdata;
    bool                    clip_grabbed;

    SpiceSession            *session;
    SpiceChannel            *main;
    SpiceChannel            *display;
    SpiceCursorChannel      *cursor;
    SpiceInputsChannel      *inputs;

    enum SpiceMouseMode     mouse_mode;
    int                     mouse_grab_active;
    bool                    mouse_have_pointer;
    GdkCursor               *mouse_cursor;
    int                     mouse_last_x;
    int                     mouse_last_y;

    bool                    keyboard_grab_active;
    bool                    keyboard_have_focus;
    int                     keyboard_grab_count;
    time_t                  keyboard_grab_time;

    const guint16 const     *keycode_map;
    size_t                  keycode_maplen;

    gint                    timer_id;
};

G_DEFINE_TYPE(SpiceDisplay, spice_display, GTK_TYPE_DRAWING_AREA)

/* Properties */
enum {
    PROP_0,
    PROP_KEYBOARD_GRAB,
    PROP_MOUSE_GRAB,
    PROP_RESIZE_GUEST,
    PROP_AUTO_CLIPBOARD,
};

#if 0
/* Signals */
enum {
    SPICE_DISPLAY_FOO,
    SPICE_DISPLAY_LAST_SIGNAL,
};

static guint signals[SPICE_DISPLAY_LAST_SIGNAL];
#endif

static bool no_mitshm;

static void try_keyboard_grab(GtkWidget *widget);
static void try_keyboard_ungrab(GtkWidget *widget);
static void try_mouse_grab(GtkWidget *widget);
static void try_mouse_ungrab(GtkWidget *widget);
static void recalc_geometry(GtkWidget *widget);
static void clipboard_owner_change(GtkClipboard *clipboard,
                                   GdkEventOwnerChange *event, gpointer user_data);

/* ---------------------------------------------------------------- */

static struct format_table {
    enum SpiceSurfaceFmt  spice;
    XVisualInfo           xvisual;
} format_table[] = {
    {
        .spice = SPICE_SURFACE_FMT_32_xRGB,
        .xvisual = {
            .depth      = 24,
            .red_mask   = 0xff0000,
            .green_mask = 0x00ff00,
            .blue_mask  = 0x0000ff,
        },
    },{
        .spice = SPICE_SURFACE_FMT_16_555,
        .xvisual = {
            .depth      = 16,
            .red_mask   = 0x7c00,
            .green_mask = 0x03e0,
            .blue_mask  = 0x001f,
        },
    },{
        .spice = SPICE_SURFACE_FMT_16_565,
        .xvisual = {
            .depth      = 16,
            .red_mask   = 0xf800,
            .green_mask = 0x07e0,
            .blue_mask  = 0x001f,
        },
    }
};

/* ---------------------------------------------------------------- */

static void spice_display_get_property(GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
    SpiceDisplay *display = SPICE_DISPLAY(object);
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);

    switch (prop_id) {
    case PROP_KEYBOARD_GRAB:
        g_value_set_boolean(value, d->keyboard_grab_enable);
	break;
    case PROP_MOUSE_GRAB:
        g_value_set_boolean(value, d->mouse_grab_enable);
	break;
    case PROP_RESIZE_GUEST:
        g_value_set_boolean(value, d->resize_guest_enable);
	break;
    case PROP_AUTO_CLIPBOARD:
        g_value_set_boolean(value, d->auto_clipboard_enable);
	break;
    default:
	G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	break;
    }
}

static void spice_display_set_property(GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
    SpiceDisplay *display = SPICE_DISPLAY(object);
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);

    switch (prop_id) {
    case PROP_KEYBOARD_GRAB:
        d->keyboard_grab_enable = g_value_get_boolean(value);
        if (d->keyboard_grab_enable) {
            try_keyboard_grab(GTK_WIDGET(display));
        } else {
            try_keyboard_ungrab(GTK_WIDGET(display));
        }
        break;
    case PROP_MOUSE_GRAB:
        d->mouse_grab_enable = g_value_get_boolean(value);
        if (!d->mouse_grab_enable) {
            try_mouse_ungrab(GTK_WIDGET(display));
        }
        break;
    case PROP_RESIZE_GUEST:
        d->resize_guest_enable = g_value_get_boolean(value);
        if (d->resize_guest_enable) {
            gtk_widget_set_size_request(GTK_WIDGET(display), 640, 480);
            recalc_geometry(GTK_WIDGET(display));
        } else {
            gtk_widget_set_size_request(GTK_WIDGET(display),
                                        d->width, d->height);
        }
        break;
    case PROP_AUTO_CLIPBOARD:
        d->auto_clipboard_enable = g_value_get_boolean(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void spice_display_destroy(GtkObject *obj)
{
    GTK_OBJECT_CLASS(spice_display_parent_class)->destroy(obj);
}

static void spice_display_finalize(GObject *obj)
{
    G_OBJECT_CLASS(spice_display_parent_class)->finalize(obj);
}

static void spice_display_init(SpiceDisplay *display)
{
    GtkWidget *widget = GTK_WIDGET(display);
    spice_display *d;

    d = display->priv = SPICE_DISPLAY_GET_PRIVATE(display);
    memset(d, 0, sizeof(*d));

    gtk_widget_add_events(widget,
                          GDK_STRUCTURE_MASK |
                          GDK_POINTER_MOTION_MASK |
                          GDK_BUTTON_PRESS_MASK |
                          GDK_BUTTON_RELEASE_MASK |
                          GDK_BUTTON_MOTION_MASK |
                          GDK_ENTER_NOTIFY_MASK |
                          GDK_LEAVE_NOTIFY_MASK |
                          GDK_KEY_PRESS_MASK);
    gtk_widget_set_double_buffered(widget, false);
    gtk_widget_set_can_focus(widget, true);

    d->keycode_map = vnc_display_keymap_gdk2xtkbd_table(&d->keycode_maplen);

    d->clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    g_signal_connect(G_OBJECT(d->clipboard), "owner-change",
                     G_CALLBACK(clipboard_owner_change), display);

    d->have_mitshm = true;
}

static void try_keyboard_grab(GtkWidget *widget)
{
    SpiceDisplay *display = SPICE_DISPLAY(widget);
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);
    time_t now;

    if (d->keyboard_grab_active)
        return;

    if (!d->keyboard_grab_enable)
        return;
    if (!d->keyboard_have_focus)
        return;
    if (!d->mouse_have_pointer)
        return;

#if 1
    /*
     * == DEBUG ==
     * focus / keyboard grab behavior is funky
     * when going fullscreen (with KDE):
     * focus-in-event -> grab -> focus-out-event -> ungrab -> repeat
     * I have no idea why the grab triggers focus-out :-(
     */
    assert(gtk_widget_is_focus(widget));
    assert(gtk_widget_has_focus(widget));

    now = time(NULL);
    if (d->keyboard_grab_time != now) {
        d->keyboard_grab_time = now;
        d->keyboard_grab_count = 0;
    }
    if (d->keyboard_grab_count++ > 32) {
        fprintf(stderr, "%s: 32 grabs last second -> emergency exit\n",
                __FUNCTION__);
        return;
    }
#endif

#if 0
    fprintf(stderr, "grab keyboard\n");
#endif

    gdk_keyboard_grab(gtk_widget_get_window(widget), FALSE,
                      GDK_CURRENT_TIME);
    d->keyboard_grab_active = true;
}


static void try_keyboard_ungrab(GtkWidget *widget)
{
    SpiceDisplay *display = SPICE_DISPLAY(widget);
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);

    if (!d->keyboard_grab_active)
        return;

#if 0
    fprintf(stderr, "ungrab keyboard\n");
#endif
    gdk_keyboard_ungrab(GDK_CURRENT_TIME);
    d->keyboard_grab_active = false;
}

static void try_mouse_grab(GtkWidget *widget)
{
    SpiceDisplay *display = SPICE_DISPLAY(widget);
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);

    if (!d->mouse_grab_enable)
        return;
    if (d->mouse_mode != SPICE_MOUSE_MODE_SERVER)
        return;
    if (d->mouse_grab_active)
        return;

    gdk_pointer_grab(gtk_widget_get_window(widget),
                     FALSE, /* All events to come to our window directly */
                     GDK_POINTER_MOTION_MASK |
                     GDK_BUTTON_PRESS_MASK |
                     GDK_BUTTON_RELEASE_MASK |
                     GDK_BUTTON_MOTION_MASK,
                     NULL, /* Allow cursor to move over entire desktop */
                     gdk_cursor_new(GDK_BLANK_CURSOR),
                     GDK_CURRENT_TIME);
    d->mouse_grab_active = true;
    d->mouse_last_x = -1;
    d->mouse_last_y = -1;
}

static void mouse_check_edges(GtkWidget *widget, GdkEventMotion *motion)
{
    SpiceDisplay *display = SPICE_DISPLAY(widget);
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);
    GdkDrawable *drawable = GDK_DRAWABLE(gtk_widget_get_window(widget));
    GdkScreen *screen = gdk_drawable_get_screen(drawable);
    int x = (int)motion->x_root;
    int y = (int)motion->y_root;

    /* In relative mode check to see if client pointer hit
     * one of the screen edges, and if so move it back by
     * 200 pixels. This is important because the pointer
     * in the server doesn't correspond 1-for-1, and so
     * may still be only half way across the screen. Without
     * this warp, the server pointer would thus appear to hit
     * an invisible wall */
    if (x == 0) x += 200;
    if (y == 0) y += 200;
    if (x == (gdk_screen_get_width(screen) - 1)) x -= 200;
    if (y == (gdk_screen_get_height(screen) - 1)) y -= 200;

    if (x != (int)motion->x_root || y != (int)motion->y_root) {
        gdk_display_warp_pointer(gdk_drawable_get_display(drawable),
                                 screen, x, y);
        d->mouse_last_x = -1;
        d->mouse_last_y = -1;
    }
}

static void try_mouse_ungrab(GtkWidget *widget)
{
    SpiceDisplay *display = SPICE_DISPLAY(widget);
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);

    if (!d->mouse_grab_active)
        return;

    gdk_pointer_ungrab(GDK_CURRENT_TIME);
    gdk_window_set_cursor(gtk_widget_get_window(widget), NULL);
    d->mouse_grab_active = false;
}

static gboolean geometry_timer(gpointer data)
{
    SpiceDisplay *display = data;
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);

    d->timer_id = 0;
    spice_main_set_display(d->main, d->channel_id,
                           0, 0, d->ww, d->wh);
    return false;
}

static void recalc_geometry(GtkWidget *widget)
{
    SpiceDisplay *display = SPICE_DISPLAY(widget);
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);

    d->mx = 0;
    d->my = 0;
    if (d->ww > d->width)
        d->mx = (d->ww - d->width) / 2;
    if (d->wh > d->height)
        d->my = (d->wh - d->height) / 2;

#if 0
    fprintf(stderr, "%s: guest %dx%d, window %dx%d, offset +%d+%d\n", __FUNCTION__,
            d->width, d->height, d->ww, d->wh, d->mx, d->my);
#endif

    if (d->timer_id) {
        g_source_remove(d->timer_id);
    }
    if (d->resize_guest_enable) {
        d->timer_id = g_timeout_add_seconds(1, geometry_timer, display);
    }
}

static XVisualInfo *get_visual_for_format(GtkWidget *widget, enum SpiceSurfaceFmt format)
{
    GdkDrawable  *drawable = gtk_widget_get_window(widget);
    GdkDisplay   *display = gdk_drawable_get_display(drawable);
    GdkScreen    *screen = gdk_drawable_get_screen(drawable);
    XVisualInfo  template;
    int          found, i;

    for (i = 0; i < SPICE_N_ELEMENTS(format_table); i++) {
        if (format == format_table[i].spice)
            break;
    }
    if (i == SPICE_N_ELEMENTS(format_table))
        return NULL;

    template = format_table[i].xvisual;
    template.screen = gdk_x11_screen_get_screen_number(screen);
    return XGetVisualInfo(gdk_x11_display_get_xdisplay(display),
                          VisualScreenMask | VisualDepthMask |
                          VisualRedMaskMask | VisualGreenMaskMask | VisualBlueMaskMask,
                          &template, &found);
}

static XVisualInfo *get_visual_default(GtkWidget *widget)
{
    GdkDrawable  *drawable = gtk_widget_get_window(widget);
    GdkDisplay   *display = gdk_drawable_get_display(drawable);
    GdkScreen    *screen = gdk_drawable_get_screen(drawable);
    XVisualInfo  template;
    int          found;

    template.screen = gdk_x11_screen_get_screen_number(screen);
    return XGetVisualInfo(gdk_x11_display_get_xdisplay(display),
                          VisualScreenMask,
                          &template, &found);
}

static int catch_no_mitshm(Display * dpy, XErrorEvent * event)
{
    no_mitshm = true;
    return 0;
}

static int ximage_create(GtkWidget *widget)
{
    SpiceDisplay    *display = SPICE_DISPLAY(widget);
    spice_display   *d = SPICE_DISPLAY_GET_PRIVATE(display);
    GdkDrawable     *window = gtk_widget_get_window(widget);
    GdkDisplay      *gtkdpy = gdk_drawable_get_display(window);
    void            *old_handler = NULL;
    XGCValues       gcval = {
        .foreground = 0,
        .background = 0,
    };

    d->dpy = gdk_x11_display_get_xdisplay(gtkdpy);
    d->convert = false;
    d->vi = get_visual_for_format(widget, d->format);
    if (d->vi == NULL) {
        d->convert = true;
        d->vi = get_visual_default(widget);
        ASSERT(d->vi != NULL);
    }
    if (d->convert) {
        PANIC("format conversion not implemented yet");
    }

    d->gc = XCreateGC(d->dpy, gdk_x11_drawable_get_xid(window),
                      GCForeground | GCBackground, &gcval);

    if (d->have_mitshm && d->shmid != -1) {
        if (!XShmQueryExtension(d->dpy)) {
            goto shm_fail;
        }
        no_mitshm = false;
        old_handler = XSetErrorHandler(catch_no_mitshm);
        d->shminfo = spice_new0(XShmSegmentInfo, 1);
        d->ximage = XShmCreateImage(d->dpy, d->vi->visual, d->vi->depth,
                                    ZPixmap, d->data, d->shminfo, d->width, d->height);
        if (d->ximage == NULL)
            goto shm_fail;
        d->shminfo->shmaddr = d->data;
        d->shminfo->shmid = d->shmid;
        d->shminfo->readOnly = false;
        XShmAttach(d->dpy, d->shminfo);
        XSync(d->dpy, False);
        shmctl(d->shmid, IPC_RMID, 0);
        if (no_mitshm)
            goto shm_fail;
        XSetErrorHandler(old_handler);
        return 0;
    }

shm_fail:
    d->have_mitshm = false;
    if (old_handler)
        XSetErrorHandler(old_handler);
    d->ximage = XCreateImage(d->dpy, d->vi->visual, d->vi->depth, ZPixmap, 0,
                             d->data, d->width, d->height, 32, d->stride);
    return 0;
}

static void ximage_destroy(GtkWidget *widget)
{
    SpiceDisplay *display = SPICE_DISPLAY(widget);
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);

    if (d->ximage) {
        XDestroyImage(d->ximage);
        d->ximage = NULL;
    }
    if (d->shminfo) {
        XShmDetach(d->dpy, d->shminfo);
        free(d->shminfo);
        d->shminfo = NULL;
    }
    if (d->gc) {
        XFreeGC(d->dpy, d->gc);
        d->gc = NULL;
    }
}

static gboolean expose_event(GtkWidget *widget, GdkEventExpose *expose)
{
    SpiceDisplay *display = SPICE_DISPLAY(widget);
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);
    GdkDrawable *window = gtk_widget_get_window(widget);

#if 0
    fprintf(stderr, "%s: area %dx%d at %d,%d\n", __FUNCTION__,
            expose->area.width,
            expose->area.height,
            expose->area.x,
            expose->area.y);
#endif

    if (d->data == NULL)
        return true;
    if (!d->ximage) {
        ximage_create(widget);
    }

    if (expose->area.x >= d->mx &&
        expose->area.y >= d->my &&
        expose->area.x + expose->area.width  <= d->mx + d->width &&
        expose->area.y + expose->area.height <= d->my + d->height) {
        /* area is completely inside the guest screen -- blit it */
        if (d->have_mitshm) {
            XShmPutImage(d->dpy, gdk_x11_drawable_get_xid(window),
                         d->gc, d->ximage,
                         expose->area.x - d->mx, expose->area.y - d->my,
                         expose->area.x,         expose->area.y,
                         expose->area.width, expose->area.height,
                         true);
        } else {
            XPutImage(d->dpy, gdk_x11_drawable_get_xid(window),
                      d->gc, d->ximage,
                      expose->area.x - d->mx, expose->area.y - d->my,
                      expose->area.x,         expose->area.y,
                      expose->area.width, expose->area.height);
        }
    } else {
        /* complete window update */
        if (d->ww > d->width || d->wh > d->height) {
            int x1 = d->mx;
            int x2 = d->mx + d->width;
            int y1 = d->my;
            int y2 = d->my + d->height;
            XFillRectangle(d->dpy, gdk_x11_drawable_get_xid(window),
                           d->gc, 0, 0, x1, d->wh);
            XFillRectangle(d->dpy, gdk_x11_drawable_get_xid(window),
                           d->gc, x2, 0, d->ww - x2, d->wh);
            XFillRectangle(d->dpy, gdk_x11_drawable_get_xid(window),
                           d->gc, 0, 0, d->ww, y1);
            XFillRectangle(d->dpy, gdk_x11_drawable_get_xid(window),
                           d->gc, 0, y2, d->ww, d->wh - y2);
        }
        if (d->have_mitshm) {
            XShmPutImage(d->dpy, gdk_x11_drawable_get_xid(window),
                         d->gc, d->ximage,
                         0, 0, d->mx, d->my, d->width, d->height,
                         true);
        } else {
            XPutImage(d->dpy, gdk_x11_drawable_get_xid(window),
                      d->gc, d->ximage,
                      0, 0, d->mx, d->my, d->width, d->height);
        }
    }

    return true;
}

static gboolean key_event(GtkWidget *widget, GdkEventKey *key)
{
    SpiceDisplay *display = SPICE_DISPLAY(widget);
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);
    int scancode;

#if 0
    fprintf(stderr, "%s %s: keycode: %d  state: %d  group %d\n",
            __FUNCTION__, key->type == GDK_KEY_PRESS ? "press" : "release",
            key->hardware_keycode, key->state, key->group);
#endif

    if (!d->inputs)
        return true;

    scancode = vnc_display_keymap_gdk2xtkbd(d->keycode_map, d->keycode_maplen,
                                            key->hardware_keycode);
    switch (key->type) {
    case GDK_KEY_PRESS:
        spice_inputs_key_press(d->inputs, scancode);
        break;
    case GDK_KEY_RELEASE:
        spice_inputs_key_release(d->inputs, scancode);
        break;
    default:
        break;
    }
    return true;
}

static gboolean enter_event(GtkWidget *widget, GdkEventCrossing *crossing G_GNUC_UNUSED)
{
    SpiceDisplay *display = SPICE_DISPLAY(widget);
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);

#if 0
    fprintf(stderr, "%s\n", __FUNCTION__);
#endif
    d->mouse_have_pointer = true;
    try_keyboard_grab(widget);
    return true;
}

static gboolean leave_event(GtkWidget *widget, GdkEventCrossing *crossing G_GNUC_UNUSED)
{
    SpiceDisplay *display = SPICE_DISPLAY(widget);
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);

#if 0
    fprintf(stderr, "%s\n", __FUNCTION__);
#endif
    d->mouse_have_pointer = false;
    try_keyboard_ungrab(widget);
    return true;
}

static gboolean focus_in_event(GtkWidget *widget, GdkEventFocus *focus G_GNUC_UNUSED)
{
    SpiceDisplay *display = SPICE_DISPLAY(widget);
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);

#if 0
    fprintf(stderr, "%s\n", __FUNCTION__);
#endif
    d->keyboard_have_focus = true;
    try_keyboard_grab(widget);
    return true;
}

static gboolean focus_out_event(GtkWidget *widget, GdkEventFocus *focus G_GNUC_UNUSED)
{
    SpiceDisplay *display = SPICE_DISPLAY(widget);
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);

#if 0
    fprintf(stderr, "%s\n", __FUNCTION__);
#endif
    d->keyboard_have_focus = false;
    try_keyboard_ungrab(widget);
    return true;
}

static int button_gdk_to_spice(int gdk)
{
    static const int map[] = {
        [ 1 ] = SPICE_MOUSE_BUTTON_LEFT,
        [ 2 ] = SPICE_MOUSE_BUTTON_MIDDLE,
        [ 3 ] = SPICE_MOUSE_BUTTON_RIGHT,
        [ 4 ] = SPICE_MOUSE_BUTTON_UP,
        [ 5 ] = SPICE_MOUSE_BUTTON_DOWN,
    };

    if (gdk < SPICE_N_ELEMENTS(map)) {
        return map [ gdk ];
    }
    return 0;
}

static int button_mask_gdk_to_spice(int gdk)
{
    int spice = 0;

    if (gdk & GDK_BUTTON1_MASK)
        spice |= SPICE_MOUSE_BUTTON_MASK_LEFT;
    if (gdk & GDK_BUTTON2_MASK)
        spice |= SPICE_MOUSE_BUTTON_MASK_MIDDLE;
    if (gdk & GDK_BUTTON3_MASK)
        spice |= SPICE_MOUSE_BUTTON_MASK_RIGHT;
    return spice;
}

static gboolean motion_event(GtkWidget *widget, GdkEventMotion *motion)
{
    SpiceDisplay *display = SPICE_DISPLAY(widget);
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);

#if 0
    fprintf(stderr, "%s: +%.0f+%.0f\n", __FUNCTION__, motion->x, motion->y);
#endif

    if (!d->inputs)
        return true;
    switch (d->mouse_mode) {
    case SPICE_MOUSE_MODE_CLIENT:
        if (motion->x >= d->mx            &&
            motion->x <  d->mx + d->width &&
            motion->y >= d->my            &&
            motion->y <  d->my + d->height) {
            spice_inputs_position(d->inputs,
                                  motion->x - d->mx, motion->y - d->my,
                                  d->channel_id,
                                  button_mask_gdk_to_spice(motion->state));
        }
        break;
    case SPICE_MOUSE_MODE_SERVER:
        if (d->mouse_grab_active) {
            if (d->mouse_last_x != -1 &&
                d->mouse_last_y != -1) {
                spice_inputs_motion(d->inputs,
                                    motion->x - d->mouse_last_x,
                                    motion->y - d->mouse_last_y,
                                    button_mask_gdk_to_spice(motion->state));
            }
            d->mouse_last_x = motion->x;
            d->mouse_last_y = motion->y;
            mouse_check_edges(widget, motion);
        }
        break;
    default:
        break;
    }
    return true;
}

static gboolean button_event(GtkWidget *widget, GdkEventButton *button)
{
    SpiceDisplay *display = SPICE_DISPLAY(widget);
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);

#if 0
    fprintf(stderr, "%s %s: button %d\n", __FUNCTION__,
            button->type == GDK_BUTTON_PRESS ? "press" : "release",
            button->button);
#endif

    gtk_widget_grab_focus(widget);
    try_mouse_grab(widget);

    if (!d->inputs)
        return true;

    switch (button->type) {
    case GDK_BUTTON_PRESS:
        spice_inputs_button_press(d->inputs,
                                  button_gdk_to_spice(button->button),
                                  button_mask_gdk_to_spice(button->state));
        break;
    case GDK_BUTTON_RELEASE:
        spice_inputs_button_release(d->inputs,
                                    button_gdk_to_spice(button->button),
                                    button_mask_gdk_to_spice(button->state));
        break;
    default:
        break;
    }
    return true;
}

static gboolean configure_event(GtkWidget *widget, GdkEventConfigure *conf)
{
    SpiceDisplay *display = SPICE_DISPLAY(widget);
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);

    if (conf->width != d->ww  || conf->height != d->wh) {
        d->ww = conf->width;
        d->wh = conf->height;
        recalc_geometry(widget);
    }
    return true;
}

/* ---------------------------------------------------------------- */

static const struct {
    const char  *x;
    int         s;
} atom2agent[] = {
    { .s = VD_AGENT_CLIPBOARD_UTF8_TEXT,  .x = "UTF8_STRING"              },
    { .s = VD_AGENT_CLIPBOARD_UTF8_TEXT,  .x = "text/plain;charset=utf-8" },
    { .s = VD_AGENT_CLIPBOARD_UTF8_TEXT,  .x = "STRING"                   },
    { .s = VD_AGENT_CLIPBOARD_UTF8_TEXT,  .x = "TEXT"                     },
    { .s = VD_AGENT_CLIPBOARD_UTF8_TEXT,  .x = "text/plain"               },

#if 0 /* gimp */
    { .s = VD_AGENT_CLIPBOARD_BITMAP,     .x = "image/bmp"                },
    { .s = VD_AGENT_CLIPBOARD_BITMAP,     .x = "image/x-bmp"              },
    { .s = VD_AGENT_CLIPBOARD_BITMAP,     .x = "image/x-MS-bmp"           },
    { .s = VD_AGENT_CLIPBOARD_BITMAP,     .x = "image/x-win-bitmap"       },
#endif

#if 0 /* firefox */
    { .s = VD_AGENT_CLIPBOARD_HTML,       .x = "text/html"                },
#endif
};

static void clipboard_get_targets(GtkClipboard *clipboard,
                                  GdkAtom *atoms,
                                  gint n_atoms,
                                  gpointer data)
{
    SpiceDisplay *display = data;
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);
    int types[SPICE_N_ELEMENTS(atom2agent)];
    char *name;
    int a, m, t;

#if 1 /* debug */
    fprintf(stderr, "%s:", __FUNCTION__);
    for (a = 0; a < n_atoms; a++) {
        fprintf(stderr, " %s",gdk_atom_name(atoms[a]));
    }
    fprintf(stderr, "\n");
#endif

    memset(types, 0, sizeof(types));
    for (a = 0; a < n_atoms; a++) {
        name = gdk_atom_name(atoms[a]);
        for (m = 0; m < SPICE_N_ELEMENTS(atom2agent); m++) {
            if (strcasecmp(name, atom2agent[m].x) != 0) {
                continue;
            }
            /* found match */
            for (t = 0; t < SPICE_N_ELEMENTS(atom2agent); t++) {
                if (types[t] == atom2agent[m].s) {
                    /* type already in list */
                    break;
                }
                if (types[t] == 0) {
                    /* add type to empty slot */
                    types[t] = atom2agent[m].s;
                    break;
                }
            }
            break;
        }
    }
    for (t = 0; t < SPICE_N_ELEMENTS(atom2agent); t++) {
        if (types[t] == 0) {
            break;
        }
    }
    if (!d->clip_grabbed && t > 0) {
        d->clip_grabbed = true;
        spice_main_clipboard_grab(d->main, types, t);
    }
}

static void clipboard_owner_change(GtkClipboard        *clipboard,
                                   GdkEventOwnerChange *event,
                                   gpointer            data)
{
    SpiceDisplay *display = data;
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);

    if (d->clip_grabbed) {
        d->clip_grabbed = false;
        spice_main_clipboard_release(d->main);
    }

    switch (event->reason) {
    case GDK_OWNER_CHANGE_NEW_OWNER:
        d->clip_hasdata = 1;
        if (d->auto_clipboard_enable)
            gtk_clipboard_request_targets(clipboard, clipboard_get_targets, data);
        break;
    default:
        d->clip_hasdata = 0;
        break;
    }
}

/* ---------------------------------------------------------------- */

static void spice_display_class_init(SpiceDisplayClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GtkObjectClass *gtkobject_class = GTK_OBJECT_CLASS(klass);
    GtkWidgetClass *gtkwidget_class = GTK_WIDGET_CLASS(klass);

    gtkwidget_class->expose_event = expose_event;
    gtkwidget_class->key_press_event = key_event;
    gtkwidget_class->key_release_event = key_event;
    gtkwidget_class->enter_notify_event = enter_event;
    gtkwidget_class->leave_notify_event = leave_event;
    gtkwidget_class->focus_in_event = focus_in_event;
    gtkwidget_class->focus_out_event = focus_out_event;
    gtkwidget_class->motion_notify_event = motion_event;
    gtkwidget_class->button_press_event = button_event;
    gtkwidget_class->button_release_event = button_event;
    gtkwidget_class->configure_event = configure_event;

    gtkobject_class->destroy = spice_display_destroy;

    gobject_class->finalize = spice_display_finalize;
    gobject_class->get_property = spice_display_get_property;
    gobject_class->set_property = spice_display_set_property;

    g_object_class_install_property
        (gobject_class, PROP_KEYBOARD_GRAB,
         g_param_spec_boolean("grab-keyboard",
                              "Grab Keyboard",
                              "Whether we should grab the keyboard.",
                              TRUE,
                              G_PARAM_READWRITE |
                              G_PARAM_CONSTRUCT |
                              G_PARAM_STATIC_NAME |
                              G_PARAM_STATIC_NICK |
                              G_PARAM_STATIC_BLURB));

    g_object_class_install_property
        (gobject_class, PROP_MOUSE_GRAB,
         g_param_spec_boolean("grab-mouse",
                              "Grab Mouse",
                              "Whether we should grab the mouse.",
                              TRUE,
                              G_PARAM_READWRITE |
                              G_PARAM_CONSTRUCT |
                              G_PARAM_STATIC_NAME |
                              G_PARAM_STATIC_NICK |
                              G_PARAM_STATIC_BLURB));

    g_object_class_install_property
        (gobject_class, PROP_RESIZE_GUEST,
         g_param_spec_boolean("resize-guest",
                              "Resize guest",
                              "Try to adapt guest display on window resize. "
                              "Requires guest cooperation.",
                              FALSE,
                              G_PARAM_READWRITE |
                              G_PARAM_CONSTRUCT |
                              G_PARAM_STATIC_NAME |
                              G_PARAM_STATIC_NICK |
                              G_PARAM_STATIC_BLURB));

    g_object_class_install_property
        (gobject_class, PROP_AUTO_CLIPBOARD,
         g_param_spec_boolean("auto-clipboard",
                              "Auto clipboard",
                              "Automatically relay clipboard changes between "
                              "host and guest.",
                              FALSE,
                              G_PARAM_READWRITE |
                              G_PARAM_CONSTRUCT |
                              G_PARAM_STATIC_NAME |
                              G_PARAM_STATIC_NICK |
                              G_PARAM_STATIC_BLURB));

        g_type_class_add_private(klass, sizeof(spice_display));
}

/* ---------------------------------------------------------------- */

static void mouse_update(SpiceChannel *channel, gpointer data)
{
    SpiceDisplay *display = data;
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);

    g_object_get(channel, "mouse-mode", &d->mouse_mode, NULL);
}

static void primary_create(SpiceChannel *channel, gint format,
                           gint width, gint height, gint stride,
                           gint shmid, gpointer imgdata, gpointer data)
{
    SpiceDisplay *display = data;
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);

    d->format = format;
    d->stride = stride;
    d->shmid  = shmid;
    d->data   = imgdata;

    if (d->width != width || d->height != height) {
        d->width  = width;
        d->height = height;
        recalc_geometry(GTK_WIDGET(display));
        if (!d->resize_guest_enable) {
            gtk_widget_set_size_request(GTK_WIDGET(display), width, height);
        }
    }
}

static void primary_destroy(SpiceChannel *channel, gpointer data)
{
    SpiceDisplay *display = SPICE_DISPLAY(data);
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);

    d->format = 0;
    d->width  = 0;
    d->height = 0;
    d->stride = 0;
    d->shmid  = 0;
    d->data   = 0;
    ximage_destroy(GTK_WIDGET(display));
}

static void invalidate(SpiceChannel *channel,
                       gint x, gint y, gint w, gint h, gpointer data)
{
    SpiceDisplay *display = data;
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);
    gtk_widget_queue_draw_area(GTK_WIDGET(display),
                               x + d->mx, y + d->my, w, h);
}

static void cursor_set(SpiceCursorChannel *channel,
                       gint width, gint height, gint hot_x, gint hot_y,
                       gpointer rgba, gpointer data)
{
    SpiceDisplay *display = data;
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);
    GdkDrawable *window;
    GdkDisplay *gtkdpy;
    GdkPixbuf *pixbuf;

    window = gtk_widget_get_window(GTK_WIDGET(display));
    if (!window)
        return;
    gtkdpy = gdk_drawable_get_display(window);

    pixbuf = gdk_pixbuf_new_from_data(rgba,
                                      GDK_COLORSPACE_RGB,
                                      TRUE, 8,
                                      width,
                                      height,
                                      width * 4,
                                      NULL, NULL);
    d->mouse_cursor = gdk_cursor_new_from_pixbuf(gtkdpy, pixbuf,
                                                 hot_x, hot_y);
    g_object_unref(pixbuf);
    gdk_window_set_cursor(window, d->mouse_cursor);
}

static void cursor_hide(SpiceCursorChannel *channel, gpointer data)
{
    SpiceDisplay *display = data;
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);
    GdkDrawable *window;

    window = gtk_widget_get_window(GTK_WIDGET(display));
    if (!window)
        return;

    d->mouse_cursor = gdk_cursor_new(GDK_BLANK_CURSOR);
    gdk_window_set_cursor(window, d->mouse_cursor);
}

static void cursor_move(SpiceCursorChannel *channel, gint x, gint y, gpointer data)
{
    fprintf(stderr, "%s: TODO (+%d+%d)\n", __FUNCTION__, x, y);
}

static void cursor_reset(SpiceCursorChannel *channel, gpointer data)
{
    fprintf(stderr, "%s: TODO\n", __FUNCTION__);
}

static void channel_new(SpiceSession *s, SpiceChannel *channel, gpointer data)
{
    SpiceDisplay *display = data;
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);
    int id = spice_channel_id(channel);

    if (SPICE_IS_MAIN_CHANNEL(channel)) {
        fprintf(stderr, "%s: main channel\n", __FUNCTION__);
        d->main = channel;
        g_signal_connect(channel, "spice-main-mouse-update",
                         G_CALLBACK(mouse_update), display);
        mouse_update(channel, display);
        return;
    }

    if (SPICE_IS_DISPLAY_CHANNEL(channel)) {
        fprintf(stderr, "%s: display channel\n", __FUNCTION__);
        if (id != d->channel_id)
            return;
        d->display = channel;
        g_signal_connect(channel, "spice-display-primary-create",
                         G_CALLBACK(primary_create), display);
        g_signal_connect(channel, "spice-display-primary-destroy",
                         G_CALLBACK(primary_destroy), display);
        g_signal_connect(channel, "spice-display-invalidate",
                         G_CALLBACK(invalidate), display);
        spice_channel_connect(channel);
        return;
    }

    if (SPICE_IS_CURSOR_CHANNEL(channel)) {
        fprintf(stderr, "%s: cursor channel\n", __FUNCTION__);
        if (id != d->channel_id)
            return;
        d->cursor = SPICE_CURSOR_CHANNEL(channel);
        g_signal_connect(channel, "spice-cursor-set",
                         G_CALLBACK(cursor_set), display);
        g_signal_connect(channel, "spice-cursor-move",
                         G_CALLBACK(cursor_move), display);
        g_signal_connect(channel, "spice-cursor-hide",
                         G_CALLBACK(cursor_hide), display);
        g_signal_connect(channel, "spice-cursor-reset",
                         G_CALLBACK(cursor_reset), display);
        spice_channel_connect(channel);
        return;
    }

    if (SPICE_IS_INPUTS_CHANNEL(channel)) {
        fprintf(stderr, "%s: inputs channel\n", __FUNCTION__);
        d->inputs = SPICE_INPUTS_CHANNEL(channel);
        spice_channel_connect(channel);
        return;
    }

    fprintf(stderr, "%s: unknown channel object\n", __FUNCTION__);
    return;
}

GtkWidget *spice_display_new(SpiceSession *session, int id)
{
    SpiceDisplay *display;
    spice_display *d;
    GList *list;

    display = g_object_new(SPICE_TYPE_DISPLAY, NULL);
    d = SPICE_DISPLAY_GET_PRIVATE(display);
    d->session = session;
    d->channel_id = id;

    g_signal_connect(session, "spice-session-channel-new",
                     G_CALLBACK(channel_new), display);
    list = spice_session_get_channels(session);
    for (list = g_list_first(list); list != NULL; list = g_list_next(list)) {
        channel_new(session, list->data, (gpointer*)display);
    }
    g_list_free(list);

    return GTK_WIDGET(display);
}

void spice_display_mouse_ungrab(GtkWidget *widget)
{
    try_mouse_ungrab(widget);
}

void spice_display_copy_to_guest(GtkWidget *widget)
{
    SpiceDisplay *display = SPICE_DISPLAY(widget);
    spice_display *d = SPICE_DISPLAY_GET_PRIVATE(display);

    if (d->clip_hasdata && !d->clip_grabbed) {
        gtk_clipboard_request_targets(d->clipboard, clipboard_get_targets, display);
    }
}

void spice_display_paste_from_guest(GtkWidget *widget)
{
    fprintf(stderr, "%s: TODO\n", __FUNCTION__);
}