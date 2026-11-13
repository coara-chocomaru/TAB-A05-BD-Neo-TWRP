#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef DEVPTS_SUPER_MAGIC
#define DEVPTS_SUPER_MAGIC 0x1cd1
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define UB_NBITS(x) ((((x) + 1) + (sizeof(unsigned long) * 8 - 1)) / (sizeof(unsigned long) * 8))
#define UB_TEST_BIT(bit, array) (((array)[(bit) / (sizeof(unsigned long) * 8)] >> ((bit) % (sizeof(unsigned long) * 8))) & 1UL)

static const char *kDefaultRootfs = "/data/ubuntu";
static const char *kDefaultFramebuffer = "/dev/graphics/fb0";
static const char *kDefaultInputDirectory = "/dev/input";
static const char *kVersion = "1.6.3-a05bd";
static const int kFontWidth = 8;
static const int kFontHeight = 16;
static const int kFontScale = 2;
static const uint32_t kPalette[16] = {
    0x300A24, 0xCC0000, 0x4E9A06, 0xC4A000,
    0x3465A4, 0x75507B, 0x06989A, 0xD3D7CF,
    0x555753, 0xEF2929, 0x8AE234, 0xFCE94F,
    0x729FCF, 0xAD7FA8, 0x34E2E2, 0xFFFFFF
};

static volatile sig_atomic_t gChildPid = -1;
static volatile sig_atomic_t gCaughtSignal = 0;
static volatile sig_atomic_t gWatchdogAction = 0;

struct MountState {
    bool dev;
    bool proc;
    bool sys;
    bool devpts;
    bool shm;
};

static MountState gMounts = {false, false, false, false, false};
static bool gCreatedShmMountpoint = false;

static const unsigned char kFont[95][16] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x28, 0x28, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x14, 0x34, 0x7E, 0x2C, 0x28, 0xFE, 0x48, 0x50, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x10, 0x38, 0x74, 0x50, 0x30, 0x1C, 0x16, 0x54, 0x3C, 0x10, 0x10, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x60, 0x90, 0x90, 0x66, 0x18, 0x6C, 0x12, 0x12, 0x0C, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x38, 0x60, 0x60, 0x20, 0x70, 0xDA, 0xCE, 0x4C, 0x7E, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x10, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x08, 0x18, 0x10, 0x10, 0x30, 0x30, 0x30, 0x10, 0x10, 0x18, 0x08, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x20, 0x10, 0x10, 0x18, 0x18, 0x18, 0x18, 0x18, 0x10, 0x10, 0x20, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x10, 0x54, 0x38, 0x38, 0x54, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0x10, 0xFE, 0x10, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x10, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x04, 0x0C, 0x08, 0x18, 0x10, 0x10, 0x20, 0x20, 0x60, 0x40, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x38, 0x6C, 0x44, 0x46, 0x56, 0x46, 0x44, 0x6C, 0x38, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x78, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x38, 0x4C, 0x04, 0x04, 0x08, 0x18, 0x30, 0x60, 0x7C, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x38, 0x4C, 0x04, 0x0C, 0x38, 0x0C, 0x04, 0x4C, 0x78, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x0C, 0x1C, 0x3C, 0x2C, 0x6C, 0x4C, 0xFE, 0x0C, 0x0C, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x7C, 0x40, 0x40, 0x78, 0x0C, 0x04, 0x04, 0x4C, 0x78, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x38, 0x64, 0x40, 0x7C, 0x64, 0x46, 0x46, 0x64, 0x38, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x7C, 0x04, 0x0C, 0x08, 0x08, 0x18, 0x10, 0x30, 0x20, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x38, 0x64, 0x44, 0x64, 0x38, 0x64, 0x46, 0x64, 0x3C, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x38, 0x6C, 0x44, 0x44, 0x6E, 0x3C, 0x04, 0x4C, 0x38, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0x00, 0x00, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0x00, 0x00, 0x18, 0x10, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x06, 0x1C, 0x60, 0x60, 0x1C, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0xFE, 0x00, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0xC0, 0x78, 0x0E, 0x0E, 0x78, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x38, 0x4C, 0x04, 0x08, 0x10, 0x10, 0x00, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x3C, 0x66, 0xC2, 0x9E, 0xB2, 0xB2, 0x9E, 0x40, 0x60, 0x3C, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x18, 0x38, 0x28, 0x28, 0x6C, 0x64, 0x7C, 0x46, 0xC6, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x7C, 0x44, 0x44, 0x44, 0x7C, 0x44, 0x46, 0x46, 0x7C, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x3C, 0x64, 0x40, 0x40, 0x40, 0x40, 0x40, 0x64, 0x3C, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x78, 0x4C, 0x44, 0x46, 0x46, 0x46, 0x44, 0x4C, 0x78, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x7C, 0x40, 0x40, 0x40, 0x7C, 0x40, 0x40, 0x40, 0x7E, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x7E, 0x60, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x60, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x3C, 0x64, 0x40, 0x40, 0xCE, 0x46, 0x46, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x46, 0x46, 0x46, 0x46, 0x7E, 0x46, 0x46, 0x46, 0x46, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x7C, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x7C, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x3C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x48, 0x78, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x46, 0x4C, 0x58, 0x70, 0x70, 0x58, 0x4C, 0x44, 0x46, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0xC6, 0xE6, 0xEE, 0xEE, 0xDE, 0xD6, 0xC6, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x56, 0x56, 0x5E, 0x4E, 0x4E, 0x46, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x38, 0x6C, 0x44, 0x46, 0x46, 0x46, 0x44, 0x6C, 0x38, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x7C, 0x46, 0x46, 0x46, 0x7C, 0x40, 0x40, 0x40, 0x40, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x38, 0x6C, 0x44, 0x46, 0x46, 0x46, 0x44, 0x6C, 0x38, 0x0C, 0x04, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x78, 0x4C, 0x44, 0x4C, 0x78, 0x4C, 0x44, 0x46, 0x42, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x38, 0x64, 0x40, 0x60, 0x38, 0x04, 0x06, 0x44, 0x3C, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0xFE, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x64, 0x38, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0xC6, 0x46, 0x44, 0x64, 0x6C, 0x28, 0x28, 0x38, 0x18, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x82, 0x82, 0xD2, 0xDA, 0x7E, 0x6E, 0x6C, 0x6C, 0x64, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x46, 0x64, 0x2C, 0x38, 0x18, 0x38, 0x2C, 0x44, 0xC6, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0xC6, 0x44, 0x6C, 0x38, 0x18, 0x10, 0x10, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x7E, 0x04, 0x0C, 0x08, 0x18, 0x30, 0x20, 0x60, 0x7E, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x18, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x18, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x40, 0x60, 0x20, 0x20, 0x10, 0x10, 0x18, 0x08, 0x0C, 0x04, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x38, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x18, 0x2C, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x20, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x78, 0x04, 0x04, 0x3C, 0x44, 0x4C, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x40, 0x40, 0x40, 0x78, 0x64, 0x46, 0x46, 0x46, 0x64, 0x78, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x3C, 0x20, 0x60, 0x40, 0x60, 0x20, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x04, 0x04, 0x04, 0x3C, 0x6C, 0x44, 0x44, 0x44, 0x6C, 0x3C, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x38, 0x64, 0x46, 0x7E, 0x40, 0x64, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x1C, 0x10, 0x10, 0x7C, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x3C, 0x6C, 0x44, 0x44, 0x44, 0x6C, 0x3C, 0x04, 0x4C, 0x38, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x40, 0x40, 0x40, 0x7C, 0x64, 0x44, 0x44, 0x44, 0x44, 0x44, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x70, 0x10, 0x10, 0x10, 0x10, 0x10, 0x7E, 0x00, 0x00, 0x00},
    {0x00, 0x18, 0x00, 0x00, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x70, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x60, 0x60, 0x60, 0x64, 0x68, 0x70, 0x78, 0x68, 0x64, 0x66, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x70, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1C, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x7C, 0x56, 0x52, 0x52, 0x52, 0x52, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x7C, 0x64, 0x44, 0x44, 0x44, 0x44, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x38, 0x64, 0x44, 0x46, 0x44, 0x64, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x78, 0x64, 0x46, 0x46, 0x46, 0x64, 0x78, 0x40, 0x40, 0x40, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x3C, 0x6C, 0x44, 0x44, 0x44, 0x6C, 0x3C, 0x04, 0x04, 0x04, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x3E, 0x30, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x38, 0x64, 0x60, 0x38, 0x04, 0x44, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x30, 0x30, 0x7C, 0x30, 0x30, 0x30, 0x30, 0x30, 0x1C, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x44, 0x44, 0x44, 0x44, 0x44, 0x6C, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x46, 0x44, 0x64, 0x2C, 0x28, 0x38, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x82, 0x82, 0xD2, 0x5E, 0x6C, 0x6C, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x44, 0x2C, 0x38, 0x18, 0x38, 0x6C, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x46, 0x44, 0x64, 0x2C, 0x38, 0x38, 0x18, 0x10, 0x30, 0x60, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x7C, 0x0C, 0x08, 0x10, 0x30, 0x60, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x0C, 0x18, 0x10, 0x10, 0x10, 0x70, 0x10, 0x10, 0x10, 0x18, 0x0C, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00, 0x00},
    {0x00, 0x00, 0x70, 0x10, 0x10, 0x10, 0x18, 0x0C, 0x18, 0x10, 0x10, 0x10, 0x70, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x72, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
};

static void print_error(const char *operation, const char *path) {
    fprintf(stderr, "[ubuntu-loader] %s failed", operation);
    if (path != NULL && path[0] != '\0') {
        fprintf(stderr, " for %s", path);
    }
    fprintf(stderr, ": %s\n", strerror(errno));
}

static bool make_path(char *output, size_t outputSize,
                      const char *root, const char *relative) {
    int length;
    if (relative[0] == '/') {
        length = snprintf(output, outputSize, "%s%s", root, relative);
    } else {
        length = snprintf(output, outputSize, "%s/%s", root, relative);
    }
    return length > 0 && static_cast<size_t>(length) < outputSize;
}

static bool is_directory(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool read_exact_at(int fd, void *buffer, size_t size, off_t offset) {
    unsigned char *output = static_cast<unsigned char *>(buffer);
    size_t completed = 0;
    while (completed < size) {
        ssize_t count = pread(fd, output + completed, size - completed,
                              offset + static_cast<off_t>(completed));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (count == 0) {
            return false;
        }
        completed += static_cast<size_t>(count);
    }
    return true;
}

static bool resolve_inside_root(const char *root, const char *path,
                                char *resolvedPath, size_t resolvedSize) {
    if (realpath(path, resolvedPath) == NULL) {
        print_error("realpath", path);
        return false;
    }
    size_t rootLength = strlen(root);
    if (strncmp(resolvedPath, root, rootLength) != 0 ||
        (resolvedPath[rootLength] != '/' && resolvedPath[rootLength] != '\0')) {
        fprintf(stderr, "[ubuntu-loader] RootFS symlink escapes its root: %s\n", path);
        return false;
    }
    if (strlen(resolvedPath) >= resolvedSize) {
        fprintf(stderr, "[ubuntu-loader] Resolved path is too long: %s\n", path);
        return false;
    }
    return true;
}

static bool check_arm64_elf(const char *root, const char *relativePath) {
    char filePath[PATH_MAX];
    char resolvedFilePath[PATH_MAX];
    char interpreter[PATH_MAX];
    struct stat st;
    Elf64_Ehdr header;
    bool hasInterpreter = false;
    interpreter[0] = '\0';

    if (!make_path(filePath, sizeof(filePath), root, relativePath) ||
        !resolve_inside_root(root, filePath, resolvedFilePath, sizeof(resolvedFilePath))) {
        return false;
    }

    int fd = open(resolvedFilePath, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        print_error("open ELF", resolvedFilePath);
        return false;
    }

    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        !read_exact_at(fd, &header, sizeof(header), 0)) {
        fprintf(stderr, "[ubuntu-loader] Cannot read ELF executable: %s\n", filePath);
        close(fd);
        return false;
    }

    if (memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB ||
        header.e_ident[EI_VERSION] != EV_CURRENT ||
        header.e_version != EV_CURRENT ||
        header.e_ehsize != sizeof(Elf64_Ehdr) ||
        header.e_machine != EM_AARCH64 ||
        (header.e_type != ET_EXEC && header.e_type != ET_DYN) ||
        header.e_phentsize != sizeof(Elf64_Phdr) ||
        header.e_phnum > 256 ||
        header.e_phoff > static_cast<Elf64_Off>(st.st_size) ||
        static_cast<uint64_t>(header.e_phnum) * sizeof(Elf64_Phdr) >
            static_cast<uint64_t>(st.st_size) - header.e_phoff) {
        fprintf(stderr, "[ubuntu-loader] Not a valid ARM64 ELF executable: %s\n", filePath);
        close(fd);
        return false;
    }

    for (uint16_t index = 0; index < header.e_phnum; ++index) {
        Elf64_Phdr phdr;
        off_t offset = static_cast<off_t>(header.e_phoff) +
                       static_cast<off_t>(index) * sizeof(Elf64_Phdr);
        if (!read_exact_at(fd, &phdr, sizeof(phdr), offset)) {
            fprintf(stderr, "[ubuntu-loader] Cannot read ELF program header: %s\n", filePath);
            close(fd);
            return false;
        }
        if (phdr.p_type == PT_INTERP) {
            if (hasInterpreter || phdr.p_filesz < 2 ||
                phdr.p_filesz > sizeof(interpreter) ||
                phdr.p_offset > static_cast<Elf64_Off>(st.st_size) ||
                phdr.p_filesz > static_cast<uint64_t>(st.st_size) - phdr.p_offset ||
                !read_exact_at(fd, interpreter, static_cast<size_t>(phdr.p_filesz),
                               static_cast<off_t>(phdr.p_offset))) {
                fprintf(stderr, "[ubuntu-loader] Invalid ELF interpreter entry: %s\n", filePath);
                close(fd);
                return false;
            }
            if (interpreter[phdr.p_filesz - 1] != '\0' || interpreter[0] != '/') {
                fprintf(stderr, "[ubuntu-loader] Invalid ELF interpreter path: %s\n", filePath);
                close(fd);
                return false;
            }
            hasInterpreter = true;
        }
    }
    close(fd);

    if (hasInterpreter) {
        char interpreterPath[PATH_MAX];
        char resolvedInterpreter[PATH_MAX];
        if (!make_path(interpreterPath, sizeof(interpreterPath), root, interpreter) ||
            !resolve_inside_root(root, interpreterPath,
                                 resolvedInterpreter, sizeof(resolvedInterpreter))) {
            return false;
        }
        int interpreterFd = open(resolvedInterpreter, O_RDONLY | O_CLOEXEC);
        if (interpreterFd < 0) {
            print_error("open ELF interpreter", resolvedInterpreter);
            return false;
        }
        Elf64_Ehdr interpreterHeader;
        bool valid = read_exact_at(interpreterFd, &interpreterHeader,
                                   sizeof(interpreterHeader), 0) &&
            memcmp(interpreterHeader.e_ident, ELFMAG, SELFMAG) == 0 &&
            interpreterHeader.e_ident[EI_CLASS] == ELFCLASS64 &&
            interpreterHeader.e_ident[EI_DATA] == ELFDATA2LSB &&
            interpreterHeader.e_machine == EM_AARCH64 &&
            (interpreterHeader.e_type == ET_EXEC || interpreterHeader.e_type == ET_DYN);
        close(interpreterFd);
        if (!valid) {
            fprintf(stderr, "[ubuntu-loader] ELF interpreter is not ARM64: %s\n",
                    resolvedInterpreter);
            return false;
        }
    }
    return true;
}

static bool find_shell(const char *root, char *shell, size_t shellSize) {
    static const char *candidates[] = {
        "/bin/bash", "/usr/bin/bash", "/bin/sh", "/usr/bin/sh"
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        char path[PATH_MAX];
        if (!make_path(path, sizeof(path), root, candidates[i]) || access(path, X_OK) != 0) {
            continue;
        }
        if (!check_arm64_elf(root, candidates[i])) {
            continue;
        }
        size_t length = strlen(candidates[i]);
        if (length + 1 > shellSize) {
            return false;
        }
        memcpy(shell, candidates[i], length + 1);
        return true;
    }
    fprintf(stderr, "[ubuntu-loader] No executable ARM64 bash or sh found in RootFS\n");
    return false;
}

static bool check_mountpoint(const char *root, const char *relative) {
    char path[PATH_MAX];
    struct stat st;
    if (!make_path(path, sizeof(path), root, relative) || lstat(path, &st) != 0) {
        print_error("lstat mountpoint", relative);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "[ubuntu-loader] Mountpoint must be a real directory: %s\n", path);
        return false;
    }
    return true;
}

static bool validate_rootfs(const char *input, char *root, size_t rootSize,
                            char *shell, size_t shellSize) {
    struct stat st;
    if (realpath(input, root) == NULL) {
        print_error("realpath RootFS", input);
        return false;
    }
    if (strlen(root) >= rootSize || root[0] != '/' || strcmp(root, "/") == 0) {
        fprintf(stderr, "[ubuntu-loader] RootFS must be a dedicated mounted directory\n");
        return false;
    }
    if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "[ubuntu-loader] RootFS is not a directory: %s\n", input);
        return false;
    }
    struct statvfs fs;
    if (statvfs(root, &fs) != 0) {
        print_error("statvfs RootFS", root);
        return false;
    }
    if ((fs.f_flag & ST_NOEXEC) != 0) {
        fprintf(stderr, "[ubuntu-loader] RootFS filesystem is mounted noexec: %s\n", root);
        return false;
    }
    if ((fs.f_flag & ST_RDONLY) != 0) {
        fprintf(stderr, "[ubuntu-loader] WARNING: RootFS is read-only\n");
    }

    const char *required[] = {"etc", "usr", "dev", "proc", "sys"};
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        char path[PATH_MAX];
        if (!make_path(path, sizeof(path), root, required[i]) || !is_directory(path)) {
            fprintf(stderr, "[ubuntu-loader] Missing RootFS directory: %s\n", required[i]);
            return false;
        }
    }

    char osRelease[PATH_MAX];
    if (!make_path(osRelease, sizeof(osRelease), root, "etc/os-release") ||
        access(osRelease, R_OK) != 0) {
        fprintf(stderr, "[ubuntu-loader] Missing /etc/os-release\n");
        return false;
    }

    if (!check_mountpoint(root, "dev") ||
        !check_mountpoint(root, "proc") ||
        !check_mountpoint(root, "sys") ||
        !check_mountpoint(root, "dev/pts")) {
        return false;
    }

    if (!find_shell(root, shell, shellSize)) {
        return false;
    }
    return true;
}

static void signal_handler(int signalNumber) {
    gCaughtSignal = signalNumber;
    pid_t child = static_cast<pid_t>(gChildPid);
    if (child > 0) {
        kill(-child, signalNumber);
        kill(child, signalNumber);
    }
}

static bool install_signal_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0 ||
        sigaction(SIGHUP, &action, NULL) != 0) {
        print_error("sigaction", "signals");
        return false;
    }
    signal(SIGPIPE, SIG_IGN);
    return true;
}

static bool mount_one(const char *source, const char *target,
                      const char *filesystem, unsigned long flags,
                      const char *data, const char *label) {
    if (mount(source, target, filesystem, flags, data) != 0) {
        print_error(label, target);
        return false;
    }
    return true;
}

static bool is_devpts(const char *path) {
    struct statfs fs;
    if (statfs(path, &fs) != 0) {
        return false;
    }
    return static_cast<unsigned long>(fs.f_type) ==
           static_cast<unsigned long>(DEVPTS_SUPER_MAGIC);
}

static bool setup_mounts(const char *root) {
    char dev[PATH_MAX];
    char proc[PATH_MAX];
    char sys[PATH_MAX];
    char devpts[PATH_MAX];
    if (!make_path(dev, sizeof(dev), root, "dev") ||
        !make_path(proc, sizeof(proc), root, "proc") ||
        !make_path(sys, sizeof(sys), root, "sys") ||
        !make_path(devpts, sizeof(devpts), root, "dev/pts")) {
        fprintf(stderr, "[ubuntu-loader] Mount path too long\n");
        return false;
    }
    if (unshare(CLONE_NEWNS) != 0) {
        print_error("unshare mount namespace", NULL);
        fprintf(stderr, "[ubuntu-loader] Check CONFIG_NAMESPACES, CONFIG_MOUNT_NS and CAP_SYS_ADMIN\n");
        return false;
    }
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        print_error("make mounts private", "/");
        return false;
    }
    if (!is_devpts("/dev/pts")) {
        if (!mount_one("devpts", "/dev/pts", "devpts",
                       MS_NOSUID | MS_NOEXEC,
                       "mode=0620,ptmxmode=0666",
                       "mount recovery devpts")) {
            return false;
        }
        gMounts.devpts = true;
    }
    if (!mount_one("/dev", dev, NULL, MS_BIND | MS_REC, NULL, "bind /dev")) {
        return false;
    }
    gMounts.dev = true;
    char shm[PATH_MAX];
    if (!make_path(shm, sizeof(shm), root, "dev/shm")) {
        fprintf(stderr, "[ubuntu-loader] /dev/shm path too long\n");
        return false;
    }
    if (!is_directory(shm)) {
        if (mkdir(shm, 01777) != 0 && errno != EEXIST) {
            print_error("create /dev/shm", shm);
            return false;
        }
        gCreatedShmMountpoint = true;
    }
    if (!mount_one("tmpfs", shm, "tmpfs",
                   MS_NOSUID | MS_NODEV,
                   "mode=1777,size=128m", "mount /dev/shm")) {
        return false;
    }
    gMounts.shm = true;
    if (!mount_one("proc", proc, "proc",
                   MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RDONLY,
                   NULL, "mount proc")) {
        return false;
    }
    gMounts.proc = true;
    if (!mount_one("sysfs", sys, "sysfs",
                   MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RDONLY,
                   NULL, "mount sysfs")) {
        return false;
    }
    gMounts.sys = true;
    return true;
}

static bool unmount_one(const char *root, const char *relative, bool *mounted) {
    if (!*mounted) {
        return true;
    }
    char path[PATH_MAX];
    if (!make_path(path, sizeof(path), root, relative)) {
        fprintf(stderr, "[ubuntu-loader] Unmount path too long: %s\n", relative);
        return false;
    }
    if (umount2(path, 0) == 0) {
        *mounted = false;
        return true;
    }
    int firstError = errno;
    if (umount2(path, MNT_DETACH) == 0) {
        fprintf(stderr, "[ubuntu-loader] Detached busy mount %s after %s\n",
                path, strerror(firstError));
        *mounted = false;
        return true;
    }
    print_error("umount", path);
    return false;
}

static bool unmount_if_present(const char *path, const char *label) {
    if (umount2(path, 0) == 0) return true;
    int firstError = errno;
    if (firstError == EINVAL || firstError == ENOENT) return true;
    if (umount2(path, MNT_DETACH) == 0) {
        fprintf(stderr, "[ubuntu-loader] Detached busy mount %s after %s\n",
                path, strerror(firstError));
        return true;
    }
    errno = firstError;
    print_error(label, path);
    return false;
}

static bool cleanup_mounts(const char *root) {
    bool result = true;
    if (!unmount_one(root, "dev/shm", &gMounts.shm)) result = false;
    if (gCreatedShmMountpoint) {
        char rootShm[PATH_MAX];
        if (make_path(rootShm, sizeof(rootShm), root, "dev/shm")) {
            if (rmdir(rootShm) != 0 && errno != ENOENT) {
                print_error("remove temporary /dev/shm", rootShm);
                result = false;
            }
        }
        gCreatedShmMountpoint = false;
    }
    char rootDevPts[PATH_MAX];
    if (!make_path(rootDevPts, sizeof(rootDevPts), root, "dev/pts")) {
        fprintf(stderr, "[ubuntu-loader] Unmount path too long: %s\n", "dev/pts");
        result = false;
    } else if (!unmount_if_present(rootDevPts, "umount RootFS devpts")) {
        result = false;
    }
    if (!unmount_one(root, "sys", &gMounts.sys)) result = false;
    if (!unmount_one(root, "proc", &gMounts.proc)) result = false;
    if (!unmount_one(root, "dev", &gMounts.dev)) result = false;
    if (gMounts.devpts) {
        if (unmount_if_present("/dev/pts", "umount recovery devpts")) {
            gMounts.devpts = false;
        } else {
            result = false;
        }
    }
    return result;
}

static void reset_child_signals(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGQUIT, &action, NULL);
    signal(SIGPIPE, SIG_DFL);
}

static void child_exec(const char *root, const char *shell,
                       int argc, char **argv, int commandIndex,
                       const char *termName, bool usePty,
                       pid_t expectedParent) {
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != expectedParent) {
        _exit(125);
    }
    reset_child_signals();
    if (chdir(root) != 0 || chroot(".") != 0 || chdir("/") != 0) {
        fprintf(stderr, "[ubuntu-loader] chroot setup failed: %s\n", strerror(errno));
        _exit(126);
    }
    clearenv();
    setenv("HOME", "/root", 1);
    setenv("USER", "root", 1);
    setenv("LOGNAME", "root", 1);
    setenv("SHELL", shell, 1);
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
    setenv("LANG", "C.UTF-8", 1);
    setenv("LC_ALL", "C.UTF-8", 1);
    setenv("TERM", termName, 1);
        umask(022);

    if (commandIndex >= 0 && commandIndex < argc) {
        execvp(argv[commandIndex], &argv[commandIndex]);
        fprintf(stderr, "[ubuntu-loader] exec %s failed: %s\n",
                argv[commandIndex], strerror(errno));
        _exit(errno == ENOENT ? 127 : 126);
    }

    if (strcmp(shell, "/bin/bash") == 0 || strcmp(shell, "/usr/bin/bash") == 0) {
        if (usePty) {
            char *args[] = {const_cast<char *>("bash"),
                            const_cast<char *>("--login"),
                            const_cast<char *>("-i"), NULL};
            execv(shell, args);
        } else if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && isatty(STDERR_FILENO)) {
            char *args[] = {const_cast<char *>("bash"),
                            const_cast<char *>("--login"),
                            const_cast<char *>("-i"), NULL};
            execv(shell, args);
        } else {
            char *args[] = {const_cast<char *>("bash"),
                            const_cast<char *>("--login"), NULL};
            execv(shell, args);
        }
    } else {
        if (usePty || (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && isatty(STDERR_FILENO))) {
            char *args[] = {const_cast<char *>("sh"), const_cast<char *>("-i"), NULL};
            execv(shell, args);
        } else {
            char *args[] = {const_cast<char *>("sh"), NULL};
            execv(shell, args);
        }
    }
    fprintf(stderr, "[ubuntu-loader] Shell execution failed: %s\n", strerror(errno));
    _exit(127);
}

static bool check_recovery_pid(pid_t pid) {
    if (pid <= 1 || pid == getpid()) {
        fprintf(stderr, "[ubuntu-loader] Refusing unsafe recovery PID: %ld\n", static_cast<long>(pid));
        return false;
    }
    char path[64];
    char exe[PATH_MAX];
    snprintf(path, sizeof(path), "/proc/%ld/exe", static_cast<long>(pid));
    ssize_t length = readlink(path, exe, sizeof(exe) - 1);
    if (length < 0) {
        print_error("readlink recovery executable", path);
        return false;
    }
    exe[length] = '\0';
    const char *base = strrchr(exe, '/');
    base = base == NULL ? exe : base + 1;
    if (strcmp(base, "recovery") != 0) {
        fprintf(stderr, "[ubuntu-loader] PID %ld is not /recovery: %s\n",
                static_cast<long>(pid), exe);
        return false;
    }
    return true;
}

static bool process_is_stopped(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/status", static_cast<long>(pid));
    FILE *file = fopen(path, "r");
    if (file == NULL) return false;
    char line[256];
    bool stopped = false;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strncmp(line, "State:", 6) == 0) {
            char *p = line + 6;
            while (*p == ' ' || *p == '\t') ++p;
            stopped = (*p == 'T' || *p == 't');
            break;
        }
    }
    fclose(file);
    return stopped;
}

static bool freeze_recovery(pid_t pid) {
    if (!check_recovery_pid(pid)) return false;
    if (kill(pid, SIGSTOP) != 0) {
        print_error("SIGSTOP recovery", NULL);
        return false;
    }
    for (int i = 0; i < 300; ++i) {
        if (process_is_stopped(pid)) return true;
        usleep(10000);
    }
    kill(pid, SIGCONT);
    fprintf(stderr, "[ubuntu-loader] Recovery did not enter stopped state\n");
    return false;
}

static void watchdog_restore_handler(int) {
    gWatchdogAction = 1;
}

static void watchdog_stop_handler(int) {
    if (gWatchdogAction == 0) gWatchdogAction = 2;
}

static pid_t start_recovery_watchdog(pid_t recoveryPid,
                                      unsigned char *framebuffer,
                                      unsigned char *snapshot,
                                      size_t framebufferLength) {
    pid_t parentPid = getpid();
    pid_t watcher = fork();
    if (watcher < 0) {
        print_error("fork recovery watchdog", NULL);
        return -1;
    }
    if (watcher == 0) {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = watchdog_restore_handler;
        sigemptyset(&action.sa_mask);
        sigaction(SIGUSR1, &action, NULL);
        action.sa_handler = watchdog_stop_handler;
        sigaction(SIGTERM, &action, NULL);
        if (prctl(PR_SET_PDEATHSIG, SIGUSR1) != 0 || getppid() != parentPid) {
            if (framebuffer != NULL && snapshot != NULL && framebufferLength != 0) {
                memcpy(framebuffer, snapshot, framebufferLength);
                msync(framebuffer, framebufferLength, MS_SYNC);
            }
            kill(recoveryPid, SIGCONT);
            _exit(0);
        }
        long maxFd = sysconf(_SC_OPEN_MAX);
        if (maxFd < 0 || maxFd > 4096) maxFd = 4096;
        for (int fd = 0; fd < maxFd; ++fd) close(fd);
        while (gWatchdogAction == 0) pause();
        if (gWatchdogAction == 1) {
            if (framebuffer != NULL && snapshot != NULL && framebufferLength != 0) {
                memcpy(framebuffer, snapshot, framebufferLength);
                msync(framebuffer, framebufferLength, MS_SYNC);
            }
            kill(recoveryPid, SIGCONT);
        }
        _exit(0);
    }
    return watcher;
}

static void stop_watchdog(pid_t watcher) {
    if (watcher <= 0) return;
    if (kill(watcher, SIGTERM) != 0 && errno != ESRCH) {
        print_error("stop recovery watchdog", NULL);
    }
    int status = 0;
    while (waitpid(watcher, &status, 0) < 0 && errno == EINTR) {}
}

struct Framebuffer {
    int fd;
    unsigned char *memory;
    size_t length;
    unsigned char *backup;
    fb_fix_screeninfo fixed;
    fb_var_screeninfo variable;
    int width;
    int height;
    int rawWidth;
    int rawHeight;
    int rotation;
    int bytesPerPixel;
    bool ready;
    char devicePath[PATH_MAX];

    Framebuffer() : fd(-1), memory(NULL), length(0), backup(NULL),
                    width(0), height(0), rawWidth(0), rawHeight(0),
                    rotation(0), bytesPerPixel(0), ready(false) {
        memset(&fixed, 0, sizeof(fixed));
        memset(&variable, 0, sizeof(variable));
        memset(devicePath, 0, sizeof(devicePath));
    }

    bool open_device(const char *requested, bool preserveSnapshot = true) {
        const char *actualPath = requested;
        fd = open(actualPath, O_RDWR | O_CLOEXEC);
        if (fd < 0 && strcmp(requested, "/dev/graphics/fb0") == 0) {
            actualPath = "/dev/fb0";
            fd = open(actualPath, O_RDWR | O_CLOEXEC);
        }
        if (fd < 0) {
            print_error("open framebuffer", requested);
            return false;
        }
        if (strlen(actualPath) + 1 > sizeof(devicePath)) {
            fprintf(stderr, "[ubuntu-loader] Framebuffer device path too long\n");
            close(fd);
            fd = -1;
            return false;
        }
        strcpy(devicePath, actualPath);
        if (ioctl(fd, FBIOGET_FSCREENINFO, &fixed) != 0 ||
            ioctl(fd, FBIOGET_VSCREENINFO, &variable) != 0) {
            print_error("query framebuffer", requested);
            return false;
        }
        if (fixed.type != FB_TYPE_PACKED_PIXELS ||
            fixed.visual != FB_VISUAL_TRUECOLOR ||
            (variable.bits_per_pixel != 16 && variable.bits_per_pixel != 24 && variable.bits_per_pixel != 32) ||
            variable.xres == 0 || variable.yres == 0 ||
            fixed.line_length == 0 || fixed.smem_len == 0 ||
            variable.red.length == 0 || variable.green.length == 0 ||
            variable.blue.length == 0 || variable.red.msb_right != 0 ||
            variable.green.msb_right != 0 || variable.blue.msb_right != 0 ||
            variable.transp.msb_right != 0 || variable.nonstd != 0) {
            fprintf(stderr,
                    "[ubuntu-loader] Unsupported framebuffer: type=%u visual=%u bpp=%u\n",
                    fixed.type, fixed.visual, variable.bits_per_pixel);
            return false;
        }
        bytesPerPixel = static_cast<int>(variable.bits_per_pixel / 8);
        uint64_t right = static_cast<uint64_t>(variable.xoffset) + variable.xres;
        uint64_t bottom = static_cast<uint64_t>(variable.yoffset) + variable.yres;
        uint64_t visibleBytes = bottom * fixed.line_length;
        if (right * bytesPerPixel > fixed.line_length || visibleBytes > fixed.smem_len ||
            fixed.smem_len > 128U * 1024U * 1024U) {
            fprintf(stderr, "[ubuntu-loader] Invalid framebuffer memory geometry\n");
            return false;
        }
        rawWidth = static_cast<int>(variable.xres);
        rawHeight = static_cast<int>(variable.yres);
        width = rawWidth;
        height = rawHeight;
        rotation = 0;
        length = fixed.smem_len;
        memory = static_cast<unsigned char *>(mmap(NULL, length,
                    PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        if (memory == MAP_FAILED) {
            memory = NULL;
            print_error("mmap framebuffer", requested);
            return false;
        }
        if (preserveSnapshot) {
            backup = static_cast<unsigned char *>(malloc(length));
            if (backup == NULL) {
                fprintf(stderr, "[ubuntu-loader] Not enough memory to preserve TWRP screen\n");
                return false;
            }
        }
        ready = true;
        return true;
    }

    bool set_rotation(int degrees) {
        if (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270) {
            fprintf(stderr, "[ubuntu-loader] Invalid screen rotation: %d\n", degrees);
            return false;
        }
        rotation = degrees;
        if (degrees == 90 || degrees == 270) {
            width = rawHeight;
            height = rawWidth;
        } else {
            width = rawWidth;
            height = rawHeight;
        }
        return true;
    }

    bool snapshot() {
        if (!ready || backup == NULL) return false;
        memcpy(backup, memory, length);
        return true;
    }

    void restore() {
        if (!ready || backup == NULL || memory == NULL) return;
        if (fd >= 0) {
            fb_var_screeninfo original = variable;
            original.activate = FB_ACTIVATE_NOW;
            if (ioctl(fd, FBIOPUT_VSCREENINFO, &original) != 0) {
                print_error("restore framebuffer mode", devicePath);
            }
            original = variable;
            original.activate = FB_ACTIVATE_NOW;
            if (ioctl(fd, FBIOPAN_DISPLAY, &original) != 0) {
                print_error("restore framebuffer layer", devicePath);
            }
        }
        memcpy(memory, backup, length);
        if (msync(memory, length, MS_SYNC) != 0 && errno != EINVAL) {
            print_error("sync restored framebuffer", devicePath);
        }
    }

    uint32_t component(uint8_t value, const fb_bitfield &field) const {
        if (field.length == 0 || field.length > 16 || field.offset + field.length > variable.bits_per_pixel) {
            return 0;
        }
        uint32_t maxValue = (1U << field.length) - 1U;
        uint32_t scaled = (static_cast<uint32_t>(value) * maxValue + 127U) / 255U;
        return (scaled << field.offset);
    }

    uint32_t color_value(uint32_t rgb) const {
        uint8_t r = static_cast<uint8_t>((rgb >> 16) & 0xff);
        uint8_t g = static_cast<uint8_t>((rgb >> 8) & 0xff);
        uint8_t b = static_cast<uint8_t>(rgb & 0xff);
        return component(r, variable.red) |
               component(g, variable.green) |
               component(b, variable.blue) |
               component(255, variable.transp);
    }

    void write_raw_pixel(int rawX, int rawY, uint32_t value) {
        if (!ready || rawX < 0 || rawY < 0 || rawX >= rawWidth || rawY >= rawHeight) return;
        size_t offset = static_cast<size_t>(variable.yoffset + static_cast<uint32_t>(rawY)) * fixed.line_length +
                        static_cast<size_t>(variable.xoffset + static_cast<uint32_t>(rawX)) * bytesPerPixel;
        if (offset + bytesPerPixel > length) return;
        if (bytesPerPixel == 2) {
            uint16_t pixelValue = static_cast<uint16_t>(value);
            memcpy(memory + offset, &pixelValue, sizeof(pixelValue));
        } else if (bytesPerPixel == 3) {
            memcpy(memory + offset, &value, 3);
        } else {
            memcpy(memory + offset, &value, sizeof(value));
        }
    }

    void pixel(int x, int y, uint32_t rgb) {
        if (!ready || x < 0 || y < 0 || x >= width || y >= height) return;
        int rawX = x;
        int rawY = y;
        if (rotation == 90) {
            rawX = y;
            rawY = rawHeight - 1 - x;
        } else if (rotation == 180) {
            rawX = rawWidth - 1 - x;
            rawY = rawHeight - 1 - y;
        } else if (rotation == 270) {
            rawX = rawWidth - 1 - y;
            rawY = x;
        }
        write_raw_pixel(rawX, rawY, color_value(rgb));
    }

    void fill_rect(int x, int y, int w, int h, uint32_t rgb) {
        if (!ready || w <= 0 || h <= 0) return;
        int64_t right = static_cast<int64_t>(x) + w;
        int64_t bottom = static_cast<int64_t>(y) + h;
        int x0 = x < 0 ? 0 : x;
        int y0 = y < 0 ? 0 : y;
        int x1 = right > width ? width : static_cast<int>(right);
        int y1 = bottom > height ? height : static_cast<int>(bottom);
        if (x1 <= x0 || y1 <= y0) return;
        uint32_t value = color_value(rgb);
        if (rotation != 0) {
            for (int yy = y0; yy < y1; ++yy) {
                for (int xx = x0; xx < x1; ++xx) {
                    int rawX = xx;
                    int rawY = yy;
                    if (rotation == 90) {
                        rawX = yy;
                        rawY = rawHeight - 1 - xx;
                    } else if (rotation == 180) {
                        rawX = rawWidth - 1 - xx;
                        rawY = rawHeight - 1 - yy;
                    } else if (rotation == 270) {
                        rawX = rawWidth - 1 - yy;
                        rawY = xx;
                    }
                    write_raw_pixel(rawX, rawY, value);
                }
            }
            return;
        }
        for (int yy = y0; yy < y1; ++yy) {
            size_t offset = static_cast<size_t>(variable.yoffset + static_cast<uint32_t>(yy)) * fixed.line_length +
                            static_cast<size_t>(variable.xoffset + static_cast<uint32_t>(x0)) * bytesPerPixel;
            size_t count = static_cast<size_t>(x1 - x0);
            if (offset + count * bytesPerPixel > length) continue;
            if (bytesPerPixel == 2) {
                uint16_t *row = reinterpret_cast<uint16_t *>(memory + offset);
                uint16_t pixelValue = static_cast<uint16_t>(value);
                for (size_t xx = 0; xx < count; ++xx) row[xx] = pixelValue;
            } else if (bytesPerPixel == 3) {
                for (size_t xx = 0; xx < count; ++xx) {
                    memcpy(memory + offset + xx * 3, &value, 3);
                }
            } else {
                uint32_t *row = reinterpret_cast<uint32_t *>(memory + offset);
                for (size_t xx = 0; xx < count; ++xx) row[xx] = value;
            }
        }
    }

    void close_device() {
        if (memory != NULL) {
            munmap(memory, length);
            memory = NULL;
        }
        if (backup != NULL) {
            free(backup);
            backup = NULL;
        }
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
        ready = false;
    }
};

struct Cell {
    unsigned char ch;
    unsigned char fg;
    unsigned char bg;
    bool bold;
};

class Terminal {
public:
    int columns;
    int rows;
    int cursorX;
    int cursorY;
    int savedX;
    int savedY;
    unsigned char foreground;
    unsigned char background;
    bool bold;
    bool dirty;
    std::vector<Cell> cells;

    Terminal() : columns(80), rows(24), cursorX(0), cursorY(0),
                 savedX(0), savedY(0), foreground(7), background(0),
                 bold(false), dirty(true), parserState(0), csiLength(0) {
        memset(csi, 0, sizeof(csi));
        resize(80, 24);
    }

    void resize(int cols, int rowCount) {
        columns = cols < 1 ? 1 : cols;
        rows = rowCount < 1 ? 1 : rowCount;
        cells.assign(static_cast<size_t>(columns) * rows, Cell{' ', 7, 0, false});
        cursorX = 0;
        cursorY = 0;
        dirty = true;
    }

    void clear_cell(int x, int y) {
        if (x < 0 || y < 0 || x >= columns || y >= rows) return;
        cells[static_cast<size_t>(y) * columns + x] = Cell{' ', foreground, background, bold};
    }

    void clear_all() {
        for (size_t i = 0; i < cells.size(); ++i) {
            cells[i] = Cell{' ', foreground, background, bold};
        }
        cursorX = 0;
        cursorY = 0;
        dirty = true;
    }

    void scroll_up() {
        if (rows <= 1) {
            clear_cell(0, 0);
            return;
        }
        memmove(&cells[0], &cells[columns],
                static_cast<size_t>(rows - 1) * columns * sizeof(Cell));
        for (int x = 0; x < columns; ++x) clear_cell(x, rows - 1);
        dirty = true;
    }

    void newline() {
        cursorX = 0;
        ++cursorY;
        if (cursorY >= rows) {
            scroll_up();
            cursorY = rows - 1;
        }
    }

    void put_char(unsigned char ch) {
        if (ch == '\r') {
            cursorX = 0;
            return;
        }
        if (ch == '\n') {
            newline();
            dirty = true;
            return;
        }
        if (ch == '\b' || ch == 0x7f) {
            if (cursorX > 0) --cursorX;
            dirty = true;
            return;
        }
        if (ch == '\t') {
            int next = ((cursorX / 8) + 1) * 8;
            while (cursorX < next) put_char(' ');
            return;
        }
        if (ch < 32 || ch > 126) {
            if (ch >= 0x80) {
                if ((ch & 0xc0) != 0x80) put_char('?');
            }
            return;
        }
        if (cursorX >= columns) newline();
        Cell &cell = cells[static_cast<size_t>(cursorY) * columns + cursorX];
        cell.ch = ch;
        cell.fg = foreground;
        cell.bg = background;
        cell.bold = bold;
        ++cursorX;
        dirty = true;
    }

    void feed(const unsigned char *data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            unsigned char ch = data[i];
            if (parserState == 0) {
                if (ch == 0x1b) {
                    parserState = 1;
                } else {
                    put_char(ch);
                }
            } else if (parserState == 1) {
                if (ch == '[') {
                    parserState = 2;
                    csiLength = 0;
                    memset(csi, 0, sizeof(csi));
                } else if (ch == ']') {
                    parserState = 3;
                } else if (ch == '7') {
                    savedX = cursorX;
                    savedY = cursorY;
                    parserState = 0;
                } else if (ch == '8') {
                    cursorX = savedX;
                    cursorY = savedY;
                    parserState = 0;
                    dirty = true;
                } else {
                    parserState = 0;
                }
            } else if (parserState == 2) {
                if (csiLength + 1 < sizeof(csi)) csi[csiLength++] = static_cast<char>(ch);
                if (ch >= 0x40 && ch <= 0x7e) {
                    handle_csi(ch);
                    parserState = 0;
                }
            } else if (parserState == 3) {
                if (ch == 0x07) parserState = 0;
                else if (ch == 0x1b) parserState = 4;
            } else if (parserState == 4) {
                parserState = ch == '\\' ? 0 : 3;
            }
        }
    }

private:
    int parserState;
    char csi[128];
    size_t csiLength;

    int param_value(const int *params, int count, int index, int fallback) {
        if (index >= count || params[index] < 0) return fallback;
        return params[index] == 0 ? fallback : params[index];
    }

    void handle_csi(char finalChar) {
        int params[16];
        int count = 0;
        for (int i = 0; i < 16; ++i) params[i] = -1;
        bool privateMode = csiLength > 0 && csi[0] == '?';
        int value = 0;
        bool haveValue = false;
        for (size_t i = privateMode ? 1 : 0; i < csiLength; ++i) {
            char ch = csi[i];
            if (ch >= '0' && ch <= '9') {
                value = value * 10 + (ch - '0');
                haveValue = true;
            } else if (ch == ';') {
                if (count < 16) params[count++] = haveValue ? value : 0;
                value = 0;
                haveValue = false;
            }
        }
        if (count < 16 && (haveValue || count == 0)) params[count++] = haveValue ? value : 0;
        int n = param_value(params, count, 0, 1);
        switch (finalChar) {
            case 'A': cursorY -= n; break;
            case 'B': cursorY += n; break;
            case 'C': cursorX += n; break;
            case 'D': cursorX -= n; break;
            case 'G': cursorX = n - 1; break;
            case 'd': cursorY = n - 1; break;
            case 'H':
            case 'f':
                cursorY = param_value(params, count, 0, 1) - 1;
                cursorX = param_value(params, count, 1, 1) - 1;
                break;
            case 'J': {
                int mode = count == 0 ? 0 : params[0];
                if (mode == 2 || mode == 3) clear_all();
                else if (mode == 0) {
                    for (int y = cursorY; y < rows; ++y)
                        for (int x = (y == cursorY ? cursorX : 0); x < columns; ++x) clear_cell(x, y);
                } else if (mode == 1) {
                    for (int y = 0; y <= cursorY; ++y)
                        for (int x = 0; x < (y == cursorY ? cursorX + 1 : columns); ++x) clear_cell(x, y);
                }
                break;
            }
            case 'K': {
                int mode = count == 0 ? 0 : params[0];
                if (mode == 0) for (int x = cursorX; x < columns; ++x) clear_cell(x, cursorY);
                else if (mode == 1) for (int x = 0; x <= cursorX; ++x) clear_cell(x, cursorY);
                else if (mode == 2) for (int x = 0; x < columns; ++x) clear_cell(x, cursorY);
                break;
            }
            case 'm':
                for (int i = 0; i < count; ++i) {
                    int p = params[i] < 0 ? 0 : params[i];
                    if (p == 0) { foreground = 7; background = 0; bold = false; }
                    else if (p == 1) bold = true;
                    else if (p == 22) bold = false;
                    else if (p >= 30 && p <= 37) foreground = static_cast<unsigned char>(p - 30);
                    else if (p >= 40 && p <= 47) background = static_cast<unsigned char>(p - 40);
                    else if (p >= 90 && p <= 97) foreground = static_cast<unsigned char>(p - 90 + 8);
                    else if (p >= 100 && p <= 107) background = static_cast<unsigned char>(p - 100 + 8);
                    else if (p == 39) foreground = 7;
                    else if (p == 49) background = 0;
                }
                break;
            case 's': savedX = cursorX; savedY = cursorY; break;
            case 'u': cursorX = savedX; cursorY = savedY; break;
            case 'h':
            case 'l':
                break;
            default:
                break;
        }
        if (cursorX < 0) cursorX = 0;
        if (cursorY < 0) cursorY = 0;
        if (cursorX >= columns) cursorX = columns - 1;
        if (cursorY >= rows) cursorY = rows - 1;
        dirty = true;
    }
};

static void draw_glyph(Framebuffer &fb, int x, int y, unsigned char ch,
                       uint32_t fg, uint32_t bg, int scale) {
    if (ch < 32 || ch > 126) ch = '?';
    const unsigned char *glyph = kFont[ch - 32];
    fb.fill_rect(x, y, kFontWidth * scale, kFontHeight * scale, bg);
    for (int gy = 0; gy < kFontHeight; ++gy) {
        unsigned char bits = glyph[gy];
        int gx = 0;
        while (gx < kFontWidth) {
            while (gx < kFontWidth && (bits & (0x80 >> gx)) == 0) ++gx;
            int start = gx;
            while (gx < kFontWidth && (bits & (0x80 >> gx)) != 0) ++gx;
            if (gx > start) {
                fb.fill_rect(x + start * scale, y + gy * scale,
                             (gx - start) * scale, scale, fg);
            }
        }
    }
}

static void draw_text(Framebuffer &fb, int x, int y, const char *text,
                      uint32_t fg, uint32_t bg, int scale, int maxWidth) {
    int origin = x;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] == '\n') {
            x = origin;
            y += kFontHeight * scale;
            continue;
        }
        if (x + kFontWidth * scale > maxWidth) break;
        draw_glyph(fb, x, y, static_cast<unsigned char>(text[i]), fg, bg, scale);
        x += kFontWidth * scale;
    }
}

enum SoftKeyType {
    SOFT_CHAR,
    SOFT_SHIFT,
    SOFT_CTRL,
    SOFT_TAB,
    SOFT_ENTER,
    SOFT_BACKSPACE,
    SOFT_SPACE,
    SOFT_ESCAPE,
    SOFT_UP,
    SOFT_DOWN,
    SOFT_LEFT,
    SOFT_RIGHT
};

struct SoftKey {
    int x;
    int y;
    int w;
    int h;
    float units;
    char label[12];
    char value;
    SoftKeyType type;
};

struct KeySpec {
    const char *label;
    char value;
    SoftKeyType type;
    float units;
};

class ScreenRenderer {
public:
    Framebuffer &fb;
    Terminal &terminal;
    std::vector<SoftKey> softKeys;
    std::vector<Cell> drawnCells;
    int drawnCursorX;
    int drawnCursorY;
    bool initialized;
    int headerHeight;
    int keyboardTop;
    int terminalTop;
    int terminalBottom;
    int scale;
    int charWidth;
    int charHeight;
    bool keyboardEnabled;
    bool shift;
    bool ctrl;

    ScreenRenderer(Framebuffer &framebuffer, Terminal &term, bool keyboard)
        : fb(framebuffer), terminal(term), drawnCursorX(-1), drawnCursorY(-1),
          initialized(false), headerHeight(42), keyboardTop(framebuffer.height),
          terminalTop(42), terminalBottom(framebuffer.height), scale(kFontScale),
          charWidth(kFontWidth * kFontScale), charHeight(kFontHeight * kFontScale),
          keyboardEnabled(keyboard), shift(false), ctrl(false) {
        if (keyboardEnabled) build_keyboard();
        int availableHeight = terminalBottom - terminalTop;
        terminal.resize(fb.width / charWidth, availableHeight / charHeight);
        drawnCells.assign(terminal.cells.size(), Cell{255, 0, 0, false});
    }

    void add_row(int y, const KeySpec *specs, size_t count, float totalUnits,
                 int gap, int margin) {
        int available = fb.width - margin * 2 - gap * static_cast<int>(count - 1);
        float unitWidth = static_cast<float>(available) / totalUnits;
        int x = margin;
        for (size_t i = 0; i < count; ++i) {
            SoftKey key;
            key.x = x;
            key.y = y;
            key.w = static_cast<int>(unitWidth * specs[i].units);
            if (i == count - 1) key.w = fb.width - margin - x;
            key.h = 0;
            key.units = specs[i].units;
            snprintf(key.label, sizeof(key.label), "%s", specs[i].label);
            key.value = specs[i].value;
            key.type = specs[i].type;
            softKeys.push_back(key);
            x += key.w + gap;
        }
    }

    void build_keyboard() {
        int panelHeight = fb.height / 4;
        if (panelHeight > 330) panelHeight = 330;
        if (panelHeight < 210) panelHeight = 210;
        keyboardTop = fb.height - panelHeight;
        terminalBottom = keyboardTop - 8;
        int gap = 5;
        int margin = 10;
        int rowGap = 5;
        int rowHeight = (panelHeight - 16 - rowGap * 4) / 5;
        int y = keyboardTop + 8;

        KeySpec r0[12] = {
            {"1",'1',SOFT_CHAR,1},{"2",'2',SOFT_CHAR,1},{"3",'3',SOFT_CHAR,1},{"4",'4',SOFT_CHAR,1},
            {"5",'5',SOFT_CHAR,1},{"6",'6',SOFT_CHAR,1},{"7",'7',SOFT_CHAR,1},{"8",'8',SOFT_CHAR,1},
            {"9",'9',SOFT_CHAR,1},{"0",'0',SOFT_CHAR,1},{"-",'-',SOFT_CHAR,1},{"=",'=',SOFT_CHAR,1}
        };
        add_row(y, r0, 12, 12.0f, gap, margin);
        y += rowHeight + rowGap;

        KeySpec r1[12] = {
            {"q",'q',SOFT_CHAR,1},{"w",'w',SOFT_CHAR,1},{"e",'e',SOFT_CHAR,1},{"r",'r',SOFT_CHAR,1},
            {"t",'t',SOFT_CHAR,1},{"y",'y',SOFT_CHAR,1},{"u",'u',SOFT_CHAR,1},{"i",'i',SOFT_CHAR,1},
            {"o",'o',SOFT_CHAR,1},{"p",'p',SOFT_CHAR,1},{"[",'[',SOFT_CHAR,1},{"]",']',SOFT_CHAR,1}
        };
        add_row(y, r1, 12, 12.0f, gap, margin);
        y += rowHeight + rowGap;

        KeySpec r2[11] = {
            {"SHIFT",0,SOFT_SHIFT,1.5f},{"a",'a',SOFT_CHAR,1},{"s",'s',SOFT_CHAR,1},
            {"d",'d',SOFT_CHAR,1},{"f",'f',SOFT_CHAR,1},{"g",'g',SOFT_CHAR,1},
            {"h",'h',SOFT_CHAR,1},{"j",'j',SOFT_CHAR,1},{"k",'k',SOFT_CHAR,1},
            {"l",'l',SOFT_CHAR,1},{"BKSP",0,SOFT_BACKSPACE,1.5f}
        };
        add_row(y, r2, 11, 12.0f, gap, margin);
        y += rowHeight + rowGap;

        KeySpec r3[11] = {
            {"CTRL",0,SOFT_CTRL,1.2f},{"TAB",0,SOFT_TAB,1.0f},
            {"z",'z',SOFT_CHAR,1},{"x",'x',SOFT_CHAR,1},{"c",'c',SOFT_CHAR,1},
            {"v",'v',SOFT_CHAR,1},{"b",'b',SOFT_CHAR,1},{"n",'n',SOFT_CHAR,1},
            {"m",'m',SOFT_CHAR,1},{"/",'/',SOFT_CHAR,1},{"ENTER",0,SOFT_ENTER,1.8f}
        };
        add_row(y, r3, 11, 12.0f, gap, margin);
        y += rowHeight + rowGap;

        KeySpec r4[7] = {
            {"ESC",0,SOFT_ESCAPE,1},{"UP",0,SOFT_UP,1},{"SPACE",' ',SOFT_SPACE,5},
            {"LEFT",0,SOFT_LEFT,1},{"DOWN",0,SOFT_DOWN,1},{"RIGHT",0,SOFT_RIGHT,1},
            {".",'.',SOFT_CHAR,1}
        };
        add_row(y, r4, 7, 11.0f, gap, margin);
        for (size_t i = 0; i < softKeys.size(); ++i) softKeys[i].h = rowHeight;
    }

    void draw_terminal() {
        int cellTop = terminalTop;
        for (int y = 0; y < terminal.rows; ++y) {
            for (int x = 0; x < terminal.columns; ++x) {
                size_t index = static_cast<size_t>(y) * terminal.columns + x;
                const Cell &cell = terminal.cells[index];
                const Cell &previous = drawnCells[index];
                if (cell.ch != previous.ch || cell.fg != previous.fg ||
                    cell.bg != previous.bg || cell.bold != previous.bold) {
                    unsigned char fgIndex = cell.fg;
                    if (cell.bold && fgIndex < 8) fgIndex += 8;
                    uint32_t fg = kPalette[fgIndex & 15];
                    uint32_t bg = kPalette[cell.bg & 15];
                    draw_glyph(fb, x * charWidth, cellTop + y * charHeight,
                               cell.ch, fg, bg, scale);
                    drawnCells[index] = cell;
                }
            }
        }
        if (drawnCursorX >= 0 && drawnCursorY >= 0 &&
            (drawnCursorX != terminal.cursorX || drawnCursorY != terminal.cursorY) &&
            drawnCursorX < terminal.columns && drawnCursorY < terminal.rows) {
            size_t index = static_cast<size_t>(drawnCursorY) * terminal.columns + drawnCursorX;
            const Cell &cell = terminal.cells[index];
            unsigned char fgIndex = cell.fg;
            if (cell.bold && fgIndex < 8) fgIndex += 8;
            draw_glyph(fb, drawnCursorX * charWidth,
                       cellTop + drawnCursorY * charHeight,
                       cell.ch, kPalette[fgIndex & 15], kPalette[cell.bg & 15], scale);
            drawnCells[index] = cell;
        }
        if (terminal.cursorX >= 0 && terminal.cursorX < terminal.columns &&
            terminal.cursorY >= 0 && terminal.cursorY < terminal.rows) {
            int x = terminal.cursorX * charWidth;
            int y = cellTop + terminal.cursorY * charHeight;
            fb.fill_rect(x, y + charHeight - 3, charWidth, 3, 0xF5A623);
        }
        drawnCursorX = terminal.cursorX;
        drawnCursorY = terminal.cursorY;
    }

    void draw_keyboard() {
        if (!keyboardEnabled) return;
        fb.fill_rect(0, keyboardTop, fb.width, fb.height - keyboardTop, 0x24101F);
        for (size_t i = 0; i < softKeys.size(); ++i) {
            const SoftKey &key = softKeys[i];
            uint32_t fill = 0x594052;
            if ((key.type == SOFT_SHIFT && shift) || (key.type == SOFT_CTRL && ctrl)) fill = 0xA64D79;
            fb.fill_rect(key.x, key.y, key.w, key.h, 0xA07890);
            fb.fill_rect(key.x + 2, key.y + 2, key.w - 4, key.h - 4, fill);
            char label[12];
            snprintf(label, sizeof(label), "%s", key.label);
            if (key.type == SOFT_CHAR && shift && label[0] >= 'a' && label[0] <= 'z') {
                label[0] = static_cast<char>(label[0] - 'a' + 'A');
            }
            int textWidth = static_cast<int>(strlen(label)) * 8;
            int tx = key.x + (key.w - textWidth) / 2;
            int ty = key.y + (key.h - 16) / 2;
            draw_text(fb, tx, ty, label, 0xFFFFFF, fill, 1, key.x + key.w - 2);
        }
    }

    void render() {
        if (!initialized) {
            fb.fill_rect(0, 0, fb.width, fb.height, 0x300A24);
            fb.fill_rect(0, 0, fb.width, headerHeight, 0x5E2750);
            draw_text(fb, 14, 13, "UBUNTU ARM64  |  TAB-A05-BD  |  MT8168", 0xFFFFFF, 0x5E2750, 1, fb.width - 10);
            draw_text(fb, fb.width - 190, 13, "TOUCH / ADB", 0xF5B5D1, 0x5E2750, 1, fb.width - 5);
            initialized = true;
        }
        if (!terminal.dirty) return;
        draw_terminal();
        draw_keyboard();
        terminal.dirty = false;
    }

    bool hit_key(int x, int y, SoftKey *out) {
        for (size_t i = 0; i < softKeys.size(); ++i) {
            const SoftKey &key = softKeys[i];
            if (x >= key.x && x < key.x + key.w && y >= key.y && y < key.y + key.h) {
                if (out != NULL) *out = key;
                return true;
            }
        }
        return false;
    }
};

struct InputDevice {
    int fd;
    bool touch;
    bool keyboard;
    bool grabbed;
    int xAxis;
    int yAxis;
    input_absinfo xInfo;
    input_absinfo yInfo;
    int rawX;
    int rawY;
    bool down;
    bool pendingTap;
    char name[128];
    char path[PATH_MAX];
    InputDevice() : fd(-1), touch(false), keyboard(false), grabbed(false),
                    xAxis(-1), yAxis(-1), rawX(0), rawY(0), down(false), pendingTap(false) {
        memset(&xInfo, 0, sizeof(xInfo));
        memset(&yInfo, 0, sizeof(yInfo));
        memset(name, 0, sizeof(name));
        memset(path, 0, sizeof(path));
    }
};

static bool query_bits(int fd, int eventType, unsigned long *bits, size_t bytes) {
    memset(bits, 0, bytes);
    return ioctl(fd, EVIOCGBIT(eventType, bytes), bits) >= 0;
}

static bool query_axis(int fd, int axis, input_absinfo *info) {
    memset(info, 0, sizeof(*info));
    return ioctl(fd, EVIOCGABS(axis), info) == 0 && info->maximum > info->minimum;
}

static void close_inputs(std::vector<InputDevice> &devices) {
    for (size_t i = 0; i < devices.size(); ++i) {
        if (devices[i].fd >= 0) {
            if (devices[i].grabbed) ioctl(devices[i].fd, EVIOCGRAB, 0);
            close(devices[i].fd);
            devices[i].fd = -1;
        }
    }
    devices.clear();
}

static bool open_input_devices(const char *directory, std::vector<InputDevice> &devices,
                               bool grabDevices) {
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        print_error("opendir input", directory);
        return false;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >= static_cast<int>(sizeof(path))) continue;
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        unsigned long evBits[UB_NBITS(EV_MAX + 1)];
        unsigned long absBits[UB_NBITS(ABS_MAX + 1)];
        unsigned long keyBits[UB_NBITS(KEY_MAX + 1)];
        if (!query_bits(fd, 0, evBits, sizeof(evBits))) {
            close(fd);
            continue;
        }
        bool hasAbs = UB_TEST_BIT(EV_ABS, evBits) != 0;
        bool hasKey = UB_TEST_BIT(EV_KEY, evBits) != 0;
        bool touch = false;
        bool keyboard = false;
        int xAxis = -1;
        int yAxis = -1;
        input_absinfo xInfo;
        input_absinfo yInfo;
        memset(&xInfo, 0, sizeof(xInfo));
        memset(&yInfo, 0, sizeof(yInfo));

        if (hasAbs && query_bits(fd, EV_ABS, absBits, sizeof(absBits))) {
            if (UB_TEST_BIT(ABS_MT_POSITION_X, absBits) &&
                UB_TEST_BIT(ABS_MT_POSITION_Y, absBits)) {
                xAxis = ABS_MT_POSITION_X;
                yAxis = ABS_MT_POSITION_Y;
            } else if (UB_TEST_BIT(ABS_X, absBits) && UB_TEST_BIT(ABS_Y, absBits)) {
                xAxis = ABS_X;
                yAxis = ABS_Y;
            }
            if (xAxis >= 0 && query_axis(fd, xAxis, &xInfo) && query_axis(fd, yAxis, &yInfo)) {
                bool keyTouch = false;
                if (hasKey && query_bits(fd, EV_KEY, keyBits, sizeof(keyBits))) {
                    keyTouch = UB_TEST_BIT(BTN_TOUCH, keyBits) != 0;
                }
                bool tracking = UB_TEST_BIT(ABS_MT_TRACKING_ID, absBits) != 0;
                touch = keyTouch || tracking;
            }
        }

        if (hasKey && query_bits(fd, EV_KEY, keyBits, sizeof(keyBits))) {
            keyboard = UB_TEST_BIT(KEY_A, keyBits) &&
                       UB_TEST_BIT(KEY_Z, keyBits) &&
                       UB_TEST_BIT(KEY_ENTER, keyBits) &&
                       !touch;
        }
        if (!touch && !keyboard) {
            close(fd);
            continue;
        }

        InputDevice device;
        device.fd = fd;
        device.touch = touch;
        device.keyboard = keyboard;
        device.xAxis = xAxis;
        device.yAxis = yAxis;
        device.xInfo = xInfo;
        device.yInfo = yInfo;
        ioctl(fd, EVIOCGNAME(sizeof(device.name)), device.name);
        device.name[sizeof(device.name) - 1] = '\0';
        snprintf(device.path, sizeof(device.path), "%s", path);
        if (grabDevices) {
            if (ioctl(fd, EVIOCGRAB, 1) == 0) {
                device.grabbed = true;
            } else {
                print_error("EVIOCGRAB input", path);
                close(fd);
                continue;
            }
        }
        devices.push_back(device);
        fprintf(stderr, "[ubuntu-loader] Input: %s name=\"%s\" (%s%s)",
                path, device.name,
                touch ? "touch" : "",
                keyboard ? "keyboard" : "");
        if (touch) {
            fprintf(stderr, " x=%d..%d y=%d..%d", xInfo.minimum,
                    xInfo.maximum, yInfo.minimum, yInfo.maximum);
        }
        fprintf(stderr, " grab=%s\n", device.grabbed ? "yes" : "no");
    }
    closedir(dir);
    return true;
}

static int scale_axis(int value, const input_absinfo &info) {
    if (info.maximum <= info.minimum) return 0;
    if (value < info.minimum) value = info.minimum;
    if (value > info.maximum) value = info.maximum;
    return static_cast<int>((static_cast<int64_t>(value - info.minimum) * 1000000) /
                            (info.maximum - info.minimum));
}

struct TouchTransform {
    bool swapXY;
    bool flipX;
    bool flipY;
    TouchTransform() : swapXY(false), flipX(false), flipY(false) {}
};

static void map_touch(const InputDevice &device, const TouchTransform &transform,
                      int width, int height, int *screenX, int *screenY) {
    double nx = static_cast<double>(scale_axis(device.rawX, device.xInfo)) / 1000000.0;
    double ny = static_cast<double>(scale_axis(device.rawY, device.yInfo)) / 1000000.0;
    if (transform.swapXY) {
        double temp = nx;
        nx = ny;
        ny = temp;
    }
    if (transform.flipX) nx = 1.0 - nx;
    if (transform.flipY) ny = 1.0 - ny;
    int x = static_cast<int>(nx * width);
    int y = static_cast<int>(ny * height);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= width) x = width - 1;
    if (y >= height) y = height - 1;
    *screenX = x;
    *screenY = y;
}

static bool write_all(int fd, const void *data, size_t size) {
    const unsigned char *p = static_cast<const unsigned char *>(data);
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, p + done, size - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            return false;
        }
        if (n == 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

static void send_key_bytes(int master, const char *bytes, size_t size) {
    if (master >= 0) write_all(master, bytes, size);
}

struct KeyboardState {
    bool shift;
    bool ctrl;
    bool caps;
    KeyboardState() : shift(false), ctrl(false), caps(false) {}
};

static void send_character(int master, char value, KeyboardState &state,
                           bool consumeModifiers) {
    if (value >= 'a' && value <= 'z') {
        bool uppercase = state.shift != state.caps;
        if (uppercase) value = static_cast<char>(value - 'a' + 'A');
    } else if (state.shift) {
        switch (value) {
            case '1': value = '!'; break; case '2': value = '@'; break;
            case '3': value = '#'; break; case '4': value = '$'; break;
            case '5': value = '%'; break; case '6': value = '^'; break;
            case '7': value = '&'; break; case '8': value = '*'; break;
            case '9': value = '('; break; case '0': value = ')'; break;
            case '-': value = '_'; break; case '=': value = '+'; break;
            case '[': value = '{'; break; case ']': value = '}'; break;
            case ';': value = ':'; break; case '\'': value = '"'; break;
            case ',': value = '<'; break; case '.': value = '>'; break;
            case '/': value = '?'; break; case '`': value = '~'; break;
            case '\\': value = '|'; break;
        }
    }
    if (state.ctrl && value >= 'a' && value <= 'z') {
        char control = static_cast<char>(value - 'a' + 1);
        send_key_bytes(master, &control, 1);
    } else if (state.ctrl && value >= 'A' && value <= 'Z') {
        char control = static_cast<char>(value - 'A' + 1);
        send_key_bytes(master, &control, 1);
    } else {
        send_key_bytes(master, &value, 1);
    }
    if (consumeModifiers) {
        state.shift = false;
        state.ctrl = false;
    }
}

static void handle_soft_key(const SoftKey &key, int master,
                            KeyboardState &state, ScreenRenderer &renderer) {
    static const char up[] = "\033[A";
    static const char down[] = "\033[B";
    static const char left[] = "\033[D";
    static const char right[] = "\033[C";
    switch (key.type) {
        case SOFT_CHAR: send_character(master, key.value, state, true); break;
        case SOFT_SHIFT: state.shift = !state.shift; renderer.shift = state.shift; break;
        case SOFT_CTRL: state.ctrl = !state.ctrl; renderer.ctrl = state.ctrl; break;
        case SOFT_TAB: { char c = '\t'; send_key_bytes(master, &c, 1); state.shift = false; state.ctrl = false; break; }
        case SOFT_ENTER: { char c = '\r'; send_key_bytes(master, &c, 1); state.shift = false; state.ctrl = false; break; }
        case SOFT_BACKSPACE: { char c = 0x7f; send_key_bytes(master, &c, 1); break; }
        case SOFT_SPACE: send_character(master, ' ', state, true); break;
        case SOFT_ESCAPE: { char c = 0x1b; send_key_bytes(master, &c, 1); break; }
        case SOFT_UP: send_key_bytes(master, up, sizeof(up) - 1); break;
        case SOFT_DOWN: send_key_bytes(master, down, sizeof(down) - 1); break;
        case SOFT_LEFT: send_key_bytes(master, left, sizeof(left) - 1); break;
        case SOFT_RIGHT: send_key_bytes(master, right, sizeof(right) - 1); break;
    }
    renderer.terminal.dirty = true;
}

static void handle_physical_key(unsigned int code, int value, int master,
                                KeyboardState &state) {
    if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) { state.shift = value != 0; return; }
    if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) { state.ctrl = value != 0; return; }
    if (code == KEY_CAPSLOCK && value == 1) { state.caps = !state.caps; return; }
    if (value == 0) return;
    char letter = 0;
    switch (code) {
        case KEY_A: letter = 'a'; break; case KEY_B: letter = 'b'; break;
        case KEY_C: letter = 'c'; break; case KEY_D: letter = 'd'; break;
        case KEY_E: letter = 'e'; break; case KEY_F: letter = 'f'; break;
        case KEY_G: letter = 'g'; break; case KEY_H: letter = 'h'; break;
        case KEY_I: letter = 'i'; break; case KEY_J: letter = 'j'; break;
        case KEY_K: letter = 'k'; break; case KEY_L: letter = 'l'; break;
        case KEY_M: letter = 'm'; break; case KEY_N: letter = 'n'; break;
        case KEY_O: letter = 'o'; break; case KEY_P: letter = 'p'; break;
        case KEY_Q: letter = 'q'; break; case KEY_R: letter = 'r'; break;
        case KEY_S: letter = 's'; break; case KEY_T: letter = 't'; break;
        case KEY_U: letter = 'u'; break; case KEY_V: letter = 'v'; break;
        case KEY_W: letter = 'w'; break; case KEY_X: letter = 'x'; break;
        case KEY_Y: letter = 'y'; break; case KEY_Z: letter = 'z'; break;
        default: break;
    }
    if (letter != 0) {
        send_character(master, letter, state, false);
        return;
    }
    if (code >= KEY_1 && code <= KEY_9) {
        send_character(master, static_cast<char>('1' + code - KEY_1), state, false);
        return;
    }
    switch (code) {
        case KEY_0: send_character(master, '0', state, false); break;
        case KEY_SPACE: send_character(master, ' ', state, false); break;
        case KEY_ENTER: { char c = '\r'; send_key_bytes(master, &c, 1); break; }
        case KEY_KPENTER: { char c = '\r'; send_key_bytes(master, &c, 1); break; }
        case KEY_TAB: { char c = '\t'; send_key_bytes(master, &c, 1); break; }
        case KEY_BACKSPACE: { char c = 0x7f; send_key_bytes(master, &c, 1); break; }
        case KEY_ESC: { char c = 0x1b; send_key_bytes(master, &c, 1); break; }
        case KEY_UP: send_key_bytes(master, "\033[A", 3); break;
        case KEY_DOWN: send_key_bytes(master, "\033[B", 3); break;
        case KEY_LEFT: send_key_bytes(master, "\033[D", 3); break;
        case KEY_RIGHT: send_key_bytes(master, "\033[C", 3); break;
        case KEY_MINUS: send_character(master, '-', state, false); break;
        case KEY_EQUAL: send_character(master, '=', state, false); break;
        case KEY_LEFTBRACE: send_character(master, '[', state, false); break;
        case KEY_RIGHTBRACE: send_character(master, ']', state, false); break;
        case KEY_SEMICOLON: send_character(master, ';', state, false); break;
        case KEY_APOSTROPHE: send_character(master, '\'', state, false); break;
        case KEY_COMMA: send_character(master, ',', state, false); break;
        case KEY_DOT: send_character(master, '.', state, false); break;
        case KEY_SLASH: send_character(master, '/', state, false); break;
        case KEY_BACKSLASH: send_character(master, '\\', state, false); break;
        case KEY_GRAVE: send_character(master, '`', state, false); break;
        default: break;
    }
}

static void handle_touch_tap(int x, int y, int master,
                             KeyboardState &state, ScreenRenderer &renderer) {
    SoftKey key;
    if (renderer.hit_key(x, y, &key)) {
        handle_soft_key(key, master, state, renderer);
    }
}

static void process_input_event(InputDevice &device, const input_event &event,
                                int master, KeyboardState &keyboardState,
                                ScreenRenderer &renderer,
                                const TouchTransform &transform,
                                const Framebuffer &fb) {
    if (device.keyboard && event.type == EV_KEY) {
        handle_physical_key(event.code, event.value, master, keyboardState);
    }
    if (!device.touch) return;
    if (event.type == EV_ABS) {
        if (event.code == device.xAxis) device.rawX = event.value;
        if (event.code == device.yAxis) device.rawY = event.value;
        if (event.code == ABS_MT_TRACKING_ID) {
            device.down = event.value >= 0;
            if (device.down) device.pendingTap = true;
        }
    } else if (event.type == EV_KEY &&
               event.code == BTN_TOUCH) {
        bool nextDown = event.value != 0;
        if (nextDown && !device.down) device.pendingTap = true;
        device.down = nextDown;
    } else if (event.type == EV_SYN && event.code == SYN_REPORT) {
        if (device.pendingTap && device.down) {
            int x = 0;
            int y = 0;
            map_touch(device, transform, fb.width, fb.height, &x, &y);
            handle_touch_tap(x, y, master, keyboardState, renderer);
            device.pendingTap = false;
        }
    }
}

static bool create_pty(int *master, int *slave, int columns, int rows) {
    int m = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (m < 0) {
        print_error("posix_openpt", NULL);
        return false;
    }
    if (grantpt(m) != 0 || unlockpt(m) != 0) {
        print_error("prepare PTY", NULL);
        close(m);
        return false;
    }
    char *name = ptsname(m);
    if (name == NULL) {
        print_error("ptsname", NULL);
        close(m);
        return false;
    }
    int s = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (s < 0) {
        print_error("open PTY slave", name);
        close(m);
        return false;
    }
    struct winsize size;
    memset(&size, 0, sizeof(size));
    size.ws_col = static_cast<unsigned short>(columns > 65535 ? 65535 : columns);
    size.ws_row = static_cast<unsigned short>(rows > 65535 ? 65535 : rows);
    if (ioctl(s, TIOCSWINSZ, &size) != 0) {
        print_error("TIOCSWINSZ", name);
        close(s);
        close(m);
        return false;
    }
    *master = m;
    *slave = s;
    return true;
}

static bool set_parent_stdin_raw(struct termios *saved, bool *changed) {
    *changed = false;
    if (!isatty(STDIN_FILENO)) return true;
    if (tcgetattr(STDIN_FILENO, saved) != 0) return false;
    struct termios raw = *saved;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return false;
    *changed = true;
    return true;
}

static void restore_parent_stdin(const struct termios *saved, bool changed) {
    if (changed) tcsetattr(STDIN_FILENO, TCSANOW, saved);
}

static int probe_display(const char *fbPath, const char *inputPath) {
    Framebuffer fb;
    if (!fb.open_device(fbPath, false)) {
        fb.close_device();
        return 1;
    }
    printf("[ubuntu-loader] Framebuffer device: %s\n", fbPath);
    printf("[ubuntu-loader] Driver: %.*s type=%u visual=%u accel=%u\n",
           static_cast<int>(sizeof(fb.fixed.id)), fb.fixed.id,
           fb.fixed.type, fb.fixed.visual, fb.fixed.accel);
    printf("[ubuntu-loader] Visible size: %d x %d\n", fb.width, fb.height);
    printf("[ubuntu-loader] Format: %u bpp, line_length=%u, smem_len=%u\n",
           fb.variable.bits_per_pixel, fb.fixed.line_length, fb.fixed.smem_len);
    printf("[ubuntu-loader] Offset: x=%u y=%u, virtual=%u x %u, rotate=%u\n",
           fb.variable.xoffset, fb.variable.yoffset,
           fb.variable.xres_virtual, fb.variable.yres_virtual,
           fb.variable.rotate);
    printf("[ubuntu-loader] RGB bitfields: R=%u:%u G=%u:%u B=%u:%u A=%u:%u\n",
           fb.variable.red.offset, fb.variable.red.length,
           fb.variable.green.offset, fb.variable.green.length,
           fb.variable.blue.offset, fb.variable.blue.length,
           fb.variable.transp.offset, fb.variable.transp.length);
    fb.close_device();

    std::vector<InputDevice> devices;
    if (!open_input_devices(inputPath, devices, false)) {
        fprintf(stderr, "[ubuntu-loader] Input probe unavailable; framebuffer probe completed\n");
        return 0;
    }
    bool hasTouch = false;
    bool hasKeyboard = false;
    for (size_t i = 0; i < devices.size(); ++i) {
        if (devices[i].touch) hasTouch = true;
        if (devices[i].keyboard) hasKeyboard = true;
    }
    printf("[ubuntu-loader] Touchscreen: %s\n", hasTouch ? "detected" : "not detected");
    printf("[ubuntu-loader] Physical keyboard: %s\n", hasKeyboard ? "detected" : "not detected");
    close_inputs(devices);
    return 0;
}

static bool has_open_input_device(const std::vector<InputDevice> &devices) {
    for (size_t i = 0; i < devices.size(); ++i) {
        if (devices[i].fd >= 0 && (devices[i].touch || devices[i].keyboard)) {
            return true;
        }
    }
    return false;
}

static void disconnect_input_device(InputDevice &device) {
    if (device.fd >= 0) {
        if (device.grabbed) ioctl(device.fd, EVIOCGRAB, 0);
        close(device.fd);
        device.fd = -1;
    }
    device.grabbed = false;
    device.down = false;
    device.pendingTap = false;
}

static int run_framebuffer_session(const char *root, const char *shell,
                                   int argc, char **argv, int commandIndex,
                                   const char *fbPath, const char *inputPath,
                                   pid_t recoveryPid, const TouchTransform &transform,
                                   int screenRotation, bool showKeyboard) {
    if (recoveryPid <= 1) {
        fprintf(stderr, "[ubuntu-loader] Framebuffer takeover requires --freeze-pid PID\n");
        fprintf(stderr, "[ubuntu-loader] This prevents TWRP and Ubuntu from drawing to fb0 at the same time\n");
        return 2;
    }
    if (!install_signal_handlers()) return 1;
    if (!setup_mounts(root)) {
        cleanup_mounts(root);
        return 1;
    }

    Framebuffer fb;
    if (!fb.open_device(fbPath)) {
        fb.close_device();
        cleanup_mounts(root);
        return 1;
    }
    if (!fb.set_rotation(screenRotation)) {
        fb.close_device();
        cleanup_mounts(root);
        return 2;
    }

    std::vector<InputDevice> inputs;
    open_input_devices(inputPath, inputs, true);
    bool hasTouch = false;
    bool hasKeyboard = false;
    for (size_t i = 0; i < inputs.size(); ++i) {
        if (inputs[i].touch) hasTouch = true;
        if (inputs[i].keyboard) hasKeyboard = true;
    }
    bool useSoftKeyboard = showKeyboard && hasTouch;
    bool adbInputAvailable = isatty(STDIN_FILENO) != 0;
    if (!adbInputAvailable && !hasKeyboard && !useSoftKeyboard) {
        fprintf(stderr,
                "[ubuntu-loader] No usable input source for framebuffer mode\n"
                "[ubuntu-loader] Use adb shell -tt, a grabbed touchscreen, or a grabbed USB keyboard\n");
        close_inputs(inputs);
        fb.close_device();
        cleanup_mounts(root);
        return 2;
    }

    Terminal terminal;
    ScreenRenderer renderer(fb, terminal, useSoftKeyboard);
    int ptyMaster = -1;
    int ptySlave = -1;
    if (!create_pty(&ptyMaster, &ptySlave, terminal.columns, terminal.rows)) {
        close_inputs(inputs);
        fb.close_device();
        cleanup_mounts(root);
        return 1;
    }

    if (!freeze_recovery(recoveryPid)) {
        close(ptySlave);
        close(ptyMaster);
        close_inputs(inputs);
        fb.close_device();
        cleanup_mounts(root);
        return 1;
    }

    if (!fb.snapshot()) {
        fprintf(stderr, "[ubuntu-loader] Cannot preserve original TWRP framebuffer\n");
        kill(recoveryPid, SIGCONT);
        close(ptySlave);
        close(ptyMaster);
        close_inputs(inputs);
        fb.close_device();
        cleanup_mounts(root);
        return 1;
    }

    pid_t watchdog = start_recovery_watchdog(recoveryPid, fb.memory,
                                             fb.backup, fb.length);
    if (watchdog < 0) {
        fb.restore();
        kill(recoveryPid, SIGCONT);
        close(ptySlave);
        close(ptyMaster);
        close_inputs(inputs);
        fb.close_device();
        cleanup_mounts(root);
        return 1;
    }

    fb.fill_rect(0, 0, fb.width, fb.height, 0x300A24);
    terminal.dirty = true;
    renderer.render();

    pid_t loaderPid = getpid();
    pid_t child = fork();
    if (child < 0) {
        print_error("fork Ubuntu PTY", NULL);
        fb.restore();
        kill(recoveryPid, SIGCONT);
        stop_watchdog(watchdog);
        close(ptySlave);
        close(ptyMaster);
        close_inputs(inputs);
        fb.close_device();
        cleanup_mounts(root);
        return 1;
    }
    if (child == 0) {
        close(ptyMaster);
        for (size_t i = 0; i < inputs.size(); ++i) {
            if (inputs[i].fd >= 0) close(inputs[i].fd);
        }
        munmap(fb.memory, fb.length);
        close(fb.fd);
        if (setsid() < 0) _exit(126);
        if (ioctl(ptySlave, TIOCSCTTY, 0) != 0) _exit(126);
        dup2(ptySlave, STDIN_FILENO);
        dup2(ptySlave, STDOUT_FILENO);
        dup2(ptySlave, STDERR_FILENO);
        if (ptySlave > STDERR_FILENO) close(ptySlave);
        tcsetpgrp(STDIN_FILENO, getpid());
        child_exec(root, shell, argc, argv, commandIndex, "ansi", true, loaderPid);
        _exit(126);
    }

    close(ptySlave);
    gChildPid = static_cast<sig_atomic_t>(child);

    struct termios oldTermios;
    bool stdinRaw = false;
    if (!set_parent_stdin_raw(&oldTermios, &stdinRaw)) {
        print_error("set ADB input terminal to raw mode", NULL);
        kill(-child, SIGTERM);
        kill(child, SIGTERM);
        int terminalStatus = 0;
        while (waitpid(child, &terminalStatus, 0) < 0 && errno == EINTR) {}
        fb.restore();
        close_inputs(inputs);
        close(ptyMaster);
        fb.close_device();
        cleanup_mounts(root);
        kill(recoveryPid, SIGCONT);
        stop_watchdog(watchdog);
        return 1;
    }

    fprintf(stderr, "[ubuntu-loader] Framebuffer: %dx%d %ubpp line=%u\n",
            fb.width, fb.height, fb.variable.bits_per_pixel, fb.fixed.line_length);
    fprintf(stderr, "[ubuntu-loader] Touch: %s, keyboard: %s, soft keyboard: %s\n",
            hasTouch ? "yes" : "no", hasKeyboard ? "yes" : "no",
            useSoftKeyboard ? "yes" : "no");
    fprintf(stderr, "[ubuntu-loader] TWRP recovery PID %ld is paused\n",
            static_cast<long>(recoveryPid));

    KeyboardState keyboardState;
    bool ptyOpen = true;
    bool stdinOpen = isatty(STDIN_FILENO) != 0;
    bool childExited = false;
    int childStatus = 0;
    bool mirrorOutput = isatty(STDOUT_FILENO) != 0;
    unsigned char buffer[8192];

    while (!childExited) {
        std::vector<pollfd> pollFds;
        std::vector<int> inputIndex;
        pollfd masterPoll;
        masterPoll.fd = ptyMaster;
        masterPoll.events = ptyOpen ? POLLIN | POLLHUP | POLLERR : 0;
        masterPoll.revents = 0;
        pollFds.push_back(masterPoll);
        inputIndex.push_back(-1);

        if (stdinOpen) {
            pollfd inputPoll;
            inputPoll.fd = STDIN_FILENO;
            inputPoll.events = POLLIN | POLLHUP | POLLERR;
            inputPoll.revents = 0;
            pollFds.push_back(inputPoll);
            inputIndex.push_back(-2);
        }

        for (size_t i = 0; i < inputs.size(); ++i) {
            if (inputs[i].fd < 0) continue;
            pollfd inputPoll;
            inputPoll.fd = inputs[i].fd;
            inputPoll.events = POLLIN | POLLHUP | POLLERR;
            inputPoll.revents = 0;
            pollFds.push_back(inputPoll);
            inputIndex.push_back(static_cast<int>(i));
        }

        int pollResult = poll(pollFds.data(), pollFds.size(), 100);
        if (pollResult < 0 && errno != EINTR) {
            print_error("poll framebuffer input", NULL);
            break;
        }

        if (pollResult > 0) {
            for (size_t i = 0; i < pollFds.size(); ++i) {
                if (pollFds[i].revents == 0) continue;
                int source = inputIndex[i];
                if (source == -1) {
                    if ((pollFds[i].revents & POLLIN) != 0) {
                        ssize_t n = read(ptyMaster, buffer, sizeof(buffer));
                        if (n > 0) {
                            terminal.feed(buffer, static_cast<size_t>(n));
                            if (mirrorOutput) write_all(STDOUT_FILENO, buffer, static_cast<size_t>(n));
                        } else if (n == 0 || (n < 0 && errno == EIO)) {
                            ptyOpen = false;
                        } else if (n < 0 && errno != EINTR && errno != EAGAIN) {
                            ptyOpen = false;
                        }
                    }
                    if ((pollFds[i].revents & (POLLHUP | POLLERR)) != 0) ptyOpen = false;
                } else if (source == -2) {
                    if ((pollFds[i].revents & POLLIN) != 0) {
                        ssize_t n = read(STDIN_FILENO, buffer, sizeof(buffer));
                        if (n > 0) write_all(ptyMaster, buffer, static_cast<size_t>(n));
                        else if (n == 0) stdinOpen = false;
                    }
                    if ((pollFds[i].revents & (POLLHUP | POLLERR)) != 0) stdinOpen = false;
                } else if (source >= 0) {
                    InputDevice &device = inputs[static_cast<size_t>(source)];
                    bool disconnected =
                        (pollFds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
                    if (!disconnected && (pollFds[i].revents & POLLIN) != 0) {
                        input_event event;
                        for (;;) {
                            ssize_t count = read(device.fd, &event, sizeof(event));
                            if (count == static_cast<ssize_t>(sizeof(event))) {
                                process_input_event(device, event, ptyMaster,
                                                    keyboardState, renderer, transform, fb);
                                continue;
                            }
                            if (count < 0 && errno == EINTR) continue;
                            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                            disconnected = true;
                            break;
                        }
                    }
                    if (disconnected) {
                        fprintf(stderr, "[ubuntu-loader] Input device disconnected: %s\n",
                                device.name[0] == '\0' ? "unknown" : device.name);
                        disconnect_input_device(device);
                    }
                }
            }
        }

        if (gCaughtSignal != 0) {
            kill(-child, SIGTERM);
            kill(child, SIGTERM);
        }

        if (!stdinOpen && !has_open_input_device(inputs)) {
            fprintf(stderr,
                    "[ubuntu-loader] All input sources disconnected; stopping Ubuntu session\n");
            kill(-child, SIGTERM);
            kill(child, SIGTERM);
        }

        pid_t waitResult = waitpid(child, &childStatus, WNOHANG);
        if (waitResult == child) childExited = true;
        else if (waitResult < 0 && errno != EINTR) childExited = true;

        renderer.render();
        if (!ptyOpen && waitResult == child) childExited = true;
    }

    if (!childExited) {
        kill(-child, SIGTERM);
        kill(child, SIGTERM);
        while (waitpid(child, &childStatus, 0) < 0 && errno == EINTR) {}
    }
    gChildPid = -1;
    restore_parent_stdin(&oldTermios, stdinRaw);

    fb.restore();
    close_inputs(inputs);
    close(ptyMaster);
    fb.close_device();
    bool mountCleanup = cleanup_mounts(root);

    kill(recoveryPid, SIGCONT);
    stop_watchdog(watchdog);
    fprintf(stderr, "[ubuntu-loader] TWRP framebuffer restored and recovery resumed\n");

    if (!mountCleanup) {
        fprintf(stderr, "[ubuntu-loader] WARNING: private mount cleanup incomplete\n");
        return 1;
    }
    if (gCaughtSignal != 0) return 128 + gCaughtSignal;
    if (WIFEXITED(childStatus)) return WEXITSTATUS(childStatus);
    if (WIFSIGNALED(childStatus)) return 128 + WTERMSIG(childStatus);
    return 1;
}

static bool write_u16_be(int fd, uint16_t value) {
    unsigned char bytes[2];
    bytes[0] = static_cast<unsigned char>((value >> 8) & 0xff);
    bytes[1] = static_cast<unsigned char>(value & 0xff);
    return write_all(fd, bytes, sizeof(bytes));
}

static bool write_auth_field(int fd, const void *data, size_t length) {
    if (length > 65535 || !write_u16_be(fd, static_cast<uint16_t>(length))) {
        return false;
    }
    return length == 0 || write_all(fd, data, length);
}

static bool create_xauthority(const char *root, char *absolutePath,
                              size_t absolutePathSize) {
    char path[PATH_MAX];
    if (!make_path(path, sizeof(path), root, "tmp/ubuntu-loader.Xauthority")) {
        fprintf(stderr, "[ubuntu-loader] Xauthority path too long\n");
        return false;
    }
    char hostname[256];
    memset(hostname, 0, sizeof(hostname));
    if (gethostname(hostname, sizeof(hostname) - 1) != 0) {
        print_error("gethostname", NULL);
        return false;
    }
    int randomFd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (randomFd < 0) {
        print_error("open /dev/urandom", NULL);
        return false;
    }
    unsigned char cookie[16];
    size_t randomRead = 0;
    while (randomRead < sizeof(cookie)) {
        ssize_t count = read(randomFd, cookie + randomRead,
                             sizeof(cookie) - randomRead);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            close(randomFd);
            fprintf(stderr, "[ubuntu-loader] Cannot generate Xauthority cookie\n");
            return false;
        }
        randomRead += static_cast<size_t>(count);
    }
    close(randomFd);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        print_error("create Xauthority", path);
        return false;
    }
    const char display[] = "0";
    const char mechanism[] = "MIT-MAGIC-COOKIE-1";
    bool ok = write_u16_be(fd, 256) &&
              write_auth_field(fd, hostname, strlen(hostname)) &&
              write_auth_field(fd, display, sizeof(display) - 1) &&
              write_auth_field(fd, mechanism, sizeof(mechanism) - 1) &&
              write_auth_field(fd, cookie, sizeof(cookie));
    if (fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    memset(cookie, 0, sizeof(cookie));
    if (!ok) {
        unlink(path);
        fprintf(stderr, "[ubuntu-loader] Cannot write Xauthority file\n");
        return false;
    }
    if (chmod(path, 0600) != 0) {
        print_error("chmod Xauthority", path);
        unlink(path);
        return false;
    }
    if (strlen(path) + 1 > absolutePathSize) {
        unlink(path);
        fprintf(stderr, "[ubuntu-loader] Xauthority path buffer too small\n");
        return false;
    }
    strcpy(absolutePath, path);
    return true;
}

static bool write_xorg_config(const char *root,
                              const std::vector<InputDevice> &inputs,
                              const TouchTransform &transform,
                              const Framebuffer &fb) {
    char configPath[PATH_MAX];
    if (!make_path(configPath, sizeof(configPath), root,
                   "tmp/ubuntu-loader-xorg.conf")) {
        fprintf(stderr, "[ubuntu-loader] Xorg config path too long\n");
        return false;
    }
    FILE *file = fopen(configPath, "w");
    if (file == NULL) {
        print_error("create Xorg config", configPath);
        return false;
    }
    int depth = fb.variable.bits_per_pixel == 16 ? 16 : 24;
    bool ok = true;
    ok = ok && fprintf(file,
        "Section \"ServerFlags\"\n"
        "    Option \"AutoAddDevices\" \"false\"\n"
        "    Option \"AutoEnableDevices\" \"false\"\n"
        "    Option \"AllowMouseOpenFail\" \"true\"\n"
        "    Option \"DontVTSwitch\" \"true\"\n"
        "    Option \"DontZap\" \"true\"\n"
        "    Option \"BlankTime\" \"0\"\n"
        "    Option \"StandbyTime\" \"0\"\n"
        "    Option \"SuspendTime\" \"0\"\n"
        "    Option \"OffTime\" \"0\"\n"
        "EndSection\n\n"
        "Section \"Device\"\n"
        "    Identifier \"MTKFramebuffer\"\n"
        "    Driver \"fbdev\"\n"
        "    Option \"fbdev\" \"%s\"\n"
        "    Option \"ShadowFB\" \"true\"\n"
        "EndSection\n\n"
        "Section \"Monitor\"\n"
        "    Identifier \"MTKPanel\"\n"
        "EndSection\n\n"
        "Section \"Screen\"\n"
        "    Identifier \"UbuntuScreen\"\n"
        "    Device \"MTKFramebuffer\"\n"
        "    Monitor \"MTKPanel\"\n"
        "    DefaultDepth %d\n"
        "EndSection\n\n"
        "Section \"ServerLayout\"\n"
        "    Identifier \"UbuntuRecoveryLayout\"\n"
        "    Screen \"UbuntuScreen\"\n",
        fb.devicePath, depth) > 0;

    bool corePointerWritten = false;
    bool coreKeyboardWritten = false;
    for (size_t i = 0; ok && i < inputs.size(); ++i) {
        const InputDevice &device = inputs[i];
        if (device.fd < 0 || (!device.touch && !device.keyboard)) continue;
        char identifier[32];
        snprintf(identifier, sizeof(identifier), "InputDevice%lu",
                 static_cast<unsigned long>(i));
        if (fprintf(file,
            "    InputDevice \"%s\" \"%s\"\n",
            identifier,
            device.touch && !corePointerWritten ? "CorePointer" :
            device.keyboard && !coreKeyboardWritten ? "CoreKeyboard" :
            "SendCoreEvents") < 0) {
            ok = false;
            break;
        }
        if (device.touch && !corePointerWritten) corePointerWritten = true;
        if (device.keyboard && !coreKeyboardWritten) coreKeyboardWritten = true;
    }
    ok = ok && fprintf(file, "EndSection\n\n") > 0;

    for (size_t i = 0; ok && i < inputs.size(); ++i) {
        const InputDevice &device = inputs[i];
        if (device.fd < 0 || (!device.touch && !device.keyboard)) continue;
        char identifier[32];
        snprintf(identifier, sizeof(identifier), "InputDevice%lu",
                 static_cast<unsigned long>(i));
        ok = fprintf(file,
            "Section \"InputDevice\"\n"
            "    Identifier \"%s\"\n"
            "    Driver \"evdev\"\n"
            "    Option \"Device\" \"%s\"\n"
            "    Option \"GrabDevice\" \"true\"\n",
            identifier, device.path) > 0;
        if (ok && device.touch) {
            ok = fprintf(file,
                "    Option \"Calibration\" \"%d %d %d %d\"\n",
                device.xInfo.minimum, device.xInfo.maximum,
                device.yInfo.minimum, device.yInfo.maximum) > 0;
            if (ok && transform.swapXY)
                ok = fprintf(file, "    Option \"SwapAxes\" \"true\"\n") > 0;
            if (ok && transform.flipX)
                ok = fprintf(file, "    Option \"InvertX\" \"true\"\n") > 0;
            if (ok && transform.flipY)
                ok = fprintf(file, "    Option \"InvertY\" \"true\"\n") > 0;
        }
        ok = ok && fprintf(file, "EndSection\n\n") > 0;
    }
    if (ferror(file)) ok = false;
    if (fclose(file) != 0) ok = false;
    if (!ok) {
        unlink(configPath);
        fprintf(stderr, "[ubuntu-loader] Cannot generate Xorg configuration\n");
        return false;
    }
    if (chmod(configPath, 0644) != 0) {
        print_error("chmod Xorg config", configPath);
        unlink(configPath);
        return false;
    }
    return true;
}

static bool desktop_dependencies(const char *root, const char *desktopSession,
                                 char *xorgBinary, size_t xorgBinarySize) {
    const char *xorgCandidates[] = {
        "/usr/lib/xorg/Xorg",
        "/usr/bin/Xorg",
        "/usr/bin/X"
    };
    xorgBinary[0] = '\0';
    for (size_t i = 0; i < sizeof(xorgCandidates) / sizeof(xorgCandidates[0]); ++i) {
        char path[PATH_MAX];
        if (!make_path(path, sizeof(path), root, xorgCandidates[i])) continue;
        if (access(path, X_OK) == 0) {
            if (strlen(xorgCandidates[i]) + 1 > xorgBinarySize) return false;
            strcpy(xorgBinary, xorgCandidates[i]);
            break;
        }
    }
    if (xorgBinary[0] == '\0') {
        fprintf(stderr, "[ubuntu-loader] Xorg not found; install xserver-xorg-core\n");
        return false;
    }
    const char *required[] = {
        "usr/lib/xorg/modules/drivers/fbdev_drv.so",
        "usr/lib/xorg/modules/input/evdev_drv.so",
        "usr/bin/dbus-run-session",
        "usr/share/X11/xkb"
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        char path[PATH_MAX];
        if (!make_path(path, sizeof(path), root, required[i]) || access(path, R_OK) != 0) {
            fprintf(stderr, "[ubuntu-loader] Missing desktop dependency: /%s\n",
                    required[i]);
            return false;
        }
    }
    if (!check_arm64_elf(root, "usr/lib/xorg/modules/drivers/fbdev_drv.so") ||
        !check_arm64_elf(root, "usr/lib/xorg/modules/input/evdev_drv.so")) {
        fprintf(stderr, "[ubuntu-loader] Xorg fbdev/evdev modules are not ARM64 ELF\n");
        return false;
    }
    const char *sessionBinary = strcmp(desktopSession, "xfce") == 0
        ? "usr/bin/startxfce4" : "usr/bin/openbox-session";
    char sessionPath[PATH_MAX];
    if (!make_path(sessionPath, sizeof(sessionPath), root, sessionBinary) ||
        access(sessionPath, X_OK) != 0) {
        fprintf(stderr, "[ubuntu-loader] Missing desktop session: /%s\n",
                sessionBinary);
        return false;
    }
    if (strcmp(desktopSession, "xfce") == 0) {
        const char *xfceBinaries[] = {
            "usr/bin/xfconf-query",
            "usr/bin/xfce4-session",
            "usr/bin/xfce4-panel",
            "usr/bin/xfdesktop",
            "usr/bin/xfwm4",
            "usr/bin/thunar"
        };
        for (size_t i = 0; i < sizeof(xfceBinaries) / sizeof(xfceBinaries[0]); ++i) {
            char xfcePath[PATH_MAX];
            if (!make_path(xfcePath, sizeof(xfcePath), root, xfceBinaries[i]) ||
                access(xfcePath, X_OK) != 0) {
                fprintf(stderr, "[ubuntu-loader] Missing XFCE component: /%s\n", xfceBinaries[i]);
                return false;
            }
        }
    } else {
        char openboxPath[PATH_MAX];
        if (!make_path(openboxPath, sizeof(openboxPath), root, "usr/bin/openbox") ||
            access(openboxPath, X_OK) != 0) {
            fprintf(stderr, "[ubuntu-loader] Missing Openbox executable: /usr/bin/openbox\n");
            return false;
        }
    }
    if (!check_arm64_elf(root, xorgBinary + 1)) {
        fprintf(stderr, "[ubuntu-loader] Xorg is not a usable ARM64 executable\n");
        return false;
    }
    return true;
}

static void print_xorg_log_tail(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "[ubuntu-loader] Xorg log unavailable: %s\n", path);
        return;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return;
    }
    long end = ftell(file);
    if (end < 0) {
        fclose(file);
        return;
    }
    long start = end > 12288 ? end - 12288 : 0;
    if (fseek(file, start, SEEK_SET) != 0) {
        fclose(file);
        return;
    }
    fprintf(stderr, "[ubuntu-loader] Xorg log tail (%s):\n", path);
    unsigned char buffer[1024];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        fwrite(buffer, 1, count, stderr);
    }
    if (end > 0) fprintf(stderr, "\n[ubuntu-loader] End of Xorg log\n");
    fclose(file);
}

static bool wait_for_x_server(pid_t xorgPid, const char *socketPath,
                             int timeoutMs, bool *alreadyReaped,
                             int *earlyStatus) {
    const int intervalMs = 100;
    int elapsed = 0;
    *alreadyReaped = false;
    while (elapsed < timeoutMs) {
        int status = 0;
        pid_t result = waitpid(xorgPid, &status, WNOHANG);
        if (result == xorgPid) {
            *alreadyReaped = true;
            *earlyStatus = status;
            fprintf(stderr, "[ubuntu-loader] Xorg exited during startup\n");
            return false;
        }
        if (result < 0 && errno != EINTR) {
            print_error("wait Xorg startup", NULL);
            return false;
        }
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct sockaddr_un address;
            memset(&address, 0, sizeof(address));
            address.sun_family = AF_UNIX;
            if (strlen(socketPath) >= sizeof(address.sun_path)) {
                close(fd);
                fprintf(stderr, "[ubuntu-loader] X socket path is too long\n");
                return false;
            }
            strcpy(address.sun_path, socketPath);
            if (connect(fd, reinterpret_cast<struct sockaddr *>(&address),
                        sizeof(address)) == 0) {
                close(fd);
                return true;
            }
            close(fd);
        }
        usleep(intervalMs * 1000);
        elapsed += intervalMs;
    }
    fprintf(stderr, "[ubuntu-loader] Timed out waiting for Xorg display :0\n");
    return false;
}

static int wait_child(pid_t child, int *status) {
    pid_t result;
    do {
        result = waitpid(child, status, 0);
    } while (result < 0 && errno == EINTR);
    return result == child ? 0 : -1;
}

static void terminate_child_process(pid_t child) {
    if (child <= 0) return;
    int status = 0;
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child || (result < 0 && errno == ECHILD)) return;
    if (kill(child, SIGTERM) != 0 && errno != ESRCH) {
        print_error("terminate child", NULL);
    }
    for (int i = 0; i < 30; ++i) {
        result = waitpid(child, &status, WNOHANG);
        if (result == child || (result < 0 && errno == ECHILD)) return;
        usleep(100000);
    }
    kill(child, SIGKILL);
    wait_child(child, &status);
}

static void terminate_xorg(pid_t xorgPid, bool alreadyReaped) {
    if (xorgPid <= 0 || alreadyReaped) return;
    terminate_child_process(xorgPid);
}

static int desktop_child_exec(const char *root, const char *xorgBinary,
                              const char *desktopSession, bool showKeyboard,
                              const char *authorityPath, pid_t expectedParent) {
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != expectedParent) {
        _exit(125);
    }
    reset_child_signals();
    if (setsid() < 0) _exit(126);
    if (chdir(root) != 0 || chroot(".") != 0 || chdir("/") != 0) {
        fprintf(stderr, "[ubuntu-loader] Desktop chroot failed: %s\n", strerror(errno));
        _exit(126);
    }
    clearenv();
    setenv("HOME", "/root", 1);
    setenv("USER", "root", 1);
    setenv("LOGNAME", "root", 1);
    setenv("SHELL", "/bin/bash", 1);
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
    setenv("LANG", "C.UTF-8", 1);
    setenv("LC_ALL", "C.UTF-8", 1);
    setenv("DISPLAY", ":0", 1);
    setenv("XAUTHORITY", authorityPath, 1);
    setenv("XDG_SESSION_TYPE", "x11", 1);
    setenv("XDG_CURRENT_DESKTOP", strcmp(desktopSession, "xfce") == 0 ? "XFCE" : "OPENBOX", 1);
    setenv("XDG_SESSION_DESKTOP", desktopSession, 1);
    setenv("XDG_RUNTIME_DIR", "/tmp/ubuntu-loader-runtime", 1);
    setenv("XDG_CONFIG_HOME", "/tmp/ubuntu-loader-xdg", 1);
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
    setenv("GALLIUM_DRIVER", "llvmpipe", 1);
    setenv("QT_QPA_PLATFORM", "xcb", 1);
    setenv("GDK_BACKEND", "x11", 1);
    umask(022);
    mkdir("/tmp/.X11-unix", 01777);
    chmod("/tmp/.X11-unix", 01777);
    mkdir("/tmp/ubuntu-loader-runtime", 0700);
    chmod("/tmp/ubuntu-loader-runtime", 0700);
    if (strcmp(desktopSession, "xfce") == 0) {
        mkdir("/tmp/ubuntu-loader-xdg", 0700);
        mkdir("/tmp/ubuntu-loader-xdg/xfce4", 0700);
        mkdir("/tmp/ubuntu-loader-xdg/xfce4/xfconf", 0700);
        mkdir("/tmp/ubuntu-loader-xdg/xfce4/xfconf/xfce-perchannel-xml", 0700);
        FILE *profile = fopen("/tmp/ubuntu-loader-xdg/xfce4/xfconf/xfce-perchannel-xml/xfwm4.xml", "w");
        if (profile != NULL) {
            fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                  "<channel name=\"xfwm4\" version=\"1.0\">\n"
                  "<property name=\"general\" type=\"empty\">\n"
                  "<property name=\"use_compositing\" type=\"bool\" value=\"false\"/>\n"
                  "</property>\n</channel>\n", profile);
            fclose(profile);
        }
    }
    unlink("/tmp/.X11-unix/X0");
    unlink("/tmp/.X0-lock");

    pid_t supervisorPid = getpid();
    pid_t xorgPid = fork();
    if (xorgPid < 0) {
        print_error("fork Xorg", NULL);
        _exit(1);
    }
    if (xorgPid == 0) {
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != supervisorPid) _exit(125);
        reset_child_signals();
        char *args[] = {
            const_cast<char *>("Xorg"),
            const_cast<char *>(":0"),
            const_cast<char *>("-config"),
            const_cast<char *>("/tmp/ubuntu-loader-xorg.conf"),
            const_cast<char *>("-auth"),
            const_cast<char *>("/tmp/ubuntu-loader.Xauthority"),
            const_cast<char *>("-nolisten"),
            const_cast<char *>("tcp"),
            const_cast<char *>("-noreset"),
            const_cast<char *>("-novtswitch"),
            const_cast<char *>("-logverbose"),
            const_cast<char *>("6"),
            const_cast<char *>("-logfile"),
            const_cast<char *>("/tmp/ubuntu-loader-xorg.log"),
            NULL
        };
        execv(xorgBinary, args);
        fprintf(stderr, "[ubuntu-loader] exec Xorg failed: %s\n", strerror(errno));
        _exit(errno == ENOENT ? 127 : 126);
    }

    const char *socketPath = "/tmp/.X11-unix/X0";
    bool xorgAlreadyReaped = false;
    int xorgEarlyStatus = 0;
    if (!wait_for_x_server(xorgPid, socketPath, 30000,
                           &xorgAlreadyReaped, &xorgEarlyStatus)) {
        print_xorg_log_tail("/tmp/ubuntu-loader-xorg.log");
        terminate_xorg(xorgPid, xorgAlreadyReaped);
        _exit(1);
    }
    fprintf(stderr, "[ubuntu-loader] Xorg display :0 is ready\n");

    if (showKeyboard && access("/usr/bin/onboard", X_OK) != 0) {
        fprintf(stderr, "[ubuntu-loader] Onboard is not installed; touch keyboard will not be available\n");
    }

    pid_t sessionPid = fork();
    if (sessionPid < 0) {
        print_error("fork desktop session", NULL);
        terminate_xorg(xorgPid, false);
        _exit(1);
    }
    if (sessionPid == 0) {
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != supervisorPid) _exit(125);
        reset_child_signals();
        if (strcmp(desktopSession, "xfce") == 0) {
            const char *script = showKeyboard && access("/usr/bin/onboard", X_OK) == 0
                ? "onboard & onboard_pid=$!; /usr/bin/startxfce4; status=$?; kill $onboard_pid 2>/dev/null; wait $onboard_pid 2>/dev/null; exit $status"
                : "exec /usr/bin/startxfce4";
            char *args[] = {
                const_cast<char *>("dbus-run-session"),
                const_cast<char *>("--"),
                const_cast<char *>("/bin/sh"),
                const_cast<char *>("-c"),
                const_cast<char *>(script),
                NULL
            };
            execv("/usr/bin/dbus-run-session", args);
        } else {
            const char *script = showKeyboard && access("/usr/bin/onboard", X_OK) == 0
                ? "onboard & onboard_pid=$!; /usr/bin/openbox-session; status=$?; kill $onboard_pid 2>/dev/null; wait $onboard_pid 2>/dev/null; exit $status"
                : "exec /usr/bin/openbox-session";
            char *args[] = {
                const_cast<char *>("dbus-run-session"),
                const_cast<char *>("--"),
                const_cast<char *>("/bin/sh"),
                const_cast<char *>("-c"),
                const_cast<char *>(script),
                NULL
            };
            execv("/usr/bin/dbus-run-session", args);
        }
        fprintf(stderr, "[ubuntu-loader] exec desktop session failed: %s\n", strerror(errno));
        _exit(errno == ENOENT ? 127 : 126);
    }

    int sessionStatus = 0;
    if (wait_child(sessionPid, &sessionStatus) != 0) {
        print_error("wait desktop session", NULL);
        print_xorg_log_tail("/tmp/ubuntu-loader-xorg.log");
        terminate_xorg(xorgPid, false);
        _exit(1);
    }
    if (!WIFEXITED(sessionStatus) || WEXITSTATUS(sessionStatus) != 0) {
        print_xorg_log_tail("/tmp/ubuntu-loader-xorg.log");
    }
    terminate_xorg(xorgPid, false);
    if (WIFEXITED(sessionStatus)) _exit(WEXITSTATUS(sessionStatus));
    if (WIFSIGNALED(sessionStatus)) _exit(128 + WTERMSIG(sessionStatus));
    _exit(1);
}

static int check_desktop_preflight(const char *root, const char *fbPath,
                                   const char *inputPath,
                                   const char *desktopSession,
                                   bool showKeyboard) {
    char xorgBinary[PATH_MAX];
    if (!desktop_dependencies(root, desktopSession, xorgBinary, sizeof(xorgBinary))) {
        return 1;
    }
    if (!setup_mounts(root)) {
        cleanup_mounts(root);
        return 1;
    }
    Framebuffer fb;
    if (!fb.open_device(fbPath, false)) {
        fb.close_device();
        cleanup_mounts(root);
        return 1;
    }
    std::vector<InputDevice> inputs;
    if (!open_input_devices(inputPath, inputs, false)) {
        fb.close_device();
        cleanup_mounts(root);
        return 1;
    }
    bool hasTouch = false;
    bool hasKeyboard = false;
    for (size_t i = 0; i < inputs.size(); ++i) {
        if (inputs[i].touch) hasTouch = true;
        if (inputs[i].keyboard) hasKeyboard = true;
    }
    printf("[ubuntu-loader] Xorg: %s\n", xorgBinary);
    printf("[ubuntu-loader] Framebuffer: %s\n", fb.devicePath);
    printf("[ubuntu-loader] Geometry: %ux%u virtual=%ux%u rotate=%u bpp=%u stride=%u memory=%u\n",
           fb.variable.xres, fb.variable.yres,
           fb.variable.xres_virtual, fb.variable.yres_virtual,
           fb.variable.rotate, fb.variable.bits_per_pixel,
           fb.fixed.line_length, fb.fixed.smem_len);
    printf("[ubuntu-loader] Touchscreen: %s\n", hasTouch ? "detected" : "not detected");
    printf("[ubuntu-loader] Hardware keyboard: %s\n", hasKeyboard ? "detected" : "not detected");
    bool onboardAvailable = access((std::string(root) + "/usr/bin/onboard").c_str(), X_OK) == 0;
    printf("[ubuntu-loader] Onboard: %s\n", onboardAvailable ? "available" : "not installed");
    printf("[ubuntu-loader] Renderer: CPU software rendering\n");
    printf("[ubuntu-loader] Shared memory: tmpfs /dev/shm (128 MiB limit)\n");
    printf("[ubuntu-loader] Xorg mode: fbdev, no VT dependency\n");
    close_inputs(inputs);
    fb.close_device();
    bool clean = cleanup_mounts(root);
    if (!clean) return 1;
    if (!hasTouch && !hasKeyboard) {
        fprintf(stderr, "[ubuntu-loader] No touchscreen or keyboard input device found\n");
        return 1;
    }
    if (hasTouch && !hasKeyboard && showKeyboard && !onboardAvailable) {
        fprintf(stderr, "[ubuntu-loader] Touch-only desktop requires Onboard; install package onboard\n");
        return 1;
    }
    if (!showKeyboard && !hasKeyboard) {
        fprintf(stderr, "[ubuntu-loader] On-screen keyboard is disabled and no hardware keyboard is detected\n");
    }
    printf("[ubuntu-loader] Desktop preflight successful; TWRP was not stopped and the display was not changed\n");
    return 0;
}

static int run_desktop_session(const char *root, const char *fbPath,
                               const char *inputPath, pid_t recoveryPid,
                               const TouchTransform &transform,
                               const char *desktopSession, bool showKeyboard) {
    if (recoveryPid <= 1) {
        fprintf(stderr, "[ubuntu-loader] Desktop mode requires --freeze-pid auto or a verified PID\n");
        return 2;
    }
    char xorgBinary[PATH_MAX];
    if (!desktop_dependencies(root, desktopSession, xorgBinary, sizeof(xorgBinary))) {
        fprintf(stderr,
            "[ubuntu-loader] Install: xserver-xorg-core xserver-xorg-video-fbdev "
            "xserver-xorg-input-evdev dbus-x11 xauth xfce4-session xfce4-panel "
            "xfdesktop4 xfwm4 xfce4-settings xfconf thunar xfce4-terminal "
            "fonts-dejavu-core xfonts-base onboard\n");
        return 1;
    }
    if (!install_signal_handlers() || !setup_mounts(root)) {
        cleanup_mounts(root);
        return 1;
    }
    Framebuffer fb;
    if (!fb.open_device(fbPath)) {
        fb.close_device();
        cleanup_mounts(root);
        return 1;
    }
    std::vector<InputDevice> inputs;
    if (!open_input_devices(inputPath, inputs, false)) {
        fb.close_device();
        cleanup_mounts(root);
        return 1;
    }
    bool hasTouch = false;
    bool hasKeyboard = false;
    for (size_t i = 0; i < inputs.size(); ++i) {
        if (inputs[i].touch) hasTouch = true;
        if (inputs[i].keyboard) hasKeyboard = true;
    }
    if (!hasTouch && !hasKeyboard) {
        fprintf(stderr, "[ubuntu-loader] No touchscreen or hardware keyboard detected\n");
        close_inputs(inputs);
        fb.close_device();
        cleanup_mounts(root);
        return 1;
    }
    bool onboardAvailable = access((std::string(root) + "/usr/bin/onboard").c_str(), X_OK) == 0;
    if (hasTouch && !hasKeyboard && showKeyboard && !onboardAvailable) {
        fprintf(stderr, "[ubuntu-loader] Touch-only desktop requires Onboard; install package onboard\n");
        close_inputs(inputs);
        fb.close_device();
        cleanup_mounts(root);
        return 1;
    }
    if (!hasTouch) {
        fprintf(stderr, "[ubuntu-loader] No touchscreen detected; desktop will require a hardware keyboard\n");
    }
    if (!write_xorg_config(root, inputs, transform, fb)) {
        close_inputs(inputs);
        fb.close_device();
        cleanup_mounts(root);
        return 1;
    }
    close_inputs(inputs);

    char authorityPath[PATH_MAX];
    if (!create_xauthority(root, authorityPath, sizeof(authorityPath))) {
        fb.close_device();
        cleanup_mounts(root);
        return 1;
    }
    if (!freeze_recovery(recoveryPid)) {
        char path[PATH_MAX];
        if (make_path(path, sizeof(path), root, "tmp/ubuntu-loader-xorg.conf")) unlink(path);
        if (make_path(path, sizeof(path), root, "tmp/ubuntu-loader.Xauthority")) unlink(path);
        fb.close_device();
        cleanup_mounts(root);
        return 1;
    }
    if (!fb.snapshot()) {
        fprintf(stderr, "[ubuntu-loader] Cannot preserve original TWRP framebuffer\n");
        kill(recoveryPid, SIGCONT);
        fb.close_device();
        cleanup_mounts(root);
        return 1;
    }
    pid_t watchdog = start_recovery_watchdog(recoveryPid, fb.memory,
                                             fb.backup, fb.length);
    if (watchdog < 0) {
        fb.restore();
        kill(recoveryPid, SIGCONT);
        fb.close_device();
        cleanup_mounts(root);
        return 1;
    }

    fprintf(stderr, "[ubuntu-loader] Starting Xorg fbdev desktop on %ux%u %ubpp\n",
            fb.variable.xres, fb.variable.yres, fb.variable.bits_per_pixel);
    fprintf(stderr, "[ubuntu-loader] Renderer: Xorg fbdev with CPU software rendering\n");
    fprintf(stderr, "[ubuntu-loader] Mali acceleration is not enabled; no matching Linux Mesa/DRM stack was verified\n");
    pid_t loaderPid = getpid();
    pid_t child = fork();
    if (child < 0) {
        print_error("fork desktop supervisor", NULL);
        fb.restore();
        kill(recoveryPid, SIGCONT);
        stop_watchdog(watchdog);
        fb.close_device();
        cleanup_mounts(root);
        return 1;
    }
    if (child == 0) {
        munmap(fb.memory, fb.length);
        close(fb.fd);
        desktop_child_exec(root, xorgBinary, desktopSession, showKeyboard && hasTouch,
                           "/tmp/ubuntu-loader.Xauthority", loaderPid);
        _exit(126);
    }
    gChildPid = static_cast<sig_atomic_t>(child);
    int status = 0;
    int waitResult = wait_child(child, &status);
    gChildPid = -1;
    fb.restore();
    fb.close_device();
    char path[PATH_MAX];
    if (make_path(path, sizeof(path), root, "tmp/ubuntu-loader-xorg.conf")) unlink(path);
    if (make_path(path, sizeof(path), root, "tmp/ubuntu-loader.Xauthority")) unlink(path);
    bool mountClean = cleanup_mounts(root);
    kill(recoveryPid, SIGCONT);
    stop_watchdog(watchdog);
    fprintf(stderr, "[ubuntu-loader] Framebuffer restored and TWRP resumed\n");
    if (waitResult != 0 || !mountClean) return 1;
    if (gCaughtSignal != 0) return 128 + gCaughtSignal;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

static int run_adb_session(const char *root, const char *shell,
                           int argc, char **argv, int commandIndex) {
    bool stdinTty = isatty(STDIN_FILENO) != 0;
    bool stdoutTty = isatty(STDOUT_FILENO) != 0;
    bool stderrTty = isatty(STDERR_FILENO) != 0;
    if (commandIndex < 0 && !(stdinTty && stdoutTty && stderrTty)) {
        fprintf(stderr,
                "[ubuntu-loader] Interactive ADB mode requires a remote PTY\n"
                "[ubuntu-loader] Use: adb shell -tt /sbin/ubuntu-loader --rootfs %s\n",
                root);
        return 2;
    }
    if (!install_signal_handlers() || !setup_mounts(root)) {
        cleanup_mounts(root);
        return 1;
    }
    struct utsname uts;
    if (uname(&uts) == 0) {
        fprintf(stderr, "[ubuntu-loader] Kernel: %s %s %s\n",
                uts.sysname, uts.release, uts.machine);
    }
    pid_t loaderPid = getpid();
    pid_t child = fork();
    if (child < 0) {
        print_error("fork Ubuntu", NULL);
        cleanup_mounts(root);
        return 1;
    }
    if (child == 0) {
        child_exec(root, shell, argc, argv, commandIndex,
                   getenv("TERM") != NULL ? getenv("TERM") : "xterm-256color",
                   false, loaderPid);
        _exit(126);
    }
    gChildPid = static_cast<sig_atomic_t>(child);
    int status = 0;
    pid_t result;
    do {
        result = waitpid(child, &status, 0);
    } while (result < 0 && errno == EINTR);
    gChildPid = -1;
    bool clean = cleanup_mounts(root);
    if (result < 0 || !clean) {
        print_error("wait or cleanup", "Ubuntu session");
        return 1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

static pid_t find_recovery_pid(void) {
    DIR *directory = opendir("/proc");
    if (directory == NULL) {
        print_error("opendir /proc", NULL);
        return -1;
    }
    pid_t found = -1;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        char *end = NULL;
        long value = strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || value <= 1 || value > INT_MAX) continue;
        pid_t candidate = static_cast<pid_t>(value);
        if (candidate == getpid()) continue;
        char path[64];
        char executable[PATH_MAX];
        snprintf(path, sizeof(path), "/proc/%ld/exe", value);
        ssize_t length = readlink(path, executable, sizeof(executable) - 1);
        if (length <= 0) continue;
        executable[length] = '\0';
        const char *base = strrchr(executable, '/');
        base = base == NULL ? executable : base + 1;
        if (strcmp(base, "recovery") != 0) continue;
        if (found > 0) {
            fprintf(stderr, "[ubuntu-loader] Multiple recovery processes detected; specify PID manually\n");
            closedir(directory);
            return -1;
        }
        found = candidate;
    }
    closedir(directory);
    if (found <= 1) {
        fprintf(stderr, "[ubuntu-loader] Cannot find a unique /recovery process\n");
        return -1;
    }
    return found;
}

static void show_help(const char *program) {
    printf("Ubuntu ARM64 TWRP Loader %s\n\n", kVersion);
    printf("Usage:\n");
    printf("  %s [--rootfs PATH] [--display adb|fb|desktop] [options]\n\n", program);
    printf("Options:\n");
    printf("  --rootfs PATH          Ubuntu RootFS directory or mounted ext4 mountpoint\n");
    printf("  --display adb          Interactive shell through ADB PTY (default)\n");
    printf("  --display fb           Draw Ubuntu terminal directly to framebuffer\n");
    printf("  --display desktop      Start Ubuntu Xorg desktop on the framebuffer\n");
    printf("                           Requires Xorg fbdev/evdev, XFCE or Openbox, dbus and fonts in RootFS\n");
    printf("  --desktop-session xfce|openbox  Desktop session (default xfce)\n");
    printf("  --fbdev PATH           Framebuffer device (default /dev/graphics/fb0)\n");
    printf("  --screen-rotate DEG    Rotate drawn UI by 0, 90, 180 or 270 degrees\n");
    printf("  --input-dir PATH       Input event directory (default /dev/input)\n");
    printf("  --freeze-pid PID|auto  Verified /recovery PID to pause during framebuffer takeover\n");
    printf("  --probe-display        Print framebuffer geometry and input capabilities\n");
    printf("  --touch-swap-xy        Swap touch X/Y axes\n");
    printf("  --touch-flip-x         Flip horizontal touch coordinate\n");
    printf("  --touch-flip-y         Flip vertical touch coordinate\n");
    printf("  --no-soft-keyboard     Disable the framebuffer/desktop on-screen keyboard\n");
    printf("  --check                Validate RootFS and mount namespace\n");
    printf("  --check-desktop        Preflight Xorg, framebuffer and input without stopping TWRP\n");
    printf("  --help                 Show this help\n");
    printf("  --version              Show version\n\n");
    printf("Examples:\n");
    printf("  adb shell -tt %s --rootfs /data/ubuntu\n", program);
    printf("  %s --rootfs /mnt/ubuntu --check\n", program);
    printf("  adb shell -tt %s --rootfs /data/ubuntu --display fb --freeze-pid auto --screen-rotate 270\n", program);
    printf("  %s --rootfs /mnt/ubuntu --check-desktop --desktop-session xfce\n", program);
    printf("  adb shell -tt %s --rootfs /data/ubuntu --display desktop --desktop-session xfce --freeze-pid auto\n", program);
}

int main(int argc, char **argv) {
    if (geteuid() != 0) {
        fprintf(stderr, "[ubuntu-loader] Root privileges are required\n");
        return 1;
    }

    const char *rootInput = kDefaultRootfs;
    const char *fbPath = kDefaultFramebuffer;
    const char *inputPath = kDefaultInputDirectory;
    bool rootSpecified = false;
    bool checkOnly = false;
    bool checkDesktopOnly = false;
    bool probeDisplay = false;
    bool showKeyboard = true;
    bool framebufferMode = false;
    bool desktopMode = false;
    const char *desktopSession = "xfce";
    int screenRotation = 0;
    int commandIndex = -1;
    pid_t recoveryPid = -1;
    TouchTransform transform;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            show_help(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("%s\n", kVersion);
            return 0;
        }
        if (strcmp(argv[i], "--check") == 0) {
            checkOnly = true;
            continue;
        }
        if (strcmp(argv[i], "--check-desktop") == 0) {
            checkDesktopOnly = true;
            continue;
        }
        if (strcmp(argv[i], "--probe-display") == 0) {
            probeDisplay = true;
            continue;
        }
        if (strcmp(argv[i], "--no-soft-keyboard") == 0) {
            showKeyboard = false;
            continue;
        }
        if (strcmp(argv[i], "--touch-swap-xy") == 0) {
            transform.swapXY = true;
            continue;
        }
        if (strcmp(argv[i], "--touch-flip-x") == 0) {
            transform.flipX = true;
            continue;
        }
        if (strcmp(argv[i], "--touch-flip-y") == 0) {
            transform.flipY = true;
            continue;
        }
        if (strcmp(argv[i], "--rootfs") == 0 || strcmp(argv[i], "--fbdev") == 0 ||
            strcmp(argv[i], "--input-dir") == 0 || strcmp(argv[i], "--display") == 0 ||
            strcmp(argv[i], "--freeze-pid") == 0 || strcmp(argv[i], "--screen-rotate") == 0 ||
            strcmp(argv[i], "--desktop-session") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "[ubuntu-loader] Option requires a value: %s\n", argv[i]);
                return 2;
            }
            const char *option = argv[i];
            const char *value = argv[++i];
            if (strcmp(option, "--rootfs") == 0) {
                rootInput = value;
                rootSpecified = true;
            } else if (strcmp(option, "--fbdev") == 0) {
                fbPath = value;
            } else if (strcmp(option, "--input-dir") == 0) {
                inputPath = value;
            } else if (strcmp(option, "--display") == 0) {
                if (strcmp(value, "fb") == 0) { framebufferMode = true; desktopMode = false; }
                else if (strcmp(value, "adb") == 0) { framebufferMode = false; desktopMode = false; }
                else if (strcmp(value, "desktop") == 0) { framebufferMode = false; desktopMode = true; }
                else {
                    fprintf(stderr, "[ubuntu-loader] Invalid display mode: %s\n", value);
                    return 2;
                }
            } else if (strcmp(option, "--screen-rotate") == 0) {
                char *end = NULL;
                long parsed = strtol(value, &end, 10);
                if (end == value || *end != '\0' ||
                    (parsed != 0 && parsed != 90 && parsed != 180 && parsed != 270)) {
                    fprintf(stderr, "[ubuntu-loader] Invalid screen rotation: %s\n", value);
                    return 2;
                }
                screenRotation = static_cast<int>(parsed);
            } else if (strcmp(option, "--desktop-session") == 0) {
                if (strcmp(value, "xfce") != 0 && strcmp(value, "openbox") != 0) {
                    fprintf(stderr, "[ubuntu-loader] Invalid desktop session: %s\n", value);
                    return 2;
                }
                desktopSession = value;
            } else if (strcmp(option, "--freeze-pid") == 0) {
                if (strcmp(value, "auto") == 0) {
                    recoveryPid = find_recovery_pid();
                    if (recoveryPid <= 1) return 1;
                } else {
                    char *end = NULL;
                    long parsed = strtol(value, &end, 10);
                    if (end == value || *end != '\0' || parsed <= 1 || parsed > INT_MAX) {
                        fprintf(stderr, "[ubuntu-loader] Invalid PID: %s\n", value);
                        return 2;
                    }
                    recoveryPid = static_cast<pid_t>(parsed);
                }
            }
            continue;
        }
        if (strcmp(argv[i], "--") == 0) {
            commandIndex = i + 1;
            break;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "[ubuntu-loader] Unknown option: %s\n", argv[i]);
            return 2;
        }
        if (!rootSpecified) {
            rootInput = argv[i];
            rootSpecified = true;
            commandIndex = i + 1;
            break;
        }
        commandIndex = i;
        break;
    }

    if (commandIndex >= argc) commandIndex = -1;
    if (probeDisplay) {
        if (checkOnly || checkDesktopOnly || commandIndex >= 0 || framebufferMode || desktopMode) {
            fprintf(stderr, "[ubuntu-loader] --probe-display cannot be combined with checks, commands or graphical display modes\n");
            return 2;
        }
        return probe_display(fbPath, inputPath);
    }
    if ((checkOnly || checkDesktopOnly || desktopMode || framebufferMode) && commandIndex >= 0) {
        fprintf(stderr, "[ubuntu-loader] Check or graphical display mode cannot be combined with a command\n");
        return 2;
    }
    if (checkOnly && checkDesktopOnly) {
        fprintf(stderr, "[ubuntu-loader] --check and --check-desktop are mutually exclusive\n");
        return 2;
    }

    char root[PATH_MAX];
    char shell[PATH_MAX];
    if (!validate_rootfs(rootInput, root, sizeof(root), shell, sizeof(shell))) return 1;

    printf("[ubuntu-loader] %s\n", kVersion);
    printf("[ubuntu-loader] RootFS: %s\n", root);
    printf("[ubuntu-loader] ARM64 shell: %s\n", shell);

    if (checkOnly) {
        if (!setup_mounts(root)) {
            cleanup_mounts(root);
            return 1;
        }
        if (!cleanup_mounts(root)) return 1;
        printf("[ubuntu-loader] RootFS and mount namespace validation successful\n");
        return 0;
    }
    if (checkDesktopOnly) {
        return check_desktop_preflight(root, fbPath, inputPath, desktopSession,
                                       showKeyboard);
    }

    if (desktopMode) {
        if (screenRotation != 0) {
            fprintf(stderr, "[ubuntu-loader] --screen-rotate is not applied in Xorg desktop mode; the kernel framebuffer orientation is used\n");
        }
        return run_desktop_session(root, fbPath, inputPath, recoveryPid,
                                   transform, desktopSession, showKeyboard);
    }
    if (framebufferMode) {
        return run_framebuffer_session(root, shell, argc, argv, commandIndex,
                                       fbPath, inputPath, recoveryPid,
                                       transform, screenRotation, showKeyboard);
    }
    return run_adb_session(root, shell, argc, argv, commandIndex);
}
