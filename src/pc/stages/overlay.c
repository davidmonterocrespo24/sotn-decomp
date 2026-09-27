// SPDX-License-Identifier: AGPL-3.0-or-later
#if !defined(_WIN32) && !defined(__APPLE__)
#include <unistd.h>
#endif
#include <game.h>
#include <stdio.h>
#include <string.h>
#include "overlay.h"

#if defined(ESP_PLATFORM)
// No dynamic linker on the ESP32-S3: every stage the firmware knows lives in
// the static table below, and this arm makes the dynamic path a failing stub.
#define OVL_EXT ".none"
typedef void* OvlHandle;
static OvlHandle OvlOpen(const char* path) { (void)path; return 0; }
static void* OvlSym(OvlHandle h, const char* name) {
    (void)h;
    (void)name;
    return 0;
}
static void OvlClose(OvlHandle h) { (void)h; }
static const char* OvlError(void) { return "no dynamic linker on this target"; }
static bool GetExePath(char* buf, size_t size) {
    (void)buf;
    (void)size;
    return false;
}
#elif defined(_WIN32)
__declspec(dllimport) void* __stdcall LoadLibraryA(const char* lpLibFileName);
__declspec(dllimport) void* __stdcall GetProcAddress(
    void* hModule, const char* lpProcName);
__declspec(dllimport) int __stdcall FreeLibrary(void* hLibModule);
__declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(
    void* hModule, char* lpFilename, unsigned long nSize);

#define OVL_EXT ".dll"
typedef void* OvlHandle;

static OvlHandle OvlOpen(const char* path) { return LoadLibraryA(path); }
static void* OvlSym(OvlHandle h, const char* name) {
    return (void*)GetProcAddress(h, name);
}
static void OvlClose(OvlHandle h) { FreeLibrary(h); }
static const char* OvlError(void) {
    return "LoadLibrary/GetProcAddress failed";
}
static bool GetExePath(char* buf, size_t size) {
    unsigned long len = GetModuleFileNameA(NULL, buf, (unsigned long)size);
    return len != 0 && len < size;
}
#elif defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/dyld.h>

#define OVL_EXT ".dylib"
typedef void* OvlHandle;

static OvlHandle OvlOpen(const char* path) {
    return dlopen(path, RTLD_NOW | RTLD_GLOBAL);
}
static void* OvlSym(OvlHandle h, const char* name) { return dlsym(h, name); }
static void OvlClose(OvlHandle h) { dlclose(h); }
static const char* OvlError(void) { return dlerror(); }
static bool GetExePath(char* buf, size_t size) {
    uint32_t len = size;
    return _NSGetExecutablePath(buf, &len) == 0;
}
#else
#include <dlfcn.h>

#define OVL_EXT ".so"
typedef void* OvlHandle;

static OvlHandle OvlOpen(const char* path) {
    return dlopen(path, RTLD_NOW | RTLD_GLOBAL);
}
static void* OvlSym(OvlHandle h, const char* name) { return dlsym(h, name); }
static void OvlClose(OvlHandle h) { dlclose(h); }
static const char* OvlError(void) { return dlerror(); }
static bool GetExePath(char* buf, size_t size) {
    ssize_t len = readlink("/proc/self/exe", buf, size - 1);
    if (len <= 0) {
        return false;
    }
    buf[len] = '\0';
    return true;
}
#endif

static const char* GetExeDir(void) {
    static char dir[512];
    static bool done = false;
    static bool isok = false;
    if (done) {
        return isok ? dir : NULL;
    }
    done = true;

    if (!GetExePath(dir, sizeof(dir))) {
        return NULL;
    }

    // strip the trailing "exe" on Windows to keep only the directory name
    char* lastSep = strrchr(dir, '/');
    char* lastBackslash = strrchr(dir, '\\');
    if (lastBackslash > lastSep) {
        lastSep = lastBackslash;
    }
    if (!lastSep) {
        return NULL;
    }
    *lastSep = '\0';

    isok = true;
    return dir;
}

static void* OpenOverlayEntrypoint(
    const char* name, const char* entrypointName, OvlHandle* outHandle) {
    char path[512];
    const char* exeDir = GetExeDir();
    if (exeDir) {
        // overlays must be loaded from the exe directory
        snprintf(path, sizeof(path), "%s/%s%s", exeDir, name, OVL_EXT);
    } else {
        // alternatively, use current working directory as fallback
        snprintf(path, sizeof(path), "%s%s", name, OVL_EXT);
    }

    OvlHandle handle = OvlOpen(path);
    if (!handle) {
        ERRORF("failed to load '%s': %s", path, OvlError());
        return NULL;
    }

    void* entrypoint = OvlSym(handle, entrypointName);
    if (!entrypoint) {
        ERRORF("failed as '%s' has no '%s' entrypoint: %s", path,
               entrypointName, OvlError());
        OvlClose(handle);
        return NULL;
    }

    INFOF("loaded '%s'", path);
    *outHandle = handle;
    return entrypoint;
}

static OvlHandle CurrentStageOverlay = NULL;

#ifdef SOTN_STATIC_WRP
// Statically linked stages, resolved before any dynamic loading. This is the
// whole overlay mechanism on the ESP32-S3 target, where no dynamic linker
// exists; on PC it coexists with the DLL path so unported stages keep working.
void WRP_StaticInitStage(Overlay* o);
#ifdef SOTN_STATIC_NZ0
void NZ0_StaticInitStage(Overlay* o);
#endif
#ifdef SOTN_STATIC_SEL
void SEL_StaticInitStage(Overlay* o);
#endif
#ifdef SOTN_STATIC_NP3
void NP3_StaticInitStage(Overlay* o);
#endif
static const struct {
    const char* name;
    PfnInitStage init;
} static_stages[] = {
    {"wrp", WRP_StaticInitStage},
#ifdef SOTN_STATIC_NZ0
    {"nz0", NZ0_StaticInitStage},
#endif
#ifdef SOTN_STATIC_SEL
    {"sel", SEL_StaticInitStage},
#endif
#ifdef SOTN_STATIC_NP3
    {"np3", NP3_StaticInitStage},
#endif
};
#endif

bool LoadStageOverlay(const char* name, Overlay* o) {
    OvlHandle handle;
    PfnInitStage entrypoint;

#ifdef SOTN_STATIC_WRP
    for (size_t i = 0; i < sizeof(static_stages) / sizeof(*static_stages);
         i++) {
        if (!strcmp(name, static_stages[i].name)) {
            if (CurrentStageOverlay) {
                OvlClose(CurrentStageOverlay);
                CurrentStageOverlay = NULL;
            }
            INFOF("stage '%s' statically linked", name);
            static_stages[i].init(o);
            return true;
        }
    }
#endif

    if (CurrentStageOverlay) {
        OvlClose(CurrentStageOverlay);
        CurrentStageOverlay = NULL;
    }
    entrypoint =
        (PfnInitStage)OpenOverlayEntrypoint(name, "InitStage", &handle);
    if (!entrypoint) {
        return false;
    }
    CurrentStageOverlay = handle;
    entrypoint(o);
    return true;
}

static OvlHandle CurrentServantOverlay = NULL;
bool LoadServantOverlay(const char* name, ServantDesc* o) {
    OvlHandle handle;
    PfnInitServant entrypoint;

    if (CurrentServantOverlay) {
        OvlClose(CurrentServantOverlay);
        CurrentServantOverlay = NULL;
    }
    entrypoint =
        (PfnInitServant)OpenOverlayEntrypoint(name, "InitServant", &handle);
    if (!entrypoint) {
        return false;
    }
    CurrentServantOverlay = handle;
    entrypoint(o);
    return true;
}
