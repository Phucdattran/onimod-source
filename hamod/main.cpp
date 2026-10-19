// ONI MOD — HAMOD IL2CPP port
// Port từ onimod worker() (ARM64, process_vm_readv + write_code)
// sang Dobby hook + HAMOD config pattern.
//
// ĐỔI TRƯỚC KHI BUILD:
//   PKG_NAME  → package name thực tế của game
//   NS_MAIN   → namespace C# chứa KCommand / DebugTool

#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <link.h>
#include <elf.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <atomic>
#include <cmath>
#include <android/log.h>
#include "Includes/obfuscate.h"
#include <Dobby/dobby.h>

#define BUILD_ID   "ONI_MOD"
#define LOG_TAG    "OniHamod"
#define PKG_NAME   "com.your.game.package"
#define NS_MAIN    ""   // namespace C# — để "" nếu không có

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─── Trạng thái mod từ menu (index tương ứng menu.js) ──────────────────────
std::atomic<bool>  g_dump{false};    // index 0 — boc script Lua
std::atomic<bool>  g_dmg{false};     // index 1 — damage mod
std::atomic<float> g_dmgmul{10.0f}; // index 2 — he so damage (1..20)
std::atomic<bool>  g_god{false};     // index 3 — god mode
std::atomic<bool>  g_gm{false};      // index 4 — GM mode
static std::atomic<bool> g_config_loaded{false};

// ─── IL2CPP API ─────────────────────────────────────────────────────────────
void*       (*il2cpp_domain_get)()                                             = nullptr;
void*       (*il2cpp_thread_attach)(void*)                                     = nullptr;
void        (*il2cpp_thread_detach)(void*)                                     = nullptr;
void**      (*il2cpp_domain_get_assemblies)(void*, size_t*)                    = nullptr;
void*       (*il2cpp_assembly_get_image)(void*)                                = nullptr;
void*       (*il2cpp_class_from_name)(void*, const char*, const char*)         = nullptr;
void*       (*il2cpp_class_get_method_from_name)(void*, const char*, int)      = nullptr;
void*       (*il2cpp_object_get_class)(void*)                                  = nullptr;
void*       (*il2cpp_runtime_invoke)(void*, void*, void**, void**)             = nullptr;
void*       (*il2cpp_class_get_field_from_name)(void*, const char*)            = nullptr;
int32_t     (*il2cpp_field_get_offset)(void*)                                  = nullptr;
size_t      (*il2cpp_image_get_class_count)(void*)                             = nullptr;
void*       (*il2cpp_image_get_class)(void*, size_t)                           = nullptr;
const char* (*il2cpp_class_get_name_raw)(void*)                                = nullptr;

// ─── ELF image info ─────────────────────────────────────────────────────────
struct LoadedImage { uintptr_t base; const ElfW(Phdr)* phdr; size_t phnum; const char* path; };
static LoadedImage g_il2cpp_image = {};
static void* g_il2cpp_handle = nullptr;

static bool ImageContains(uintptr_t address, size_t size, unsigned flags = PF_R) {
    if (!address || !size || !g_il2cpp_image.phdr) return false;
    for (size_t i = 0; i < g_il2cpp_image.phnum; ++i) {
        const ElfW(Phdr)& p = g_il2cpp_image.phdr[i];
        if (p.p_type != PT_LOAD || (p.p_flags & flags) != flags) continue;
        if (p.p_vaddr > UINTPTR_MAX - g_il2cpp_image.base) continue;
        uintptr_t start = g_il2cpp_image.base + p.p_vaddr;
        if (p.p_memsz > UINTPTR_MAX - start || size > p.p_memsz) continue;
        if (address >= start && address - start <= p.p_memsz - size) return true;
    }
    return false;
}

static uintptr_t ImageArrayAddress(uintptr_t address, size_t index, size_t stride) {
    if (!address || !stride || index > (UINTPTR_MAX - address) / stride) return 0;
    return address + index * stride;
}
static bool ImageReadU32(uintptr_t address, uint32_t* value) {
    if (!ImageContains(address, sizeof(*value))) return false;
    memcpy(value, reinterpret_cast<const void*>(address), sizeof(*value));
    return true;
}
static uintptr_t ImageDynamicAddress(ElfW(Addr) value, size_t bytes) {
    if (!value) return 0;
    if (ImageContains(static_cast<uintptr_t>(value), bytes)) return value;
    if (value > UINTPTR_MAX - g_il2cpp_image.base) return 0;
    uintptr_t a = g_il2cpp_image.base + value;
    return ImageContains(a, bytes) ? a : 0;
}

// ─── ELF symbol resolver ────────────────────────────────────────────────────
class Il2CppExports {
    uintptr_t symbols_ = 0, strings_ = 0, sysv_ = 0, gnu_ = 0;
    size_t    string_size_ = 0;

    void* SymbolAt(uint32_t index, const char* name) const {
        uintptr_t address = ImageArrayAddress(symbols_, index, sizeof(ElfW(Sym)));
        if (!ImageContains(address, sizeof(ElfW(Sym)))) return nullptr;
        ElfW(Sym) sym; memcpy(&sym, reinterpret_cast<const void*>(address), sizeof(sym));
        if (sym.st_shndx == SHN_UNDEF || sym.st_name >= string_size_) return nullptr;
        unsigned type = sym.st_info & 0xf, bind = sym.st_info >> 4;
        if ((type != STT_FUNC && type != STT_NOTYPE) || (bind != STB_GLOBAL && bind != STB_WEAK)) return nullptr;
        size_t len = strlen(name) + 1;
        if (len > string_size_ - sym.st_name) return nullptr;
        if (memcmp(reinterpret_cast<const void*>(strings_ + sym.st_name), name, len)) return nullptr;
        uintptr_t fn = sym.st_value;
        if (sym.st_shndx != SHN_ABS) {
            if (fn > UINTPTR_MAX - g_il2cpp_image.base) return nullptr;
            fn += g_il2cpp_image.base;
        }
        return ImageContains(fn, 1, PF_X) ? reinterpret_cast<void*>(fn) : nullptr;
    }
public:
    bool Init() {
        uintptr_t dyn_addr = 0; size_t dyn_count = 0;
        for (size_t i = 0; i < g_il2cpp_image.phnum; ++i) {
            const ElfW(Phdr)& p = g_il2cpp_image.phdr[i];
            if (p.p_type != PT_DYNAMIC || p.p_vaddr > UINTPTR_MAX - g_il2cpp_image.base) continue;
            dyn_addr  = g_il2cpp_image.base + p.p_vaddr;
            dyn_count = p.p_memsz / sizeof(ElfW(Dyn)); break;
        }
        if (!dyn_count || dyn_count > 65536 || !ImageContains(dyn_addr, dyn_count * sizeof(ElfW(Dyn)))) return false;
        const ElfW(Dyn)* d = reinterpret_cast<const ElfW(Dyn)*>(dyn_addr);
        ElfW(Addr) sym = 0, str = 0, sysv = 0, gnu = 0;
        size_t stride = sizeof(ElfW(Sym)); bool terminated = false;
        for (size_t i = 0; i < dyn_count; ++i) {
            if (d[i].d_tag == DT_NULL) { terminated = true; break; }
            switch (d[i].d_tag) {
                case DT_SYMTAB:   sym  = d[i].d_un.d_ptr; break;
                case DT_STRTAB:   str  = d[i].d_un.d_ptr; break;
                case DT_STRSZ:    string_size_ = d[i].d_un.d_val; break;
                case DT_SYMENT:   stride = d[i].d_un.d_val; break;
                case DT_HASH:     sysv = d[i].d_un.d_ptr; break;
                case DT_GNU_HASH: gnu  = d[i].d_un.d_ptr; break;
            }
        }
        if (!terminated || stride != sizeof(ElfW(Sym)) || !string_size_) return false;
        symbols_ = ImageDynamicAddress(sym, sizeof(ElfW(Sym)));
        strings_ = ImageDynamicAddress(str, string_size_);
        sysv_    = ImageDynamicAddress(sysv, 2 * sizeof(uint32_t));
        gnu_     = ImageDynamicAddress(gnu, 4 * sizeof(uint32_t));
        return symbols_ && strings_ && (gnu_ || sysv_);
    }
    void* Find(const char* name) const {
        if (!name || !symbols_ || !strings_) return nullptr;
        if (gnu_) {
            uint32_t bc = 0, fs = 0, blmc = 0;
            ImageReadU32(gnu_, &bc); ImageReadU32(gnu_+4, &fs); ImageReadU32(gnu_+8, &blmc);
            uintptr_t bkts = ImageArrayAddress(gnu_+16, blmc, sizeof(ElfW(Addr)));
            uintptr_t chns = ImageArrayAddress(bkts, bc, sizeof(uint32_t));
            if (bc && blmc && bkts && chns) {
                uint32_t hash = 5381, idx = 0;
                for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p) hash = hash*33+*p;
                if (ImageReadU32(ImageArrayAddress(bkts, hash%bc, 4), &idx) && idx >= fs) {
                    for (size_t s = 0; s < 1048576 && idx != UINT32_MAX; ++s, ++idx) {
                        uint32_t chain = 0;
                        if (!ImageReadU32(ImageArrayAddress(chns, idx-fs, 4), &chain)) break;
                        if ((chain|1u)==(hash|1u)) { void* fn=SymbolAt(idx,name); if (fn) return fn; }
                        if (chain&1u) break;
                    }
                }
            }
        }
        if (sysv_) {
            uint32_t bc = 0, sc = 0;
            ImageReadU32(sysv_, &bc); ImageReadU32(sysv_+4, &sc);
            uintptr_t bkts = sysv_+8, chns = ImageArrayAddress(bkts, bc, sizeof(uint32_t));
            if (bc && sc && chns) {
                uint32_t hash = 0, idx = 0;
                for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p) {
                    hash = (hash<<4)+*p; uint32_t hi = hash&0xf0000000u;
                    if (hi) hash ^= hi>>24; hash &= ~hi;
                }
                if (ImageReadU32(ImageArrayAddress(bkts, hash%bc, 4), &idx)) {
                    for (size_t s = 0; idx && idx<sc && s<1048576; ++s) {
                        void* fn=SymbolAt(idx,name); if (fn) return fn;
                        if (!ImageReadU32(ImageArrayAddress(chns,idx,4),&idx)) break;
                    }
                }
            }
        }
        return nullptr;
    }
};

// ─── isIl2cppLoaded ──────────────────────────────────────────────────────────
bool isIl2cppLoaded() {
    bool found = false;
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* data) -> int {
        const char* path = info->dlpi_name;
        const char* leaf = path ? strrchr(path, '/') : nullptr;
        leaf = leaf ? leaf+1 : path;
        if (!leaf || strcmp(leaf, "libil2cpp.so") != 0 || !info->dlpi_phdr || !info->dlpi_phnum) return 0;
        g_il2cpp_image = {info->dlpi_addr, info->dlpi_phdr, info->dlpi_phnum, path};
        *static_cast<bool*>(data) = true;
        return 1;
    }, &found);
    return found;
}

static void* GetIl2cppHandle() {
    if (g_il2cpp_handle) return g_il2cpp_handle;
    const int flags = RTLD_NOW | RTLD_NOLOAD;
    if (g_il2cpp_image.path && *g_il2cpp_image.path)
        g_il2cpp_handle = dlopen(g_il2cpp_image.path, flags);
    if (!g_il2cpp_handle)
        g_il2cpp_handle = dlopen(OBFUSCATE("libil2cpp.so"), flags);
    return g_il2cpp_handle;
}

bool InitIl2cpp() {
    if (!isIl2cppLoaded()) return false;
    LOGI("[%s] base=%p", BUILD_ID, reinterpret_cast<void*>(g_il2cpp_image.base));
    void* handle = GetIl2cppHandle();
    Il2CppExports exports; const bool has_exp = exports.Init();
    unsigned from_h = 0, from_e = 0, missing = 0;
    auto resolve = [&](const char* name, bool req) -> void* {
        void* fn = handle ? dlsym(handle, name) : nullptr;
        if (fn && ImageContains(reinterpret_cast<uintptr_t>(fn), 1, PF_X)) { ++from_h; return fn; }
        fn = has_exp ? exports.Find(name) : nullptr;
        if (fn) { ++from_e; return fn; }
        if (req) { ++missing; LOGE("Missing: %s", name); }
        return nullptr;
    };
#define LOAD(fn, req) fn = reinterpret_cast<decltype(fn)>(resolve(#fn, req))
    LOAD(il2cpp_domain_get,              true);
    LOAD(il2cpp_thread_attach,           true);
    LOAD(il2cpp_domain_get_assemblies,   true);
    LOAD(il2cpp_assembly_get_image,      true);
    LOAD(il2cpp_class_from_name,         true);
    LOAD(il2cpp_class_get_method_from_name, true);
    LOAD(il2cpp_object_get_class,        true);
    LOAD(il2cpp_runtime_invoke,          true);
    LOAD(il2cpp_thread_detach,           false);
    LOAD(il2cpp_class_get_field_from_name, false);
    LOAD(il2cpp_field_get_offset,        false);
    LOAD(il2cpp_image_get_class_count,   false);
    LOAD(il2cpp_image_get_class,         false);
    il2cpp_class_get_name_raw = reinterpret_cast<decltype(il2cpp_class_get_name_raw)>(resolve("il2cpp_class_get_name", false));
#undef LOAD
    LOGI("[%s] dlsym=%u elf=%u missing=%u", BUILD_ID, from_h, from_e, missing);
    return missing == 0;
}

// ─── Tim_Ham ─────────────────────────────────────────────────────────────────
uintptr_t Tim_Ham(const char* ns, const char* cls, const char* method, int argc) {
    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies ||
        !il2cpp_assembly_get_image || !il2cpp_class_from_name ||
        !il2cpp_class_get_method_from_name) return 0;
    void* domain = il2cpp_domain_get(); if (!domain) return 0;
    size_t size = 0;
    void** assemblies = il2cpp_domain_get_assemblies(domain, &size); if (!assemblies) return 0;
    for (size_t i = 0; i < size; ++i) {
        void* image = il2cpp_assembly_get_image(assemblies[i]); if (!image) continue;
        void* klass = il2cpp_class_from_name(image, ns, cls); if (!klass) continue;
        void* m = il2cpp_class_get_method_from_name(klass, method, argc);
        if (m) return (uintptr_t)*(void**)m;
    }
    return 0;
}

bool DoHook(void* fn, void* hook, void** orig, const char* tag) {
    if (!fn || !hook || !orig || !ImageContains(reinterpret_cast<uintptr_t>(fn), 1, PF_X)) {
        LOGE("Hook SKIP: %s @ %p", tag, fn); return false;
    }
    bool ok = DobbyHook(fn, hook, orig) == 0 && *orig != nullptr;
    LOGI("Hook %s: %s @ %p", ok ? "OK" : "FAIL", tag, fn);
    return ok;
}

// ─── Hook: luaL_loadbufferx ──────────────────────────────────────────────────
// Bắt mọi chunk Lua nạp vào engine.
// Dump ra /sdcard nếu g_dump = true.
typedef int (*lua_State_ptr);
typedef int (*luaL_loadbufferx_t)(void* L, const char* buff, size_t size, const char* name, const char* mode);
static luaL_loadbufferx_t o_luaL_loadbufferx = nullptr;
static int h_luaL_loadbufferx(void* L, const char* buff, size_t size, const char* name, const char* mode) {
    if (g_dump.load(std::memory_order_acquire) && buff && size > 0) {
        char path[512];
        static std::atomic<unsigned> s_seq{0};
        unsigned seq = s_seq.fetch_add(1, std::memory_order_relaxed);
        snprintf(path, sizeof(path), "/sdcard/oni_dump_%04u.luac", seq);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { write(fd, buff, size); close(fd); }
        LOGI("[dump] chunk=%s seq=%u size=%zu", name ? name : "?", seq, size);
    }
    return o_luaL_loadbufferx ? o_luaL_loadbufferx(L, buff, size, name, mode) : 0;
}

// ─── Hook: DebugTool::RegisterDebugToolEntry ─────────────────────────────────
// Chặn debug tool — tắt kênh debug của game.
typedef void (*RegisterDebugToolEntry_t)(void* _this, void* mi);
static RegisterDebugToolEntry_t o_RegisterDebugToolEntry = nullptr;
static void h_RegisterDebugToolEntry(void* _this, void* mi) {
    // Không forward → vô hiệu hoá debug entry
    LOGI("[DebugTool] RegisterDebugToolEntry blocked");
}

// ─── Hook: KCommand::RegisterExecutor ────────────────────────────────────────
typedef void (*RegisterExecutor_t)(void* _this, void* executor, void* mi);
static RegisterExecutor_t o_RegisterExecutor = nullptr;
static void h_RegisterExecutor(void* _this, void* executor, void* mi) {
    // Forward bình thường — chỉ log khi cần debug
    if (o_RegisterExecutor) o_RegisterExecutor(_this, executor, mi);
}

// ─── Hook: KCommand::Exec ────────────────────────────────────────────────────
// Chặn hoặc inject lệnh KCommand.
typedef void* (*KExec_t)(void* _this, void* cmd, void* mi);
static KExec_t o_KExec = nullptr;
static void* h_KExec(void* _this, void* cmd, void* mi) {
    // TODO: xử lý cmd injection nếu cần
    return o_KExec ? o_KExec(_this, cmd, mi) : nullptr;
}

// ─── Hook: KCommand::ExecuteCommand ──────────────────────────────────────────
typedef void* (*ExecuteCommand_t)(void* _this, void* cmd, void* mi);
static ExecuteCommand_t o_ExecuteCommand = nullptr;
static void* h_ExecuteCommand(void* _this, void* cmd, void* mi) {
    return o_ExecuteCommand ? o_ExecuteCommand(_this, cmd, mi) : nullptr;
}

// ─── Hook: Update (main thread nhịp tim) ─────────────────────────────────────
// apply_gm() gốc được gọi mỗi frame khi g_gm = true.
// Hook Update để inject logic mỗi frame.
typedef void (*Update_t)(void* _this, void* mi);
static Update_t o_Update_0 = nullptr;
static void h_Update_0(void* _this, void* mi) {
    if (g_gm.load(std::memory_order_acquire)) {
        // TODO: fill logic GM mode (setHP, setAmmo, v.v.)
        // Ví dụ: call il2cpp_runtime_invoke trên method SetMaxHP
    }
    if (o_Update_0) o_Update_0(_this, mi);
}

// ─── h_t — worker thread ─────────────────────────────────────────────────────
void* h_t(void*) {
    for (int i = 0; i < 120; ++i) {
        if (isIl2cppLoaded()) break;
        sleep(1);
    }
    if (!InitIl2cpp()) { LOGE("[%s] InitIl2cpp FAIL", BUILD_ID); return nullptr; }

    // Chờ assemblies stable
    size_t last = 0; int stable = 0;
    void* domain = il2cpp_domain_get();
    void* thread = (domain && il2cpp_thread_attach) ? il2cpp_thread_attach(domain) : nullptr;
    for (int i = 0; i < 60 && stable < 5; ++i) {
        sleep(1);
        size_t count = 0;
        void** asm_ = il2cpp_domain_get_assemblies ? il2cpp_domain_get_assemblies(domain, &count) : nullptr;
        if (asm_ && count == last && count > 0) ++stable; else { last = count; stable = 0; }
    }
    if (stable < 5) { LOGE("Assemblies not ready"); if (il2cpp_thread_detach) il2cpp_thread_detach(thread); return nullptr; }
    LOGI("Assemblies stable: %zu", last);

    unsigned installed = 0;

    // 1. luaL_loadbufferx — tìm trong libil2cpp.so hoặc thư viện Lua riêng
    {
        void* lua_handle = dlopen(OBFUSCATE("libil2cpp.so"), RTLD_NOW | RTLD_NOLOAD);
        void* sym = lua_handle ? dlsym(lua_handle, OBFUSCATE("luaL_loadbufferx")) : nullptr;
        if (!sym) {
            // Game có thể nhúng Lua trong main lib
            void* main_handle = dlopen(OBFUSCATE("libMain.so"), RTLD_NOW | RTLD_NOLOAD);
            if (main_handle) sym = dlsym(main_handle, OBFUSCATE("luaL_loadbufferx"));
        }
        if (sym) installed += DoHook(sym, (void*)h_luaL_loadbufferx, (void**)&o_luaL_loadbufferx, "luaL_loadbufferx");
        else LOGE("luaL_loadbufferx not found — Lua lib may be stripped");
    }

    // 2. DebugTool::RegisterDebugToolEntry
    {
        uintptr_t addr = Tim_Ham(NS_MAIN, OBFUSCATE("DebugTool"), OBFUSCATE("RegisterDebugToolEntry"), 0);
        if (addr) installed += DoHook((void*)addr, (void*)h_RegisterDebugToolEntry, (void**)&o_RegisterDebugToolEntry, "DebugTool::RegisterDebugToolEntry");
    }

    // 3. KCommand::RegisterExecutor
    {
        uintptr_t addr = Tim_Ham(NS_MAIN, OBFUSCATE("KCommand"), OBFUSCATE("RegisterExecutor"), -1);
        if (addr) installed += DoHook((void*)addr, (void*)h_RegisterExecutor, (void**)&o_RegisterExecutor, "KCommand::RegisterExecutor");
    }

    // 4. KCommand::Exec
    {
        uintptr_t addr = Tim_Ham(NS_MAIN, OBFUSCATE("KCommand"), OBFUSCATE("Exec"), 1);
        if (addr) installed += DoHook((void*)addr, (void*)h_KExec, (void**)&o_KExec, "KCommand::Exec");
    }

    // 5. KCommand::ExecuteCommand
    {
        uintptr_t addr = Tim_Ham(NS_MAIN, OBFUSCATE("KCommand"), OBFUSCATE("ExecuteCommand"), 1);
        if (addr) installed += DoHook((void*)addr, (void*)h_ExecuteCommand, (void**)&o_ExecuteCommand, "KCommand::ExecuteCommand");
    }

    // 6. Update (main thread heartbeat — tương tự place_update() trong onimod)
    // Thử nhiều class có Update — onimod hook 4 class (64 bytes / 16 byte mỗi entry)
    static const char* update_classes[] = {
        OBFUSCATE("PlayerController"),
        OBFUSCATE("GameManager"),
        OBFUSCATE("BattleManager"),
        OBFUSCATE("HeroController"),
    };
    for (const char* cls : update_classes) {
        uintptr_t addr = Tim_Ham(NS_MAIN, cls, OBFUSCATE("Update"), 0);
        if (addr) {
            installed += DoHook((void*)addr, (void*)h_Update_0, (void**)&o_Update_0, cls);
            break; // chỉ hook class Update đầu tiên tìm thấy; thêm o_Update_N nếu cần nhiều
        }
    }

    LOGI("[%s] DONE: %u hook installed", BUILD_ID, installed);
    if (il2cpp_thread_detach) il2cpp_thread_detach(thread);
    return nullptr;
}

// ─── Doc_Config_Ngam — đọc config HAMOD ──────────────────────────────────────
// HAMOD ghi: files/mods/config_<pkg>.txt
// Format: "f0 f1 f2 f3 f4 f5 f6" (7 fields, space-separated float)
//   f0 = g_dump    (0/1)
//   f1 = g_dmg     (0/1)
//   f2 = g_dmgmul  (1..20)
//   f3 = g_god     (0/1)
//   f4 = g_gm      (0/1)
static bool ParseConfig(const char* text) {
    if (!text) return false;
    float v[7] = {}; unsigned count = 0;
    const char* c = text;
    while (true) {
        while (isspace((unsigned char)*c)) ++c;
        if (!*c) break;
        if (count == 7) return false;
        char* end = nullptr; errno = 0;
        float val = strtof(c, &end);
        if (end == c || errno == ERANGE || !std::isfinite(val) ||
            (*end && !isspace((unsigned char)*end))) return false;
        v[count++] = val;
        c = end;
    }
    if (count < 5) return false;
    g_dump   .store(v[0] >= 0.5f, std::memory_order_release);
    g_dmg    .store(v[1] >= 0.5f, std::memory_order_release);
    float mul = v[2] < 1.0f ? 1.0f : (v[2] > 20.0f ? 20.0f : v[2]);
    g_dmgmul .store(mul, std::memory_order_release);
    g_god    .store(v[3] >= 0.5f, std::memory_order_release);
    g_gm     .store(v[4] >= 0.5f, std::memory_order_release);
    return true;
}

void* Doc_Config_Ngam(void*) {
    const char* root = getenv("MODMENU_LOG_DIR");
    if (!root || !*root) root = "/data/data/menu.nextgen.launcher/files";
    char paths[2][1024];
    snprintf(paths[0], sizeof(paths[0]), "%s/mods/config_" PKG_NAME ".txt", root);
    snprintf(paths[1], sizeof(paths[1]), "%s/config_"      PKG_NAME ".txt", root);
    LOGI("[%s] config path[0]=%s", BUILD_ID, paths[0]);
    int last_status = -1;
    while (true) {
        FILE* file = nullptr; int sel = -1;
        for (int i = 0; i < 2; ++i) {
            file = fopen(paths[i], "r");
            if (file) { sel = i; break; }
        }
        if (!file) { if (last_status != 0) LOGE("Config missing"); last_status = 0; usleep(500000); continue; }
        char text[256] = {};
        size_t len = fread(text, 1, sizeof(text)-1, file);
        bool ok = !ferror(file) && len < sizeof(text)-1;
        fclose(file);
        if (!ok || memchr(text, '\0', len) || !ParseConfig(text)) {
            if (last_status != 1) LOGE("Config invalid: %s", paths[sel]);
            last_status = 1; usleep(500000); continue;
        }
        g_config_loaded.store(true, std::memory_order_release);
        if (last_status != 2)
            LOGI("Config OK: dump=%d dmg=%d mul=%.1f god=%d gm=%d",
                 (int)g_dump.load(), (int)g_dmg.load(), g_dmgmul.load(),
                 (int)g_god.load(), (int)g_gm.load());
        last_status = 2;
        usleep(500000);
    }
    return nullptr;
}

// ─── Entry point ─────────────────────────────────────────────────────────────
__attribute__((constructor))
void l_m() {
    LOGI("[%s] injected!", BUILD_ID);
    pthread_t t1, t2;
    if (pthread_create(&t1, nullptr, h_t,             nullptr) == 0) pthread_detach(t1);
    if (pthread_create(&t2, nullptr, Doc_Config_Ngam, nullptr) == 0) pthread_detach(t2);
}
