#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace {

bool get_property(Display *display, Window window, Atom property,
                  Atom &type, int &format, unsigned long &items,
                  unsigned char *&data)
{
    unsigned long bytes_after = 0;
    const int result = XGetWindowProperty(display, window, property, 0, 1L << 24,
        True, AnyPropertyType, &type, &format, &items, &bytes_after, &data);
    return result == Success && bytes_after == 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s OUTPUT_FILE\n", argv[0]);
        return 2;
    }

    Display *display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        std::fprintf(stderr, "cannot open DISPLAY\n");
        return 1;
    }

    const Window window = XCreateSimpleWindow(display, DefaultRootWindow(display),
                                               0, 0, 1, 1, 0, 0, 0);
    XSelectInput(display, window, PropertyChangeMask);
    const Atom clipboard = XInternAtom(display, "CLIPBOARD", False);
    const Atom target = XInternAtom(display, "UTF8_STRING", False);
    const Atom property = XInternAtom(display, "LAMEPASTE_TEST_PROPERTY", False);
    const Atom incr = XInternAtom(display, "INCR", False);
    XConvertSelection(display, clipboard, target, property, window, CurrentTime);
    XFlush(display);

    XEvent event{};
    do {
        XNextEvent(display, &event);
    } while (event.type != SelectionNotify);

    if (event.xselection.property == None) {
        std::fprintf(stderr, "clipboard owner refused UTF8_STRING\n");
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return 1;
    }

    Atom actual_type = None;
    int actual_format = 0;
    unsigned long items = 0;
    unsigned char *chunk = nullptr;
    if (!get_property(display, window, property, actual_type, actual_format, items, chunk)) {
        std::fprintf(stderr, "could not read clipboard property\n");
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return 1;
    }

    std::vector<unsigned char> all;
    if (actual_type == incr) {
        if (chunk != nullptr)
            XFree(chunk);
        for (;;) {
            XNextEvent(display, &event);
            if (event.type != PropertyNotify || event.xproperty.window != window ||
                event.xproperty.atom != property || event.xproperty.state != PropertyNewValue)
                continue;

            chunk = nullptr;
            if (!get_property(display, window, property, actual_type,
                              actual_format, items, chunk)) {
                std::fprintf(stderr, "could not read INCR data chunk\n");
                XDestroyWindow(display, window);
                XCloseDisplay(display);
                return 1;
            }
            if (items == 0) {
                if (chunk != nullptr)
                    XFree(chunk);
                break;
            }
            if (actual_format != 8 || items > std::numeric_limits<std::size_t>::max() - all.size()) {
                std::fprintf(stderr, "invalid INCR chunk size or format\n");
                if (chunk != nullptr)
                    XFree(chunk);
                XDestroyWindow(display, window);
                XCloseDisplay(display);
                return 1;
            }
            try {
                all.insert(all.end(), chunk, chunk + static_cast<std::size_t>(items));
            } catch (...) {
                std::fprintf(stderr, "allocation failed while reading INCR data\n");
                if (chunk != nullptr)
                    XFree(chunk);
                XDestroyWindow(display, window);
                XCloseDisplay(display);
                return 1;
            }
            XFree(chunk);
        }
    } else {
        if (actual_format != 8 || items > std::numeric_limits<std::size_t>::max()) {
            std::fprintf(stderr, "invalid direct clipboard property\n");
            if (chunk != nullptr)
                XFree(chunk);
            XDestroyWindow(display, window);
            XCloseDisplay(display);
            return 1;
        }
        try {
            if (items != 0)
                all.assign(chunk, chunk + static_cast<std::size_t>(items));
        } catch (...) {
            std::fprintf(stderr, "allocation failed while reading clipboard property\n");
            if (chunk != nullptr)
                XFree(chunk);
            XDestroyWindow(display, window);
            XCloseDisplay(display);
            return 1;
        }
        if (chunk != nullptr)
            XFree(chunk);
    }

    FILE *output = std::fopen(argv[1], "wb");
    if (output == nullptr) {
        std::fprintf(stderr, "cannot create output: %s\n", std::strerror(errno));
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return 1;
    }
    if (!all.empty() && std::fwrite(all.data(), 1, all.size(), output) != all.size()) {
        std::fprintf(stderr, "write failed\n");
        std::fclose(output);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return 1;
    }
    if (std::fclose(output) != 0) {
        std::fprintf(stderr, "close output failed\n");
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return 1;
    }

    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}
