#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define TEST_BIT(bits, bit) (((bits)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1UL)

#define MAX_DEVICES 128
#define MAX_GROUPS 64
#define DEVICE_NAME_LEN 256
#define DEVICE_PATH_LEN 256
#define UINPUT_NAME "memf"

struct input_device {
    char path[DEVICE_PATH_LEN];
    char name[DEVICE_NAME_LEN];
    struct input_id id;
    int fd;
};

struct device_group {
    unsigned short vendor;
    unsigned short product;
    size_t count;
};

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signo) {
    (void)signo;
    stop_requested = 1;
}

static void usage(FILE *stream, const char *program) {
    fprintf(stream,
            "Usage: %s [options]\n"
            "\n"
            "Options:\n"
            "  -l, --list              list candidate devices and exit\n"
            "  -v, --vendor HEX        force USB/input vendor id\n"
            "  -p, --product HEX       force USB/input product id\n"
            "  -d, --debug             print selected devices\n"
            "  -h, --help              show this help\n",
            program);
}

static int parse_hex_id(const char *value, unsigned short *out) {
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(value, &end, 16);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT16_MAX) {
        return -1;
    }

    *out = (unsigned short)parsed;
    return 0;
}

static bool contains_case_insensitive(const char *haystack, const char *needle) {
    size_t needle_len = strlen(needle);

    if (needle_len == 0) {
        return true;
    }

    for (const char *p = haystack; *p != '\0'; p++) {
        size_t i = 0;

        while (i < needle_len && p[i] != '\0' &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }

        if (i == needle_len) {
            return true;
        }
    }

    return false;
}

static int get_bits(int fd, unsigned int type, unsigned long *bits, size_t bytes) {
    memset(bits, 0, bytes);
    if (ioctl(fd, EVIOCGBIT(type, bytes), bits) < 0) {
        return -1;
    }
    return 0;
}

static bool is_candidate_device(int fd, const char *name) {
    unsigned long ev_bits[(EV_MAX + BITS_PER_LONG) / BITS_PER_LONG];
    unsigned long rel_bits[(REL_MAX + BITS_PER_LONG) / BITS_PER_LONG];

    if (contains_case_insensitive(name, "keyboard") || contains_case_insensitive(name, UINPUT_NAME)) {
        return false;
    }

    if (get_bits(fd, 0, ev_bits, sizeof(ev_bits)) < 0 || !TEST_BIT(ev_bits, EV_REL)) {
        return false;
    }

    if (get_bits(fd, EV_REL, rel_bits, sizeof(rel_bits)) < 0) {
        return false;
    }

    return TEST_BIT(rel_bits, REL_X) || TEST_BIT(rel_bits, REL_Y) || TEST_BIT(rel_bits, REL_WHEEL) ||
           TEST_BIT(rel_bits, REL_HWHEEL);
}

static int compare_device_path(const void *left, const void *right) {
    const struct input_device *a = left;
    const struct input_device *b = right;

    return strcmp(a->path, b->path);
}

static size_t scan_devices(struct input_device *devices, size_t capacity) {
    DIR *dir = opendir("/dev/input");
    struct dirent *entry;
    size_t count = 0;

    if (dir == NULL) {
        perror("opendir /dev/input");
        return 0;
    }

    while ((entry = readdir(dir)) != NULL) {
        struct input_device device;

        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        memset(&device, 0, sizeof(device));
        if (snprintf(device.path, sizeof(device.path), "/dev/input/%s", entry->d_name) >=
            (int)sizeof(device.path)) {
            continue;
        }
        device.fd = open(device.path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (device.fd < 0) {
            continue;
        }

        if (ioctl(device.fd, EVIOCGNAME(sizeof(device.name)), device.name) < 0 ||
            ioctl(device.fd, EVIOCGID, &device.id) < 0 || !is_candidate_device(device.fd, device.name)) {
            close(device.fd);
            continue;
        }

        close(device.fd);
        device.fd = -1;

        if (count < capacity) {
            devices[count++] = device;
        }
    }

    closedir(dir);
    qsort(devices, count, sizeof(devices[0]), compare_device_path);
    return count;
}

static void print_device(const struct input_device *device, bool selected) {
    printf("%c %s vendor=%04x product=%04x name=%s\n", selected ? '*' : ' ', device->path,
           device->id.vendor, device->id.product, device->name);
}

static int choose_group(const struct input_device *devices, size_t device_count,
                        unsigned short forced_vendor, unsigned short forced_product,
                        bool has_forced_vendor, bool has_forced_product,
                        unsigned short *vendor, unsigned short *product) {
    struct device_group groups[MAX_GROUPS];
    size_t group_count = 0;

    if (has_forced_vendor && has_forced_product) {
        *vendor = forced_vendor;
        *product = forced_product;
        return 0;
    }

    for (size_t i = 0; i < device_count; i++) {
        bool found = false;

        if ((has_forced_vendor && devices[i].id.vendor != forced_vendor) ||
            (has_forced_product && devices[i].id.product != forced_product)) {
            continue;
        }

        for (size_t j = 0; j < group_count; j++) {
            if (groups[j].vendor == devices[i].id.vendor && groups[j].product == devices[i].id.product) {
                groups[j].count++;
                found = true;
                break;
            }
        }

        if (!found && group_count < ARRAY_LEN(groups)) {
            groups[group_count].vendor = devices[i].id.vendor;
            groups[group_count].product = devices[i].id.product;
            groups[group_count].count = 1;
            group_count++;
        }
    }

    if (group_count == 0) {
        return -1;
    }

    size_t best = 0;
    for (size_t i = 1; i < group_count; i++) {
        if (groups[i].count > groups[best].count) {
            best = i;
        }
    }

    *vendor = groups[best].vendor;
    *product = groups[best].product;
    return 0;
}

static int enable_uinput_event(int fd, unsigned long request, int code) {
    if (ioctl(fd, request, code) < 0) {
        return -1;
    }
    return 0;
}

static int create_uinput_device(unsigned short vendor, unsigned short product) {
    static const int buttons[] = {
        BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA, BTN_FORWARD, BTN_BACK, BTN_TASK,
    };
    static const int rel_axes[] = {
        REL_X, REL_Y, REL_WHEEL, REL_HWHEEL,
#ifdef REL_WHEEL_HI_RES
        REL_WHEEL_HI_RES,
#endif
#ifdef REL_HWHEEL_HI_RES
        REL_HWHEEL_HI_RES,
#endif
    };
    struct uinput_setup setup;
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);

    if (fd < 0) {
        perror("open /dev/uinput");
        return -1;
    }

    if (enable_uinput_event(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        enable_uinput_event(fd, UI_SET_EVBIT, EV_REL) < 0 ||
        enable_uinput_event(fd, UI_SET_EVBIT, EV_SYN) < 0) {
        perror("configure uinput event types");
        close(fd);
        return -1;
    }

    for (size_t i = 0; i < ARRAY_LEN(buttons); i++) {
        if (enable_uinput_event(fd, UI_SET_KEYBIT, buttons[i]) < 0) {
            perror("configure uinput buttons");
            close(fd);
            return -1;
        }
    }

    for (size_t i = 0; i < ARRAY_LEN(rel_axes); i++) {
        if (enable_uinput_event(fd, UI_SET_RELBIT, rel_axes[i]) < 0) {
            perror("configure uinput relative axes");
            close(fd);
            return -1;
        }
    }

    memset(&setup, 0, sizeof(setup));
    snprintf(setup.name, sizeof(setup.name), UINPUT_NAME);
    setup.id.bustype = BUS_USB;
    setup.id.vendor = vendor;
    setup.id.product = product;
    setup.id.version = 1;

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("create uinput device");
        close(fd);
        return -1;
    }

    usleep(100000);
    return fd;
}

static int open_and_grab_selected(struct input_device *devices, size_t device_count,
                                  unsigned short vendor, unsigned short product, bool debug) {
    int selected = 0;

    for (size_t i = 0; i < device_count; i++) {
        if (devices[i].id.vendor != vendor || devices[i].id.product != product) {
            devices[i].fd = -1;
            continue;
        }

        devices[i].fd = open(devices[i].path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (devices[i].fd < 0) {
            fprintf(stderr, "failed to open %s: %s\n", devices[i].path, strerror(errno));
            continue;
        }

        if (ioctl(devices[i].fd, EVIOCGRAB, 1) < 0) {
            fprintf(stderr, "failed to grab %s: %s\n", devices[i].path, strerror(errno));
            close(devices[i].fd);
            devices[i].fd = -1;
            continue;
        }

        selected++;
        if (debug) {
            print_device(&devices[i], true);
        }
    }

    return selected;
}

static void close_devices(struct input_device *devices, size_t device_count) {
    for (size_t i = 0; i < device_count; i++) {
        if (devices[i].fd >= 0) {
            ioctl(devices[i].fd, EVIOCGRAB, 0);
            close(devices[i].fd);
            devices[i].fd = -1;
        }
    }
}

static bool should_forward_event(const struct input_event *event) {
    switch (event->type) {
    case EV_SYN:
        return event->code == SYN_REPORT;
    case EV_KEY:
        return event->code >= BTN_MOUSE && event->code <= BTN_TASK;
    case EV_REL:
        return event->code == REL_X || event->code == REL_Y || event->code == REL_WHEEL ||
               event->code == REL_HWHEEL
#ifdef REL_WHEEL_HI_RES
               || event->code == REL_WHEEL_HI_RES
#endif
#ifdef REL_HWHEEL_HI_RES
               || event->code == REL_HWHEEL_HI_RES
#endif
            ;
    default:
        return false;
    }
}

static int run_event_loop(struct input_device *devices, size_t device_count, int uinput_fd) {
    struct pollfd pollfds[MAX_DEVICES];
    size_t poll_count = 0;

    for (size_t i = 0; i < device_count; i++) {
        if (devices[i].fd >= 0) {
            pollfds[poll_count].fd = devices[i].fd;
            pollfds[poll_count].events = POLLIN;
            pollfds[poll_count].revents = 0;
            poll_count++;
        }
    }

    while (!stop_requested) {
        int ready = poll(pollfds, poll_count, -1);

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            return -1;
        }

        for (size_t i = 0; i < poll_count; i++) {
            if ((pollfds[i].revents & POLLIN) == 0) {
                continue;
            }

            for (;;) {
                struct input_event event;
                ssize_t bytes = read(pollfds[i].fd, &event, sizeof(event));

                if (bytes < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    perror("read input event");
                    return -1;
                }

                if (bytes != sizeof(event)) {
                    break;
                }

                if (should_forward_event(&event) && write(uinput_fd, &event, sizeof(event)) != sizeof(event)) {
                    perror("write uinput event");
                    return -1;
                }
            }
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    struct input_device devices[MAX_DEVICES];
    unsigned short forced_vendor = 0;
    unsigned short forced_product = 0;
    unsigned short selected_vendor = 0;
    unsigned short selected_product = 0;
    bool has_forced_vendor = false;
    bool has_forced_product = false;
    bool list_only = false;
    bool debug = false;
    size_t device_count;
    int uinput_fd = -1;
    int selected_count;
    int exit_code = EXIT_FAILURE;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0) {
            list_only = true;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
            debug = true;
        } else if ((strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--vendor") == 0) && i + 1 < argc) {
            if (parse_hex_id(argv[++i], &forced_vendor) < 0) {
                fprintf(stderr, "invalid vendor id: %s\n", argv[i]);
                return EXIT_FAILURE;
            }
            has_forced_vendor = true;
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--product") == 0) && i + 1 < argc) {
            if (parse_hex_id(argv[++i], &forced_product) < 0) {
                fprintf(stderr, "invalid product id: %s\n", argv[i]);
                return EXIT_FAILURE;
            }
            has_forced_product = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout, argv[0]);
            return EXIT_SUCCESS;
        } else {
            usage(stderr, argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (geteuid() != 0) {
        fprintf(stderr, "memf must run as root to read /dev/input and write /dev/uinput.\n");
        return EXIT_FAILURE;
    }

    device_count = scan_devices(devices, ARRAY_LEN(devices));
    if (device_count == 0) {
        fprintf(stderr, "no candidate mouse event devices found\n");
        return EXIT_FAILURE;
    }

    if (choose_group(devices, device_count, forced_vendor, forced_product, has_forced_vendor,
                     has_forced_product, &selected_vendor, &selected_product) < 0) {
        fprintf(stderr, "no devices matched the requested vendor/product\n");
        return EXIT_FAILURE;
    }

    if (list_only) {
        for (size_t i = 0; i < device_count; i++) {
            bool selected = devices[i].id.vendor == selected_vendor && devices[i].id.product == selected_product;
            print_device(&devices[i], selected);
        }
        return EXIT_SUCCESS;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (debug) {
        fprintf(stderr, "selected vendor=%04x product=%04x\n", selected_vendor, selected_product);
    }

    selected_count = open_and_grab_selected(devices, device_count, selected_vendor, selected_product, debug);
    if (selected_count <= 0) {
        fprintf(stderr, "failed to open and grab selected devices\n");
        goto out;
    }

    uinput_fd = create_uinput_device(selected_vendor, selected_product);
    if (uinput_fd < 0) {
        goto out;
    }

    exit_code = run_event_loop(devices, device_count, uinput_fd) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

out:
    if (uinput_fd >= 0) {
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
    }
    close_devices(devices, device_count);
    return exit_code;
}
