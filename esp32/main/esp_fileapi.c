// psyz's low-level file API over newlib/VFS - in its own translation unit on
// purpose: psyz/system.h macro-renames open/read/write/lseek/close, which in
// any TU that includes it turns both newlib's declarations and these wrappers'
// inner calls into psyz_* and knots the whole thing. No psyz header here.

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

// SOTN_DATA_ROOT — where the disc-derived files live — arrives as a compile
// definition rather than from a header ON PURPOSE. The Waveshare board flashes
// them into a FAT partition and the handheld keeps them on its microSD, so the
// path has to vary by board; but pulling board_pins.h in here drags the whole
// IDF driver header chain with it, and this file's hand-rolled externs (see
// below) then collide with the real declarations. Keeping it a plain string
// preserves the isolation the file was built around.

// ESP-IDF's VFS ignores chdir(): every relative path must be rebased onto the
// data mount point by hand. The component compiles with -Dfopen=sotn_fopen so
// all game/psyz stdio opens land here.
#undef fopen
FILE* sotn_fopen(const char* path, const char* mode) {
    extern int heap_caps_check_integrity_all(int print_errors);
    static int broken;
    if (!broken && !heap_caps_check_integrity_all(1)) {
        broken = 1;
        printf("sotn: HEAP BROKEN before open '%s'\n", path);
    }
    if (path && path[0] != '/') {
        char abs[128];
        snprintf(abs, sizeof(abs), SOTN_DATA_ROOT "/%s", path);
        return fopen(abs, mode);
    }
    return fopen(path, mode);
}

int psyz_open(const char* devname, int flag) {
    if (devname && devname[0] != '/') {
        char abs[128];
        snprintf(abs, sizeof(abs), SOTN_DATA_ROOT "/%s", devname);
        return open(abs, flag, 0666);
    }
    return open(devname, flag, 0666);
}
int psyz_close(int fd) { return close(fd); }
long psyz_lseek(long fd, long offset, long flag) {
    return lseek((int)fd, offset, (int)flag);
}
long psyz_read(long fd, void* buf, long n) { return read((int)fd, buf, n); }
long psyz_write(long fd, void* buf, long n) { return write((int)fd, buf, n); }
long psyz_ioctl(long fd, long com, long arg) {
    (void)fd;
    (void)com;
    (void)arg;
    return 0;
}

struct DIRENTRY;
struct DIRENTRY* my_firstfile(const char* pattern, struct DIRENTRY* entry) {
    (void)pattern;
    (void)entry;
    return 0;
}
struct DIRENTRY* my_nextfile(struct DIRENTRY* entry) {
    (void)entry;
    return 0;
}
// the save/load menu (SEL) deletes memcard files through this
long my_erase(const char* path) {
    if (path && path[0] != '/') {
        char abs[128];
        snprintf(abs, sizeof(abs), SOTN_DATA_ROOT "/%s", path);
        return remove(abs) == 0 ? 1 : 0;
    }
    return remove(path) == 0 ? 1 : 0;
}

long my_format(const char* fs) {
    (void)fs;
    return 0;
}
