/* SPDX-License-Identifier: MPL-2.0
 * Minimal fbdev and peripheral shim for the XDJ-RX3 1.19 rbp process.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <ucontext.h>

#define MAX_TRACKED_FDS 64
#define IMX6_GPT_BASE 0x02098000u
#define IMX6_GPT_COUNTER_WORD (0x24u / sizeof(uint32_t))
#define IMX6_GPT_TICKS_PER_US 3u

struct fake_device {
    const char *path;
    int fifo;
    const char *initial;
};

static const struct fake_device fake_devices[] = {
    { "/proc/udev_usb1", 1, NULL },
    { "/dev/snd/seq", 1, NULL },
    { "/dev/aloadSEQ", 1, NULL },
    { "/proc/udev_usbctn1", 0, "0\n" },
    { "/proc/udev_usbctn2", 0, "0\n" },
    { "/sys/devices/platform/pwm-backlight.1/backlight/pwm-backlight.1/max_brightness", 0, "255\n" },
    { "/sys/devices/platform/pwm-backlight.1/backlight/pwm-backlight.1/brightness", 0, "180\n" },
    { "/proc/udev_usb2", 1, NULL },
    { "/sys/class/paudiog/paudiog0/connect", 0, "0\n" },
    { "/dev/gpiodrv", 0, NULL },
    { "/dev/hidg0", 1, NULL },
    { "/dev/subucom_spi1.0", 1, NULL },
    { "/dev/subucom_spi2.0", 1, NULL },
    { "/dev/subucom_spi_rdy3.0", 1, NULL },
    { "/dev/subucom_spi_rdy4.0", 1, NULL },
    { "/dev/tsc2007_2-0048", 1, NULL },
    { NULL, 0, NULL }
};

static int (*real_open)(const char *, int, ...);
static int (*real_open64)(const char *, int, ...);
static int (*real_ioctl)(int, unsigned long, ...);
static int (*real_select)(int, fd_set *, fd_set *, fd_set *, struct timeval *);
static ssize_t (*real_read)(int, void *, size_t);
static ssize_t (*real_write)(int, const void *, size_t);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static void *(*real_mmap64)(void *, size_t, int, int, int, off64_t);
typedef unsigned long rx3emu_pthread_t;
static int (*real_pthread_create)(rx3emu_pthread_t *, const void *,
                                  void *(*)(void *), void *);
static int (*real_pthread_detach)(rx3emu_pthread_t);
static int (*real_sigaction)(int, const struct sigaction *, struct sigaction *);

static int width = 1280;
static int height = 720;
static int virtual_height = 1440;
static int bpp = 32;
static int yoffset;
static char output_directory[256] = "/tmp/rx3emu";
static int framebuffer_fds[MAX_TRACKED_FDS];
static int framebuffer_count;
static int fake_fds[MAX_TRACKED_FDS];
static const char *fake_fd_paths[MAX_TRACKED_FDS];
static int fake_count;
static unsigned int hardware_ioctl_logs;
static int gpt_fds[MAX_TRACKED_FDS];
static int gpt_count;
static volatile uint32_t *gpt_registers;
static int gpt_started;
static void *framebuffer_address;
static size_t framebuffer_mapping_size;
static int exporter_started;
static pid_t exporter_pid = -1;
static int semihosting_enabled;
static int semihosting_handle = -1;
static int crash_handler_installed;

#define SEMI_SYS_OPEN 0x01
#define SEMI_SYS_CLOSE 0x02
#define SEMI_SYS_WRITE 0x05

struct rx3emu_frame_header {
    char magic[8];
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t stride;
    uint32_t yoffset;
    uint32_t payload_size;
};

static uintptr_t semihosting_call(uintptr_t operation, void *argument)
{
#if defined(__arm__)
    register uintptr_t r0 __asm__("r0") = operation;
    register uintptr_t r1 __asm__("r1") = (uintptr_t)argument;
#if defined(__thumb__)
    __asm__ volatile("svc 0xab" : "+r"(r0) : "r"(r1) : "memory", "cc");
#else
    __asm__ volatile("svc 0x123456" : "+r"(r0) : "r"(r1) : "memory", "cc");
#endif
    return r0;
#else
    (void)operation;
    (void)argument;
    return (uintptr_t)-1;
#endif
}

static int semihosting_open_console(void)
{
    static const char output[] = "framebuffer.transport";
    uintptr_t arguments[3];
    arguments[0] = (uintptr_t)output;
    arguments[1] = 5; /* binary write */
    arguments[2] = sizeof(output) - 1;
    return (int)semihosting_call(SEMI_SYS_OPEN, arguments);
}

static void semihosting_close_output(void)
{
    uintptr_t argument = (uintptr_t)semihosting_handle;
    if (semihosting_handle >= 0)
        semihosting_call(SEMI_SYS_CLOSE, &argument);
    semihosting_handle = -1;
}

static int semihosting_write_all(const void *buffer, size_t length)
{
    const unsigned char *cursor = buffer;
    unsigned char staging[4096];
    while (length) {
        uintptr_t arguments[3];
        uintptr_t remaining;
        size_t chunk = length > 4096 ? 4096 : length;
        memcpy(staging, cursor, chunk);
        arguments[0] = (uintptr_t)semihosting_handle;
        arguments[1] = (uintptr_t)staging;
        arguments[2] = chunk;
        remaining = semihosting_call(SEMI_SYS_WRITE, arguments);
        if (remaining >= chunk)
            return -1;
        cursor += chunk - remaining;
        length -= chunk - remaining;
    }
    return 0;
}

static int export_semihosting_frame(void)
{
    struct rx3emu_frame_header header;
    size_t stride = (size_t)width * (size_t)(bpp / 8);
    size_t visible_size = stride * (size_t)height;
    size_t visible_offset = stride * (size_t)yoffset;
    if (!semihosting_enabled || !framebuffer_address ||
        visible_offset + visible_size > framebuffer_mapping_size)
        return -1;
    if (semihosting_handle < 0)
        semihosting_handle = semihosting_open_console();
    if (semihosting_handle < 0)
        return -1;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, "RX3FB01", 7);
    header.width = (uint32_t)width;
    header.height = (uint32_t)height;
    header.bpp = (uint32_t)bpp;
    header.stride = (uint32_t)stride;
    header.yoffset = (uint32_t)yoffset;
    header.payload_size = (uint32_t)visible_size;
    if (semihosting_write_all(&header, sizeof(header))) {
        semihosting_close_output();
        return -1;
    }
    if (semihosting_write_all(
        (const unsigned char *)framebuffer_address + visible_offset,
        visible_size
    )) {
        semihosting_close_output();
        return -1;
    }
    semihosting_close_output();
    return 0;
}

static void *semihosting_exporter(void *unused)
{
    int first = 1;
    (void)unused;
    fprintf(stderr, "RX3EMU: semihost exporter started mapping=%zu\n",
            framebuffer_mapping_size);
    for (;;) {
        int result = export_semihosting_frame();
        if (first) {
            fprintf(stderr, "RX3EMU: semihost first frame result=%d handle=%d\n",
                    result, semihosting_handle);
            first = 0;
        }
        usleep(500000);
    }
    return NULL;
}

static void initialize(void)
{
    const char *configured;
    struct sigaction action;
    if (real_ioctl)
        return;
    real_open = dlsym(RTLD_NEXT, "open");
    real_open64 = dlsym(RTLD_NEXT, "open64");
    real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    real_select = dlsym(RTLD_NEXT, "select");
    real_read = dlsym(RTLD_NEXT, "read");
    real_write = dlsym(RTLD_NEXT, "write");
    real_mmap = dlsym(RTLD_NEXT, "mmap");
    real_mmap64 = dlsym(RTLD_NEXT, "mmap64");
    real_pthread_create = dlsym(RTLD_NEXT, "pthread_create");
    real_pthread_detach = dlsym(RTLD_NEXT, "pthread_detach");
    real_sigaction = dlsym(RTLD_NEXT, "sigaction");
    configured = getenv("RX3EMU_OUTPUT");
    if (configured && configured[0]) {
        strncpy(output_directory, configured, sizeof(output_directory) - 1);
        output_directory[sizeof(output_directory) - 1] = '\0';
    }
    configured = getenv("RX3EMU_SEMIHOSTING");
    semihosting_enabled = configured && !strcmp(configured, "1");
    if (!crash_handler_installed && real_sigaction) {
        extern void rx3emu_crash_handler(int, siginfo_t *, void *);
        memset(&action, 0, sizeof(action));
        action.sa_sigaction = rx3emu_crash_handler;
        action.sa_flags = SA_SIGINFO | SA_RESETHAND;
        sigemptyset(&action.sa_mask);
        if (!real_sigaction(SIGSEGV, &action, NULL) &&
            !real_sigaction(SIGBUS, &action, NULL))
            crash_handler_installed = 1;
    }
}

void rx3emu_crash_handler(int signal_number, siginfo_t *information, void *context)
{
    char path[320];
    char payload[256];
    int descriptor;
    int length;
    unsigned long pc = 0;
    unsigned long lr = 0;
    unsigned long sp = 0;
#if defined(__arm__)
    ucontext_t *machine = (ucontext_t *)context;
    pc = (unsigned long)machine->uc_mcontext.arm_pc;
    lr = (unsigned long)machine->uc_mcontext.arm_lr;
    sp = (unsigned long)machine->uc_mcontext.arm_sp;
#else
    (void)context;
#endif
    snprintf(path, sizeof(path), "%s/crash.log", output_directory);
    descriptor = real_open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (descriptor >= 0) {
        length = snprintf(payload, sizeof(payload),
            "signal=%d address=%p pc=0x%08lx lr=0x%08lx sp=0x%08lx\n",
            signal_number, information ? information->si_addr : NULL, pc, lr, sp);
        real_write(descriptor, payload, (size_t)length);
        close(descriptor);
    }
    _exit(128 + signal_number);
}

static int tracked(const int *fds, int count, int fd)
{
    int index;
    for (index = 0; index < count; index++)
        if (fds[index] == fd)
            return 1;
    return 0;
}

static void remember(int *fds, int *count, int fd)
{
    if (fd >= 0 && *count < MAX_TRACKED_FDS)
        fds[(*count)++] = fd;
}

static void remember_fake(int fd, const char *path)
{
    if (fd >= 0 && fake_count < MAX_TRACKED_FDS) {
        fake_fds[fake_count] = fd;
        fake_fd_paths[fake_count] = path;
        fake_count++;
    }
}

static const char *fake_path(int fd)
{
    int index;
    for (index = fake_count - 1; index >= 0; index--)
        if (fake_fds[index] == fd)
            return fake_fd_paths[index];
    return "fake-device";
}

static void log_hardware(const char *operation, const char *path, long value)
{
    char log_path[320];
    char payload[448];
    int descriptor;
    int length;
    if (!real_open || !real_write)
        return;
    snprintf(log_path, sizeof(log_path), "%s/hardware.log", output_directory);
    descriptor = real_open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (descriptor < 0)
        return;
    length = snprintf(payload, sizeof(payload), "%s %s %ld\n",
                      operation, path ? path : "-", value);
    real_write(descriptor, payload, (size_t)length);
    close(descriptor);
}

static void *run_gpt_counter(void *unused)
{
    struct timeval now;
    uint64_t origin_us;
    (void)unused;
    gettimeofday(&now, NULL);
    origin_us = (uint64_t)now.tv_sec * 1000000u +
                (uint64_t)now.tv_usec;
    for (;;) {
        uint64_t current_us;
        gettimeofday(&now, NULL);
        current_us = (uint64_t)now.tv_sec * 1000000u +
                     (uint64_t)now.tv_usec;
        if (gpt_registers)
            gpt_registers[IMX6_GPT_COUNTER_WORD] =
                (uint32_t)((current_us - origin_us) * IMX6_GPT_TICKS_PER_US);
        usleep(100);
    }
    return NULL;
}

static void start_gpt_counter(void *mapping)
{
    rx3emu_pthread_t thread;
    gpt_registers = (volatile uint32_t *)mapping;
    if (!gpt_started && real_pthread_create && real_pthread_detach &&
        !real_pthread_create(&thread, NULL, run_gpt_counter, NULL)) {
        real_pthread_detach(thread);
        gpt_started = 1;
        log_hardware("gpt-start", "/dev/mem", 3000000);
    }
}

static size_t framebuffer_size(void)
{
    return (size_t)width * (size_t)virtual_height * (size_t)(bpp / 8);
}

static void ensure_framebuffer_capacity(int descriptor)
{
    off_t current = lseek(descriptor, 0, SEEK_END);
    off_t required = (off_t)framebuffer_size();
    if (current < required)
        ftruncate(descriptor, required);
    lseek(descriptor, 0, SEEK_SET);
}

static void write_metadata(void)
{
    char path[320];
    char payload[256];
    int descriptor;
    int length;
    snprintf(path, sizeof(path), "%s/framebuffer.json", output_directory);
    descriptor = real_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (descriptor < 0)
        return;
    length = snprintf(payload, sizeof(payload),
        "{\"width\":%d,\"height\":%d,\"virtual_height\":%d,"
        "\"bpp\":%d,\"stride\":%d,\"yoffset\":%d}\n",
        width, height, virtual_height, bpp, width * (bpp / 8), yoffset);
    real_write(descriptor, payload, (size_t)length);
    close(descriptor);
}

static int open_framebuffer(void)
{
    char path[320];
    int descriptor;
    snprintf(path, sizeof(path), "%s/framebuffer.raw", output_directory);
    descriptor = real_open(path, O_RDWR | O_CREAT, 0644);
    if (descriptor >= 0) {
        ensure_framebuffer_capacity(descriptor);
        remember(framebuffer_fds, &framebuffer_count, descriptor);
        write_metadata();
    }
    return descriptor;
}

static void remember_framebuffer_mapping(int descriptor, void *address, size_t length)
{
    if (!tracked(framebuffer_fds, framebuffer_count, descriptor) ||
        address == MAP_FAILED)
        return;
    if (semihosting_enabled)
        fprintf(stderr, "RX3EMU: framebuffer mmap fd=%d bytes=%zu\n",
                descriptor, length);
    framebuffer_address = address;
    framebuffer_mapping_size = length;
    if (semihosting_enabled && !exporter_started) {
        int initial_result = export_semihosting_frame();
        fprintf(stderr, "RX3EMU: semihost initial frame result=%d handle=%d\n",
                initial_result, semihosting_handle);
        exporter_started = 1;
        exporter_pid = fork();
        if (exporter_pid == 0) {
            semihosting_exporter(NULL);
            _exit(0);
        }
        if (exporter_pid < 0) {
            fprintf(stderr, "RX3EMU: semihost exporter fork failed\n");
            exporter_started = 0;
        } else {
            fprintf(stderr, "RX3EMU: semihost exporter pid=%d\n", exporter_pid);
        }
    }
}

void *mmap(void *address, size_t length, int protection, int flags,
           int descriptor, off_t offset)
{
    void *mapped;
    initialize();
    if (tracked(gpt_fds, gpt_count, descriptor) &&
        (unsigned long)offset == IMX6_GPT_BASE && length >= 0x28u) {
        mapped = real_mmap(address, length, protection | PROT_WRITE,
                           (flags & MAP_FIXED) | MAP_PRIVATE | MAP_ANONYMOUS,
                           -1, 0);
        if (mapped != MAP_FAILED)
            start_gpt_counter(mapped);
        log_hardware("gpt-mmap", "/dev/mem", (long)offset);
    } else {
        mapped = real_mmap(address, length, protection, flags, descriptor, offset);
    }
    remember_framebuffer_mapping(descriptor, mapped, length);
    return mapped;
}

void *mmap64(void *address, size_t length, int protection, int flags,
             int descriptor, off64_t offset)
{
    void *mapped;
    initialize();
    mapped = real_mmap64
        ? real_mmap64(address, length, protection, flags, descriptor, offset)
        : real_mmap(address, length, protection, flags, descriptor, (off_t)offset);
    remember_framebuffer_mapping(descriptor, mapped, length);
    return mapped;
}

static const struct fake_device *find_fake(const char *path)
{
    const struct fake_device *device;
    if (!path)
        return NULL;
    for (device = fake_devices; device->path; device++)
        if (!strcmp(device->path, path))
            return device;
    return NULL;
}

static int open_fake(const struct fake_device *device)
{
    char path[320];
    const char *base = strrchr(device->path, '/');
    const char *initial = device->initial;
    int descriptor;
    int fresh;
    base = base ? base + 1 : device->path;
    snprintf(path, sizeof(path), "/tmp/rx3emu-device-%s.%s",
             base, device->fifo ? "fifo" : "bin");
    if (device->fifo) {
        mkfifo(path, 0666);
        descriptor = real_open(path, O_RDWR | O_NONBLOCK);
    } else {
        fresh = access(path, F_OK) != 0;
        descriptor = real_open(path, O_RDWR | O_CREAT, 0666);
        if (descriptor >= 0 && fresh && initial) {
            real_write(descriptor, initial, strlen(initial));
            lseek(descriptor, 0, SEEK_SET);
        }
    }
    if (descriptor >= 0 && !strcmp(device->path, "/proc/udev_usb1") &&
        getenv("RX3_EMULATOR_USB1") &&
        !strcmp(getenv("RX3_EMULATOR_USB1"), "1")) {
        static const char event[] = "mount /media/usb1/sda1";
        real_write(descriptor, event, sizeof(event) - 1);
        log_hardware("usb-event", event, descriptor);
    }
    remember_fake(descriptor, device->path);
    return descriptor;
}

static int redirected_open(const char *path, int flags, mode_t mode, int use_open64)
{
    const struct fake_device *device;
    int descriptor;
    initialize();
    if (path && !strcmp(path, "/dev/fb0")) {
        descriptor = open_framebuffer();
        log_hardware("open-fb", path, descriptor);
        return descriptor;
    }
    if (path && !strcmp(path, "/dev/mem")) {
        descriptor = real_open(path, flags, mode);
        remember(gpt_fds, &gpt_count, descriptor);
        log_hardware("open-gpt", path, descriptor);
        return descriptor;
    }
    device = find_fake(path);
    if (device) {
        descriptor = open_fake(device);
        log_hardware("open-fake", path, descriptor);
        return descriptor;
    }
    descriptor = use_open64 && real_open64
        ? real_open64(path, flags, mode) : real_open(path, flags, mode);
    if (path && !strncmp(path, "/media/", 7))
        log_hardware("open-media", path, descriptor);
    if (path && (!strncmp(path, "/dev/", 5) ||
                 !strncmp(path, "/proc/", 6) ||
                 !strncmp(path, "/sys/", 5)))
        log_hardware("open-pass", path, descriptor);
    return descriptor;
}

int open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list arguments;
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
    }
    return redirected_open(path, flags, mode, 0);
}

int open64(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list arguments;
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
    }
    return redirected_open(path, flags, mode, 1);
}

ssize_t read(int descriptor, void *buffer, size_t length)
{
    ssize_t result;
    initialize();
    result = real_read(descriptor, buffer, length);
    if (tracked(fake_fds, fake_count, descriptor) &&
        hardware_ioctl_logs++ < 512u) {
        log_hardware("read-size", fake_path(descriptor), (long)length);
        log_hardware("read-result", fake_path(descriptor), (long)result);
    }
    return result;
}

ssize_t write(int descriptor, const void *buffer, size_t length)
{
    ssize_t result;
    initialize();
    result = real_write(descriptor, buffer, length);
    if (tracked(fake_fds, fake_count, descriptor) &&
        hardware_ioctl_logs++ < 512u) {
        log_hardware("write-size", fake_path(descriptor), (long)length);
        log_hardware("write-result", fake_path(descriptor), (long)result);
    }
    return result;
}

static void fill_variable(struct fb_var_screeninfo *value)
{
    memset(value, 0, sizeof(*value));
    value->xres = value->xres_virtual = (unsigned int)width;
    value->yres = (unsigned int)height;
    value->yres_virtual = (unsigned int)virtual_height;
    value->yoffset = (unsigned int)yoffset;
    value->bits_per_pixel = (unsigned int)bpp;
    value->activate = FB_ACTIVATE_NOW;
    value->vmode = FB_VMODE_NONINTERLACED;
    if (bpp == 16) {
        value->red.offset = 11; value->red.length = 5;
        value->green.offset = 5; value->green.length = 6;
        value->blue.offset = 0; value->blue.length = 5;
    } else {
        value->transp.offset = 24; value->transp.length = 8;
        value->red.offset = 16; value->red.length = 8;
        value->green.offset = 8; value->green.length = 8;
        value->blue.offset = 0; value->blue.length = 8;
    }
}

static void fill_fixed(struct fb_fix_screeninfo *value)
{
    memset(value, 0, sizeof(*value));
    strcpy(value->id, "rx3emu");
    value->smem_start = 0x10000000;
    value->smem_len = (unsigned int)framebuffer_size();
    value->type = FB_TYPE_PACKED_PIXELS;
    value->visual = FB_VISUAL_TRUECOLOR;
    value->line_length = (unsigned int)(width * (bpp / 8));
    value->ypanstep = 1;
    value->accel = FB_ACCEL_NONE;
}

int ioctl(int descriptor, unsigned long request, ...)
{
    va_list arguments;
    void *argument;
    initialize();
    va_start(arguments, request);
    argument = va_arg(arguments, void *);
    va_end(arguments);
    if (tracked(fake_fds, fake_count, descriptor)) {
        if (hardware_ioctl_logs++ < 512u)
            log_hardware("ioctl-fake", fake_path(descriptor), (long)request);
        return 0;
    }
    if (!tracked(framebuffer_fds, framebuffer_count, descriptor))
        return real_ioctl(descriptor, request, argument);
    switch (request) {
    case FBIOGET_VSCREENINFO:
        fill_variable(argument);
        return 0;
    case FBIOPUT_VSCREENINFO: {
        struct fb_var_screeninfo *requested = argument;
        if (requested->xres && requested->yres && requested->bits_per_pixel) {
            width = (int)requested->xres;
            height = (int)requested->yres;
            virtual_height = requested->yres_virtual >= requested->yres
                ? (int)requested->yres_virtual : height;
            bpp = (int)requested->bits_per_pixel;
            yoffset = 0;
            /* DirectFB keeps the first mapping alive while negotiating its
               final 16-bit mode. Never shrink storage beneath that mapping. */
            ensure_framebuffer_capacity(descriptor);
            write_metadata();
        }
        fill_variable(argument);
        return 0;
    }
    case FBIOGET_FSCREENINFO:
        fill_fixed(argument);
        return 0;
    case FBIOPAN_DISPLAY: {
        struct fb_var_screeninfo *requested = argument;
        static unsigned int export_counter;
        if ((int)requested->yoffset >= 0 &&
            requested->yoffset + (unsigned int)height <= (unsigned int)virtual_height)
            yoffset = (int)requested->yoffset;
        write_metadata();
        if (semihosting_enabled && !(export_counter++ % 30))
            export_semihosting_frame();
        return 0;
    }
    case FBIOBLANK:
    case FBIOPUTCMAP:
    case FBIOGETCMAP:
        return 0;
    default:
        return 0;
    }
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout)
{
    int result;
    initialize();
    /* The RX3 debug-console task passes 2,000,000 in tv_usec.  The vendor
       kernel accepted this as two seconds, while current Linux (and qemu-user)
       rejects it with EINVAL.  Normalize oversized microseconds so the task
       blocks as intended instead of consuming a core in an error loop. */
    if (timeout && timeout->tv_usec >= 1000000) {
        timeout->tv_sec += timeout->tv_usec / 1000000;
        timeout->tv_usec %= 1000000;
    }
    result = real_select(nfds, readfds, writefds, exceptfds, timeout);
    /* rbp's loopback debug console watches descriptors 0..4. Under qemu-user
       one emulated descriptor remains spuriously ready and turns this into a
       busy loop. A 1 ms host-only throttle restores scheduler fairness while
       retaining console responsiveness. */
    if (nfds == 5 && result > 0)
        usleep(1000);
    return result;
}
