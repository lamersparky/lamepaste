#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int append_bytes(unsigned char **buf, size_t *len, size_t *cap,
                        const unsigned char *data, size_t n)
{
    size_t need, next;
    unsigned char *p;
    if (n == 0) return 0;
    if (*len > SIZE_MAX - n) return -1;
    need = *len + n;
    if (need > *cap) {
        next = *cap ? *cap : 4096;
        while (next < need) {
            if (next > SIZE_MAX / 2) { next = need; break; }
            next *= 2;
        }
        p = realloc(*buf, next);
        if (!p) return -1;
        *buf = p;
        *cap = next;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    return 0;
}

static int get_property(Display *dpy, Window win, Atom prop,
                        Atom *type, int *format, unsigned long *items,
                        unsigned char **data)
{
    unsigned long after = 0;
    int rc = XGetWindowProperty(dpy, win, prop, 0, 1L << 24, True,
                                AnyPropertyType, type, format, items,
                                &after, data);
    return rc == Success && after == 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    Display *dpy;
    Window win;
    Atom clipboard, target, prop, incr;
    XEvent event;
    Atom actual_type;
    int actual_format;
    unsigned long items;
    unsigned char *chunk = NULL, *all = NULL;
    size_t all_len = 0, all_cap = 0;
    FILE *out;

    if (argc != 2) {
        fprintf(stderr, "usage: %s OUTPUT_FILE\n", argv[0]);
        return 2;
    }
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "cannot open DISPLAY\n"); return 1; }
    win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 1, 1, 0, 0, 0);
    XSelectInput(dpy, win, PropertyChangeMask);
    clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    target = XInternAtom(dpy, "UTF8_STRING", False);
    prop = XInternAtom(dpy, "LAMEPASTE_TEST_PROPERTY", False);
    incr = XInternAtom(dpy, "INCR", False);
    XConvertSelection(dpy, clipboard, target, prop, win, CurrentTime);
    XFlush(dpy);

    for (;;) {
        XNextEvent(dpy, &event);
        if (event.type == SelectionNotify) break;
    }
    if (event.xselection.property == None) {
        fprintf(stderr, "clipboard owner refused UTF8_STRING\n");
        XDestroyWindow(dpy, win); XCloseDisplay(dpy); return 1;
    }
    if (get_property(dpy, win, prop, &actual_type, &actual_format, &items, &chunk) != 0) {
        fprintf(stderr, "could not read clipboard property\n");
        XDestroyWindow(dpy, win); XCloseDisplay(dpy); return 1;
    }
    if (actual_type == incr) {
        if (chunk) XFree(chunk);
        for (;;) {
            XNextEvent(dpy, &event);
            if (event.type != PropertyNotify || event.xproperty.window != win ||
                event.xproperty.atom != prop || event.xproperty.state != PropertyNewValue)
                continue;
            chunk = NULL;
            if (get_property(dpy, win, prop, &actual_type, &actual_format,
                             &items, &chunk) != 0) {
                fprintf(stderr, "could not read INCR data chunk\n");
                free(all); XDestroyWindow(dpy, win); XCloseDisplay(dpy); return 1;
            }
            if (items == 0) {
                if (chunk) XFree(chunk);
                break;
            }
            if (actual_format != 8 || append_bytes(&all, &all_len, &all_cap,
                    chunk, (size_t)items) != 0) {
                fprintf(stderr, "invalid INCR chunk or allocation failure\n");
                if (chunk) XFree(chunk);
                free(all); XDestroyWindow(dpy, win); XCloseDisplay(dpy); return 1;
            }
            XFree(chunk);
        }
    } else {
        if (actual_format != 8 || append_bytes(&all, &all_len, &all_cap,
                chunk, (size_t)items) != 0) {
            fprintf(stderr, "invalid direct clipboard property\n");
            if (chunk) XFree(chunk);
            free(all); XDestroyWindow(dpy, win); XCloseDisplay(dpy); return 1;
        }
        if (chunk) XFree(chunk);
    }

    out = fopen(argv[1], "wb");
    if (!out) {
        fprintf(stderr, "cannot create output: %s\n", strerror(errno));
        free(all); XDestroyWindow(dpy, win); XCloseDisplay(dpy); return 1;
    }
    if (all_len && fwrite(all, 1, all_len, out) != all_len) {
        fprintf(stderr, "write failed\n");
        fclose(out); free(all); XDestroyWindow(dpy, win); XCloseDisplay(dpy); return 1;
    }
    if (fclose(out) != 0) {
        fprintf(stderr, "close output failed\n");
        free(all); XDestroyWindow(dpy, win); XCloseDisplay(dpy); return 1;
    }
    free(all);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
