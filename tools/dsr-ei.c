/* dsr-ei: send keyboard / mouse input to a headless gamescope through its libei socket.
 * Build (host): gcc -O2 -o ~/dsr-ref/tools/dsr-ei dsr-ei.c $(pkg-config --cflags --libs libei-1.0)
 * Usage: dsr-ei <socket> cmd...   (socket: $XDG_RUNTIME_DIR/gamescope-0-ei)
 *   key NAME [ms]   tap a key (hold ms, default 80)    down NAME / up NAME
 *   move DX DY      relative mouse motion (split into 20 px steps, one frame each)
 *   click [left|right|middle] [ms]                     wait MS
 * Key names: a-z 0-9 enter esc space tab bksp lshift lctrl lalt up down left right home end pgup pgdn f1-f12,
 *   or a raw evdev code. */
#include <libei.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct ei *ei;
static struct ei_device *kbd, *ptr;
static uint32_t seq;

static void sleep_ms(int ms) { struct timespec t = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&t, 0); }

static void pump(int ms) {
    struct pollfd p = {ei_get_fd(ei), POLLIN, 0};
    poll(&p, 1, ms);
    ei_dispatch(ei);
    struct ei_event *e;
    while ((e = ei_get_event(ei))) {
        struct ei_device *d = ei_event_get_device(e);
        switch (ei_event_get_type(e)) {
        case EI_EVENT_SEAT_ADDED:
            ei_seat_bind_capabilities(ei_event_get_seat(e), EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_KEYBOARD,
                                      EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL, NULL);
            break;
        case EI_EVENT_DEVICE_RESUMED:
            if (ei_device_has_capability(d, EI_DEVICE_CAP_KEYBOARD) && !kbd) kbd = ei_device_ref(d);
            if (ei_device_has_capability(d, EI_DEVICE_CAP_POINTER) && !ptr) ptr = ei_device_ref(d);
            ei_device_start_emulating(d, ++seq);
            break;
        case EI_EVENT_DISCONNECT: fprintf(stderr, "dsr-ei: disconnected\n"); exit(1);
        default: break;
        }
        ei_event_unref(e);
    }
}

static int keycode(const char *n) {
    static const struct { const char *n; int c; } t[] = {
        {"enter", KEY_ENTER}, {"esc", KEY_ESC}, {"space", KEY_SPACE}, {"tab", KEY_TAB}, {"bksp", KEY_BACKSPACE},
        {"lshift", KEY_LEFTSHIFT}, {"lctrl", KEY_LEFTCTRL}, {"lalt", KEY_LEFTALT}, {"up", KEY_UP}, {"down", KEY_DOWN},
        {"left", KEY_LEFT}, {"right", KEY_RIGHT}, {"f1", KEY_F1}, {"f2", KEY_F2}, {"f3", KEY_F3}, {"f4", KEY_F4},
        {"f5", KEY_F5}, {"f6", KEY_F6}, {"f7", KEY_F7}, {"f8", KEY_F8}, {"f9", KEY_F9}, {"f10", KEY_F10},
        {"f11", KEY_F11}, {"f12", KEY_F12}, {"home", KEY_HOME}, {"end", KEY_END}, {"pgup", KEY_PAGEUP},
        {"pgdn", KEY_PAGEDOWN}};
    static const char row[] = "qwertyuiop\0asdfghjkl\0zxcvbnm";
    static const int base[] = {KEY_Q, KEY_A, KEY_Z};
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) if (!strcmp(n, t[i].n)) return t[i].c;
    if (n[0] && !n[1]) {
        if (n[0] >= '1' && n[0] <= '9') return KEY_1 + n[0] - '1';
        if (n[0] == '0') return KEY_0;
        for (int r = 0, o = 0; r < 3; o += strlen(row + o) + 1, r++) {
            const char *p = strchr(row + o, n[0]);
            if (p) return base[r] + (p - (row + o));
        }
    }
    return atoi(n);
}

static void key(int code, int down) {
    ei_device_keyboard_key(kbd, code, down);
    ei_device_frame(kbd, ei_now(ei));
    pump(0);
}

static void button(int code, int down) {
    ei_device_button_button(ptr, code, down);
    ei_device_frame(ptr, ei_now(ei));
    pump(0);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: dsr-ei <socket> cmd...\n"); return 2; }
    ei = ei_new_sender(NULL);
    ei_configure_name(ei, "dsr-ei");
    if (ei_setup_backend_socket(ei, argv[1])) { fprintf(stderr, "dsr-ei: cannot connect %s\n", argv[1]); return 1; }
    for (int i = 0; i < 100 && !(kbd && ptr); i++) pump(20);
    if (!kbd || !ptr) { fprintf(stderr, "dsr-ei: no keyboard/pointer device\n"); return 1; }
    for (int i = 2; i < argc; i++) {
        const char *c = argv[i];
        const char *a = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(c, "key")) {
            int ms = i + 2 < argc && atoi(argv[i + 2]) > 0 ? atoi(argv[i + 2]) : 0;
            key(keycode(a), 1); sleep_ms(ms ? ms : 80); key(keycode(a), 0); sleep_ms(60);
            i += ms ? 2 : 1;
        } else if (!strcmp(c, "down")) { key(keycode(a), 1); i++; }
        else if (!strcmp(c, "up")) { key(keycode(a), 0); i++; }
        else if (!strcmp(c, "wait")) { int ms = atoi(a); for (; ms > 0; ms -= 20) pump(20); i++; }
        else if (!strcmp(c, "move")) {
            double dx = atof(a), dy = i + 2 < argc ? atof(argv[i + 2]) : 0;
            int n = (int)((dx * dx + dy * dy > 400 ? __builtin_sqrt(dx * dx + dy * dy) / 20 : 1));
            for (int s = 0; s < n; s++) {
                ei_device_pointer_motion(ptr, dx / n, dy / n);
                ei_device_frame(ptr, ei_now(ei));
                pump(16);
            }
            i += 2;
        } else if (!strcmp(c, "click")) {
            int b = !strcmp(a, "right") ? BTN_RIGHT : !strcmp(a, "middle") ? BTN_MIDDLE : BTN_LEFT;
            int named = !strcmp(a, "right") || !strcmp(a, "middle") || !strcmp(a, "left");
            button(b, 1); sleep_ms(80); button(b, 0); sleep_ms(60);
            i += named;
        } else { fprintf(stderr, "dsr-ei: unknown command %s\n", c); return 2; }
    }
    pump(50);
    ei_unref(ei);
    return 0;
}
