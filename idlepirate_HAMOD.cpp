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

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "HAMOD_RADAR", __VA_ARGS__)
#define HAMOD_BUILD_ID "IDLEPIRATE_UI_FINISH_20261102_R2"

// Target: com.limebolt.idlepirate (Idle Pirate Tycoon), AArch64.
// Verified libMain.so GNU Build ID: 022a32a3e3a2955a9233457de6f0b3f3efbcb161.
//
// Callee functions (new binary):
//   PurchaseLogger  = 0x74C1D8  (anchor: "Purchase" in .rodata)
//   EventTracker    = 0x538640  (anchor: "PiggyBank" in .rodata)
//   ShowPurchaseDlg = 0x779E0C  (anchor: "Offer not credited " in .rodata)
//
//   RevenueReporter — NOT FOUND in this update; delta estimate 0x74D4B0 is invalid.
//   PATCH_MAIN      — NOT FOUND in this update; fn@0x52BA08 has only 1 BL call,
//                     lookup/handler cannot be identified. Needs manual re-investigation.
//
// PATCH_BR: 2 cheat-trip sites (Flooz B.PL, BlackGem B.HI).
//   Note: Flooz trip #2 (old 0x77A34C) was not found — possibly merged or restructured.

bool v_free_packs = false;
std::atomic<uintptr_t> libMain_base{0};

// ==========================================
// HÀM TÌM BASE ADDRESS CỦA THƯ VIỆN
// ==========================================
int dl_iterate_callback(struct dl_phdr_info *info, size_t size, void *data) {
    if (info->dlpi_name && strstr(info->dlpi_name, "libMain.so")) {
        libMain_base = info->dlpi_addr;
        *(bool*)data = true;
        return 1;
    }
    return 0;
}

bool isLibraryLoaded_Safe() {
    bool isLoaded = false;
    dl_iterate_phdr(dl_iterate_callback, &isLoaded);
    return isLoaded;
}

// ====================================================================
// HÀM CHUYỂN ĐỔI CHUỖI HEX VÀ GHI VÀO MEMORY
// ====================================================================
void PatchHexMemory(uintptr_t libBase, uint32_t offset, const char* hexString) {
    if (libBase == 0) return;

    uintptr_t target_addr = libBase + offset;

    size_t len = strlen(hexString);

    if ((len % 2) != 0) {
        LOGD("[-] PatchHex: Hex khong hop le tai offset 0x%X", offset);
        return;
    }

    size_t byteLen = len / 2;
    uint8_t* patchBytes = new uint8_t[byteLen];

    for (size_t i = 0; i < byteLen; ++i) {
        if (sscanf(hexString + 2 * i, "%2hhx", &patchBytes[i]) != 1) {
            LOGD("[-] PatchHex: Loi parse Hex tai offset 0x%X", offset);
            delete[] patchBytes;
            return;
        }
    }

    size_t page_size = sysconf(_SC_PAGESIZE);

    uintptr_t page_start =
        target_addr & ~(static_cast<uintptr_t>(page_size) - 1);

    uintptr_t patch_end = target_addr + byteLen;

    size_t protect_len =
        ((patch_end - page_start + page_size - 1) / page_size) * page_size;

    if (mprotect(
            reinterpret_cast<void*>(page_start),
            protect_len,
            PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {

        LOGD(
            "[-] mprotect RWX that bai tai 0x%X errno=%d",
            offset,
            errno
        );

        delete[] patchBytes;
        return;
    }

    memcpy(
        reinterpret_cast<void*>(target_addr),
        patchBytes,
        byteLen
    );

    // ARM64: đồng bộ instruction cache sau khi sửa code
    __builtin___clear_cache(
        reinterpret_cast<char*>(target_addr),
        reinterpret_cast<char*>(target_addr + byteLen)
    );

    if (mprotect(
            reinterpret_cast<void*>(page_start),
            protect_len,
            PROT_READ | PROT_EXEC) != 0) {

        LOGD(
            "[-] mprotect RX that bai tai 0x%X errno=%d",
            offset,
            errno
        );
    }

    delete[] patchBytes;

    LOGD(
        "[+] PatchHex: Da patch offset 0x%X (%zu bytes)",
        offset,
        byteLen
    );
}

// ====================================================================
// ÁP DỤNG BỘ PATCH — com.limebolt.idlepirate (UPDATE 20261102)
// ====================================================================
void ApplyPatches(bool enable) {
    if (libMain_base == 0) {
        LOGD("[-] ApplyPatches: libMain_base = 0");
        return;
    }

    if (enable) {

        // ============================================================
        // ON
        // ============================================================

        // ① PATCH_MAIN — purchasePack fn@0x54243C (string "purchasePack %d %s %d %s" → 0x2C0C73)
        //   W4 = product_id; chain: BL1=0x74DBE4 (lookup), BL2=0x74CE48 (item desc), BL3=0x74D888 (grant)
        //   32-byte frame to save BL1's W0 between BL1 and BL2 calls.
        //   For W4 < 73 (free IDs): runs full grant chain and returns early.
        //   For W4 >= 73 (paid IDs): B.GE → LDP+RET immediately (no IAB initiated).
        PatchHexMemory(libMain_base, 0x54243C, "fd7bbea9fd0300917101249f0a0100542a0403e0e52d0894b90013e07c2a0894aa0003e1b94013e0092d0894fd7bc2a8d65f03c0d503201f");

        // ② PATCH_BR — 2 self-reporting cheat trips → unconditional branch
        PatchHexMemory(libMain_base, 0x99F4B8, "03000014"); // Flooz cheat trip (B.PL → B)
        PatchHexMemory(libMain_base, 0x9A89CC, "06000014"); // BlackGem cheat trip (B.HI → B)

        // ③ NOP — PurchaseLogger (0x74C1D8) — huỷ analytics mua hàng
        PatchHexMemory(libMain_base, 0x6BB290, "1f2003d5"); // PiggyBank
        PatchHexMemory(libMain_base, 0x74C898, "1f2003d5"); // IAP/PackFlooz
        PatchHexMemory(libMain_base, 0x74CCC0, "1f2003d5"); // IAP/Offer
        PatchHexMemory(libMain_base, 0x74D180, "1f2003d5"); // IAP/Shop/Starter
        PatchHexMemory(libMain_base, 0x74D414, "1f2003d5"); // DailyPack#1
        PatchHexMemory(libMain_base, 0x74D58C, "1f2003d5"); // DailyPack#2
        PatchHexMemory(libMain_base, 0x74D718, "1f2003d5"); // DailyPack#3
        PatchHexMemory(libMain_base, 0x74DAB8, "1f2003d5"); // Shop/Pack/SkipItPass

        // ④ NOP — EventTracker (0x538640) — huỷ event tracker
        PatchHexMemory(libMain_base, 0x6BAE2C, "1f2003d5"); // PiggyBankPurchase
        PatchHexMemory(libMain_base, 0x6BB268, "1f2003d5"); // PiggyBank/Break
        PatchHexMemory(libMain_base, 0x74C73C, "1f2003d5"); // IAP/PackFlooz#1
        PatchHexMemory(libMain_base, 0x74C90C, "1f2003d5"); // IAP/PackFlooz#2
        PatchHexMemory(libMain_base, 0x74CAF4, "1f2003d5"); // IAP/Offer
        PatchHexMemory(libMain_base, 0x74D978, "1f2003d5"); // Shop/Pack

        // ⑤ NOP — RevenueReporter — TODO: callee not identified in new binary.
        //   Old callee 0x524BF0, NOP site 0x73BDD8. Delta estimate 0x964698 has no BL.
        //   Needs manual search before enabling.

        // ⑥ NOP — ShowPurchaseDialog (0x779E0C) — huỷ popup mua không thành công
        PatchHexMemory(libMain_base, 0x74CEE4, "1f2003d5"); // "Offer not credited / not found"

        LOGD("[%s] ApplyPatches: ANTI-BAN ON", HAMOD_BUILD_ID);

    } else {

        // ============================================================
        // OFF
        // ============================================================

        // ① PATCH_MAIN OFF — khôi phục purchasePack fn@0x54243C
        PatchHexMemory(libMain_base, 0x54243C, "ff0302d1fd7b04a9f72b00f9f65706a9f44f07a9fd03019157d03bd5f50305aaf303042ae81640f9e10303aaf403022af60300aaa8831ff8");

        // ② PATCH_BR — khôi phục cheat trips
        PatchHexMemory(libMain_base, 0x99F4B8, "65000054");
        PatchHexMemory(libMain_base, 0x9A89CC, "c8000054");

        // ③ NOP OFF — PurchaseLogger
        PatchHexMemory(libMain_base, 0x6BB290, "d2430294");
        PatchHexMemory(libMain_base, 0x74C898, "50feff97");
        PatchHexMemory(libMain_base, 0x74CCC0, "46fdff97");
        PatchHexMemory(libMain_base, 0x74D180, "16fcff97");
        PatchHexMemory(libMain_base, 0x74D414, "71fbff97");
        PatchHexMemory(libMain_base, 0x74D58C, "13fbff97");
        PatchHexMemory(libMain_base, 0x74D718, "b0faff97");
        PatchHexMemory(libMain_base, 0x74DAB8, "c8f9ff97");

        // ④ NOP OFF — EventTracker
        PatchHexMemory(libMain_base, 0x6BAE2C, "05f6f997");
        PatchHexMemory(libMain_base, 0x6BB268, "f6f4f997");
        PatchHexMemory(libMain_base, 0x74C73C, "c1aff797");
        PatchHexMemory(libMain_base, 0x74C90C, "4daff797");
        PatchHexMemory(libMain_base, 0x74CAF4, "d3aef797");
        PatchHexMemory(libMain_base, 0x74D978, "32abf797");

        // ⑤ NOP OFF — RevenueReporter (skipped)

        // ⑥ NOP OFF — ShowPurchaseDialog
        PatchHexMemory(libMain_base, 0x74CEE4, "cab30094");

        LOGD("[%s] ApplyPatches: ANTI-BAN OFF", HAMOD_BUILD_ID);
    }
}

// ====================================================================
// LUỒNG NGẦM KHỞI TẠO
// ====================================================================
void *h_t(void *) {
    sleep(4);

    while (!isLibraryLoaded_Safe()) {
        usleep(500000);
    }

    LOGD(
        "[%s] Da load thanh cong libMain.so! Base = %" PRIxPTR,
        HAMOD_BUILD_ID,
        libMain_base.load()
    );

    return NULL;
}

// ====================================================================
// LUỒNG NHẬN LỆNH TỪ MENU
// ====================================================================
void *Doc_Config_Ngam(void *) {
    // Chờ h_t publish base trước khi xử lý menu
    while (libMain_base.load() == 0) {
        usleep(500000);
    }

    char config_path[512];

    const char* log_dir = getenv("MODMENU_LOG_DIR");

    if (log_dir != NULL) {
        snprintf(
            config_path,
            sizeof(config_path),
            "%s/mods/config_com.limebolt.idlepirate.txt",
            log_dir
        );
    } else {
        strcpy(
            config_path,
            "/data/data/menu.nextgen.launcher/files/mods/"
            "config_com.limebolt.idlepirate.txt"
        );
    }

    int last_free_packs = -1;

    while (true) {
        FILE *file = fopen(config_path, "r");

        if (file != NULL) {
            float f1 = 0;

            int read_count = fscanf(file, "%f", &f1);

            if (read_count >= 1) {
                int current_free_packs = (int)f1;

                if (current_free_packs != last_free_packs) {

                    LOGD(
                        "[+] MENU UPDATE: Free Packs = %d",
                        current_free_packs
                    );

                    v_free_packs =
                        (current_free_packs == 1);

                    ApplyPatches(v_free_packs);

                    last_free_packs =
                        current_free_packs;
                }
            }

            fclose(file);
        }

        usleep(500000);
    }

    return NULL;
}

// ====================================================================
// ENTRY
// ====================================================================
__attribute__((constructor))
void l_m() {

    char cmdline[256] = {0};

    int fd = open(
        "/proc/self/cmdline",
        O_RDONLY
    );

    if (fd >= 0) {

        ssize_t bytesRead =
            read(
                fd,
                cmdline,
                sizeof(cmdline) - 1
            );

        close(fd);

        if (bytesRead > 0) {
            cmdline[bytesRead] = '\0';
        }

        if (
            strstr(
                cmdline,
                "com.limebolt.idlepirate"
            ) == nullptr
        ) {
            return;
        }
    }

    pthread_t ptid;
    pthread_t config_tid;

    pthread_create(&ptid, NULL, h_t, NULL);
    pthread_create(&config_tid, NULL, Doc_Config_Ngam, NULL);

    pthread_detach(ptid);
    pthread_detach(config_tid);
}
