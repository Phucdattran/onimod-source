#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <link.h>
#include <atomic>
#include <android/log.h>
#include "Includes/obfuscate.h"

#define BUILD_ID "IDLEPIRATE_20261103_R3"
#define LOG_TAG  "HAMOD_PIRATE"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ==========================================
// BIẾN TOÀN CỤC
// ==========================================
static bool v_free_packs = false;
static std::atomic<uintptr_t> libMain_base{0};

// ==========================================
// TÌM BASE ADDRESS libMain.so
// ==========================================
static int dl_find_main(struct dl_phdr_info* info, size_t, void* data) {
    if (info->dlpi_name && strstr(info->dlpi_name, "libMain.so")) {
        libMain_base = info->dlpi_addr;
        *(bool*)data = true;
        return 1;
    }
    return 0;
}
static bool isLibMainLoaded() {
    bool found = false;
    dl_iterate_phdr(dl_find_main, &found);
    return found;
}

// ==========================================
// GHI HEX VÀO MEMORY
// ==========================================
static void PatchHex(uintptr_t base, uint32_t off, const char* hex) {
    if (!base) return;
    uintptr_t addr = base + off;
    size_t len = strlen(hex);
    if (len % 2) return;
    size_t n = len / 2;
    uint8_t* b = new uint8_t[n];
    for (size_t i = 0; i < n; i++) sscanf(hex + 2*i, "%2hhx", &b[i]);
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    uintptr_t ps = addr & ~(pg - 1);
    size_t pl = (((addr + n) - ps + pg - 1) / pg) * pg;
    if (mprotect((void*)ps, pl, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
        memcpy((void*)addr, b, n);
        __builtin___clear_cache((char*)addr, (char*)(addr + n));
        mprotect((void*)ps, pl, PROT_READ | PROT_EXEC);
        LOGI("[+] Patch 0x%X OK (%zu B)", off, n);
    } else {
        LOGE("[-] Patch 0x%X FAIL errno=%d", off, errno);
    }
    delete[] b;
}

// ==========================================
// ÁP DỤNG PATCH — com.limebolt.idlepirate
// libMain.so Build ID: 022a32a3e3a2955a9233457de6f0b3f3efbcb161
// ==========================================
static void ApplyPatches(bool on) {
    uintptr_t b = libMain_base.load();
    if (!b) { LOGE("ApplyPatches: base=0"); return; }

    if (on) {
        // ① PATCH_MAIN — purchasePack fn@0x54243C
        PatchHex(b, 0x54243C,
            "fd7bbea9fd0300917101249f0a0100542a0403e0"
            "e52d0894b90013e07c2a0894aa0003e1b94013e0"
            "092d0894fd7bc2a8d65f03c0d503201f");

        // ② PATCH_BR — cheat trips → unconditional branch
        PatchHex(b, 0x99F4B8, "03000014"); // Flooz
        PatchHex(b, 0x9A89CC, "06000014"); // BlackGem

        // ③ NOP — PurchaseLogger
        PatchHex(b, 0x6BB290, "1f2003d5");
        PatchHex(b, 0x74C898, "1f2003d5");
        PatchHex(b, 0x74CCC0, "1f2003d5");
        PatchHex(b, 0x74D180, "1f2003d5");
        PatchHex(b, 0x74D414, "1f2003d5");
        PatchHex(b, 0x74D58C, "1f2003d5");
        PatchHex(b, 0x74D718, "1f2003d5");
        PatchHex(b, 0x74DAB8, "1f2003d5");

        // ④ NOP — EventTracker
        PatchHex(b, 0x6BAE2C, "1f2003d5");
        PatchHex(b, 0x6BB268, "1f2003d5");
        PatchHex(b, 0x74C73C, "1f2003d5");
        PatchHex(b, 0x74C90C, "1f2003d5");
        PatchHex(b, 0x74CAF4, "1f2003d5");
        PatchHex(b, 0x74D978, "1f2003d5");

        // ⑤ NOP — ShowPurchaseDlg
        PatchHex(b, 0x74CEE4, "1f2003d5");

        LOGI("=== FREE PACKS: ON ===");

    } else {
        // ① PATCH_MAIN OFF
        PatchHex(b, 0x54243C,
            "ff0302d1fd7b04a9f72b00f9f65706a9f44f07a9"
            "fd03019157d03bd5f50305aaf303042ae81640f9"
            "e10303aaf403022af60300aaa8831ff8");

        // ② PATCH_BR OFF
        PatchHex(b, 0x99F4B8, "65000054");
        PatchHex(b, 0x9A89CC, "c8000054");

        // ③ PurchaseLogger OFF
        PatchHex(b, 0x6BB290, "d2430294");
        PatchHex(b, 0x74C898, "50feff97");
        PatchHex(b, 0x74CCC0, "46fdff97");
        PatchHex(b, 0x74D180, "16fcff97");
        PatchHex(b, 0x74D414, "71fbff97");
        PatchHex(b, 0x74D58C, "13fbff97");
        PatchHex(b, 0x74D718, "b0faff97");
        PatchHex(b, 0x74DAB8, "c8f9ff97");

        // ④ EventTracker OFF
        PatchHex(b, 0x6BAE2C, "05f6f997");
        PatchHex(b, 0x6BB268, "f6f4f997");
        PatchHex(b, 0x74C73C, "c1aff797");
        PatchHex(b, 0x74C90C, "4daff797");
        PatchHex(b, 0x74CAF4, "d3aef797");
        PatchHex(b, 0x74D978, "32abf797");

        // ⑤ ShowPurchaseDlg OFF
        PatchHex(b, 0x74CEE4, "cab30094");

        LOGI("=== FREE PACKS: OFF ===");
    }
}

// ==========================================
// Doc_Config_Ngam — đọc config HAMOD
// File: $MODMENU_LOG_DIR/mods/config_com.limebolt.idlepirate.txt
// Format: "1 0 0 0 0 0 0" (space-separated floats)
//         index 0 = Free Packs
// ==========================================
static void* Doc_Config_Ngam(void*) {
    while (libMain_base.load() == 0) usleep(500000);

    const char* log_dir = getenv("MODMENU_LOG_DIR");
    char cfg_path[512];
    if (log_dir && *log_dir) {
        snprintf(cfg_path, sizeof(cfg_path),
            "%s/mods/config_com.limebolt.idlepirate.txt", log_dir);
    } else {
        strcpy(cfg_path,
            "/data/data/com.hamod.vn/files/mods/config_com.limebolt.idlepirate.txt");
    }

    LOGI("[%s] CONFIG: %s", BUILD_ID, cfg_path);

    int last_free_packs = -1;

    while (true) {
        FILE* f = fopen(cfg_path, "r");
        if (f) {
            float f0 = 0;
            if (fscanf(f, "%f", &f0) == 1) {
                int cur = (int)f0;
                if (cur != last_free_packs) {
                    LOGI("[+] Free Packs = %d", cur);
                    v_free_packs = (cur == 1);
                    ApplyPatches(v_free_packs);
                    last_free_packs = cur;
                }
            }
            fclose(f);
        }
        usleep(500000);
    }
    return nullptr;
}

// ==========================================
// WORKER THREAD
// ==========================================
static void* h_t(void*) {
    sleep(4);
    while (!isLibMainLoaded()) usleep(500000);
    LOGI("[%s] libMain.so base=0x%" PRIxPTR, BUILD_ID, libMain_base.load());
    return nullptr;
}

// ==========================================
// ENTRY POINT
// ==========================================
__attribute__((constructor))
void l_m() {
    char cmdline[256] = {0};
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, cmdline, sizeof(cmdline) - 1);
        close(fd);
        if (n > 0) cmdline[n] = '\0';
        if (strstr(cmdline, "com.limebolt.idlepirate") == nullptr) return;
    }
    LOGI("[%s] injected!", BUILD_ID);
    pthread_t t1, t2;
    if (pthread_create(&t1, nullptr, h_t, nullptr) == 0) pthread_detach(t1);
    if (pthread_create(&t2, nullptr, Doc_Config_Ngam, nullptr) == 0) pthread_detach(t2);
}
