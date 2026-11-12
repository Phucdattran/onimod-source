// ============================================================
//  HAMOD MOD — com.yibu.CapybaraSurvivorGOGOGO v10.0.1
//  Features: Speed (1-10x) + Dame xN + Free Shop
//  menu.js: index 0=Speed(seekbar,10) | 1=Dame(seekbar,3) | 2=FreeShop(toggle)
// ============================================================

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

#define BUILD_ID "CAPYBARA_v1"
#define LOG_TAG  "CAPYBARA_MOD"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// === TRANG THAI MENU (index theo menu.js) ===
static std::atomic<float> g_speed{1.0f};        // index 0: 1-10x
static std::atomic<int>   g_dame_idx{0};         // index 1: 0=off,1=x5,2=x10,3=x20
static std::atomic<bool>  g_free_shop{false};    // index 2
static std::atomic<bool>  g_config_loaded_once{false};
static std::atomic<float> g_gamescale{1.0f};     // game's natural time scale

// === IL2CPP API ===
static void*       (*il2cpp_domain_get)()                                        = nullptr;
static void*       (*il2cpp_thread_attach)(void*)                                = nullptr;
static void        (*il2cpp_thread_detach)(void*)                                = nullptr;
static void**      (*il2cpp_domain_get_assemblies)(void*, size_t*)               = nullptr;
static void*       (*il2cpp_assembly_get_image)(void*)                           = nullptr;
static void*       (*il2cpp_class_from_name)(void*, const char*, const char*)    = nullptr;
static void*       (*il2cpp_class_get_methods)(void*, void**)                    = nullptr;
static const char* (*il2cpp_method_get_name)(void*)                              = nullptr;
static uint32_t    (*il2cpp_method_get_param_count)(void*)                       = nullptr;
static void*       (*il2cpp_class_get_method_from_name)(void*, const char*, int) = nullptr;
static void*       (*il2cpp_object_get_class)(void*)                             = nullptr;
static void*       (*il2cpp_runtime_invoke)(void*, void*, void**, void**)        = nullptr;
static void*       (*il2cpp_class_get_field_from_name)(void*, const char*)       = nullptr;
static int32_t     (*il2cpp_field_get_offset)(void*)                             = nullptr;
static size_t      (*il2cpp_image_get_class_count)(void*)                        = nullptr;
static void*       (*il2cpp_image_get_class)(void*, size_t)                      = nullptr;
static const char* (*il2cpp_class_get_name_raw)(void*)                           = nullptr;
static void*       (*il2cpp_method_get_param)(void*, uint32_t)                   = nullptr;
static int         (*il2cpp_type_get_type)(void*)                                = nullptr;
static bool        (*il2cpp_type_is_byref)(void*)                                = nullptr;
static void        (*il2cpp_format_exception)(const void*, char*, int)           = nullptr;

// === ELF IMAGE ===
struct LoadedIl2CppImage {
    uintptr_t         base;
    const ElfW(Phdr)* phdr;
    size_t            phnum;
    const char*       path;
};
static LoadedIl2CppImage g_il2cpp_image = {};
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
    uintptr_t address = g_il2cpp_image.base + value;
    return ImageContains(address, bytes) ? address : 0;
}

// === ELF SYMBOL RESOLVER (khong dung DobbySymbolResolver) ===
class Il2CppExports {
    uintptr_t symbols_ = 0, strings_ = 0, sysv_ = 0, gnu_ = 0;
    size_t string_size_ = 0;

    void* SymbolAt(uint32_t index, const char* name) const {
        uintptr_t address = ImageArrayAddress(symbols_, index, sizeof(ElfW(Sym)));
        if (!ImageContains(address, sizeof(ElfW(Sym)))) return nullptr;
        ElfW(Sym) symbol;
        memcpy(&symbol, reinterpret_cast<const void*>(address), sizeof(symbol));
        if (symbol.st_shndx == SHN_UNDEF || symbol.st_name >= string_size_) return nullptr;
        unsigned type = symbol.st_info & 0xf, binding = symbol.st_info >> 4;
        if ((type != STT_FUNC && type != STT_NOTYPE) ||
            (binding != STB_GLOBAL && binding != STB_WEAK)) return nullptr;
        size_t length = strlen(name) + 1;
        if (length > string_size_ - symbol.st_name) return nullptr;
        if (memcmp(reinterpret_cast<const void*>(strings_ + symbol.st_name), name, length)) return nullptr;
        uintptr_t fn = symbol.st_value;
        if (symbol.st_shndx != SHN_ABS) {
            if (fn > UINTPTR_MAX - g_il2cpp_image.base) return nullptr;
            fn += g_il2cpp_image.base;
        }
        return ImageContains(fn, 1, PF_X) ? reinterpret_cast<void*>(fn) : nullptr;
    }

public:
    bool Init() {
        uintptr_t dynamic_address = 0; size_t count = 0;
        for (size_t i = 0; i < g_il2cpp_image.phnum; ++i) {
            const ElfW(Phdr)& p = g_il2cpp_image.phdr[i];
            if (p.p_type != PT_DYNAMIC || p.p_vaddr > UINTPTR_MAX - g_il2cpp_image.base) continue;
            dynamic_address = g_il2cpp_image.base + p.p_vaddr;
            count = p.p_memsz / sizeof(ElfW(Dyn)); break;
        }
        if (!count || count > 65536 || !ImageContains(dynamic_address, count * sizeof(ElfW(Dyn)))) return false;
        const ElfW(Dyn)* dynamic = reinterpret_cast<const ElfW(Dyn)*>(dynamic_address);
        ElfW(Addr) sym = 0, str = 0, sysv = 0, gnu = 0;
        size_t stride = sizeof(ElfW(Sym)); bool terminated = false;
        for (size_t i = 0; i < count; ++i) {
            if (dynamic[i].d_tag == DT_NULL) { terminated = true; break; }
            switch (dynamic[i].d_tag) {
                case DT_SYMTAB:   sym  = dynamic[i].d_un.d_ptr; break;
                case DT_STRTAB:   str  = dynamic[i].d_un.d_ptr; break;
                case DT_STRSZ:    string_size_ = dynamic[i].d_un.d_val; break;
                case DT_SYMENT:   stride = dynamic[i].d_un.d_val; break;
                case DT_HASH:     sysv = dynamic[i].d_un.d_ptr; break;
                case DT_GNU_HASH: gnu  = dynamic[i].d_un.d_ptr; break;
            }
        }
        if (!terminated || stride != sizeof(ElfW(Sym)) || !string_size_) return false;
        symbols_ = ImageDynamicAddress(sym, sizeof(ElfW(Sym)));
        strings_ = ImageDynamicAddress(str, string_size_);
        sysv_    = ImageDynamicAddress(sysv, 2 * sizeof(uint32_t));
        gnu_     = ImageDynamicAddress(gnu,  4 * sizeof(uint32_t));
        return symbols_ && strings_ && (gnu_ || sysv_);
    }

    void* Find(const char* name) const {
        if (!name || !symbols_ || !strings_) return nullptr;
        if (gnu_) {
            uint32_t bucket_count = 0, first_symbol = 0, bloom_count = 0;
            ImageReadU32(gnu_, &bucket_count); ImageReadU32(gnu_ + 4, &first_symbol);
            ImageReadU32(gnu_ + 8, &bloom_count);
            uintptr_t buckets = ImageArrayAddress(gnu_ + 16, bloom_count, sizeof(ElfW(Addr)));
            uintptr_t chains  = ImageArrayAddress(buckets, bucket_count, sizeof(uint32_t));
            if (bucket_count && bloom_count && buckets && chains) {
                uint32_t hash = 5381, index = 0;
                for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p)
                    hash = hash * 33 + *p;
                if (ImageReadU32(ImageArrayAddress(buckets, hash % bucket_count, 4), &index) && index >= first_symbol) {
                    for (size_t step = 0; step < 1048576 && index != UINT32_MAX; ++step, ++index) {
                        uint32_t chain = 0;
                        if (!ImageReadU32(ImageArrayAddress(chains, index - first_symbol, 4), &chain)) break;
                        if ((chain | 1u) == (hash | 1u)) { void* fn = SymbolAt(index, name); if (fn) return fn; }
                        if (chain & 1u) break;
                    }
                }
            }
        }
        if (sysv_) {
            uint32_t bucket_count = 0, symbol_count = 0;
            ImageReadU32(sysv_, &bucket_count); ImageReadU32(sysv_ + 4, &symbol_count);
            uintptr_t buckets = sysv_ + 8;
            uintptr_t chains  = ImageArrayAddress(buckets, bucket_count, sizeof(uint32_t));
            if (bucket_count && symbol_count && chains) {
                uint32_t hash = 0, index = 0;
                for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p) {
                    hash = (hash << 4) + *p; uint32_t high = hash & 0xf0000000u;
                    if (high) hash ^= high >> 24; hash &= ~high;
                }
                if (ImageReadU32(ImageArrayAddress(buckets, hash % bucket_count, 4), &index)) {
                    for (size_t step = 0; index && index < symbol_count && step < 1048576; ++step) {
                        void* fn = SymbolAt(index, name); if (fn) return fn;
                        if (!ImageReadU32(ImageArrayAddress(chains, index, 4), &index)) break;
                    }
                }
            }
        }
        return nullptr;
    }
};

// === IL2CPP LOAD ===
static bool isIl2cppLoaded() {
    bool found = false;
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* data) -> int {
        const char* path = info->dlpi_name;
        const char* leaf = path ? strrchr(path, '/') : nullptr;
        leaf = leaf ? leaf + 1 : path;
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
    if (!g_il2cpp_handle) LOGI("GetHandle unavailable; will use mapped ELF exports");
    return g_il2cpp_handle;
}

static bool InitIl2cpp() {
    if (!isIl2cppLoaded()) return false;
    LOGI("[%s] Resolve BEGIN: base=%p", BUILD_ID, reinterpret_cast<void*>(g_il2cpp_image.base));
    void* handle = GetIl2cppHandle();
    Il2CppExports exports; const bool has_exports = exports.Init();
    unsigned from_handle = 0, from_image = 0, missing = 0;
    auto resolve = [&](const char* name, bool required) -> void* {
        void* fn = handle ? dlsym(handle, name) : nullptr;
        if (fn && ImageContains(reinterpret_cast<uintptr_t>(fn), 1, PF_X)) { ++from_handle; return fn; }
        fn = has_exports ? exports.Find(name) : nullptr;
        if (fn) { ++from_image; return fn; }
        if (required) { ++missing; LOGE("Required API missing: %s", name); }
        return nullptr;
    };
#define LOAD_API(fn, required) fn = reinterpret_cast<decltype(fn)>(resolve(#fn, required))
    LOAD_API(il2cpp_domain_get,              true);
    LOAD_API(il2cpp_thread_attach,           true);
    LOAD_API(il2cpp_domain_get_assemblies,   true);
    LOAD_API(il2cpp_assembly_get_image,      true);
    LOAD_API(il2cpp_class_from_name,         true);
    LOAD_API(il2cpp_class_get_method_from_name, true);
    LOAD_API(il2cpp_object_get_class,        true);
    LOAD_API(il2cpp_runtime_invoke,          true);
    LOAD_API(il2cpp_thread_detach,           false);
    LOAD_API(il2cpp_class_get_methods,       false);
    LOAD_API(il2cpp_method_get_name,         false);
    LOAD_API(il2cpp_method_get_param_count,  false);
    LOAD_API(il2cpp_method_get_param,        false);
    LOAD_API(il2cpp_type_get_type,           false);
    LOAD_API(il2cpp_type_is_byref,           false);
    LOAD_API(il2cpp_format_exception,        false);
    LOAD_API(il2cpp_class_get_field_from_name, false);
    LOAD_API(il2cpp_field_get_offset,        false);
    LOAD_API(il2cpp_image_get_class_count,   false);
    LOAD_API(il2cpp_image_get_class,         false);
    il2cpp_class_get_name_raw = reinterpret_cast<decltype(il2cpp_class_get_name_raw)>(
        resolve("il2cpp_class_get_name", false));
#undef LOAD_API
    LOGI("[%s] Resolve END: handle=%u mappedELF=%u missing=%u", BUILD_ID, from_handle, from_image, missing);
    return missing == 0;
}

// === HELPERS ===
static uintptr_t Tim_Ham(const char* ns, const char* className, const char* methodName, int argsCount) {
    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies || !il2cpp_assembly_get_image ||
        !il2cpp_class_from_name || !il2cpp_class_get_method_from_name) return 0;
    void* domain = il2cpp_domain_get(); if (!domain) return 0;
    size_t size = 0; void** assemblies = il2cpp_domain_get_assemblies(domain, &size);
    if (!assemblies) return 0;
    for (size_t i = 0; i < size; ++i) {
        void* image = il2cpp_assembly_get_image(assemblies[i]); if (!image) continue;
        void* klass = il2cpp_class_from_name(image, ns, className);
        if (klass) {
            void* method = il2cpp_class_get_method_from_name(klass, methodName, argsCount);
            if (method) return (uintptr_t)*(void**)method;
        }
    }
    return 0;
}

static void* FindClassAnywhere(const char* className) {
    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies || !il2cpp_assembly_get_image) return nullptr;
    void* domain = il2cpp_domain_get(); if (!domain) return nullptr;
    size_t nasm = 0; void** assemblies = il2cpp_domain_get_assemblies(domain, &nasm);
    for (size_t i = 0; i < nasm; i++) {
        void* img = il2cpp_assembly_get_image(assemblies[i]); if (!img) continue;
        if (il2cpp_image_get_class && il2cpp_image_get_class_count && il2cpp_class_get_name_raw) {
            size_t nc = il2cpp_image_get_class_count(img);
            for (size_t j = 0; j < nc; j++) {
                void* kls = il2cpp_image_get_class(img, j); if (!kls) continue;
                const char* n = il2cpp_class_get_name_raw(kls);
                if (n && strcmp(n, className) == 0) return kls;
            }
        } else {
            void* klass = il2cpp_class_from_name(img, "", className);
            if (klass) return klass;
        }
    }
    LOGE("FindClassAnywhere[%s] NOT FOUND", className);
    return nullptr;
}

// Dung cho private methods (il2cpp_class_get_method_from_name co the bo qua private)
static void* FindMethod(void* klass, const char* name, int nParams) {
    if (!klass || !il2cpp_class_get_methods || !il2cpp_method_get_name) return nullptr;
    void* iter = nullptr;
    while (void* m = il2cpp_class_get_methods(klass, &iter)) {
        const char* n = il2cpp_method_get_name(m); if (!n || strcmp(n, name) != 0) continue;
        if (nParams >= 0 && il2cpp_method_get_param_count &&
            (int)il2cpp_method_get_param_count(m) != nParams) continue;
        return m;
    }
    return nullptr;
}

static bool DoHook(void* fn, void* hook, void** orig, const char* tag) {
    if (!hook || !orig || !ImageContains(reinterpret_cast<uintptr_t>(fn), 1, PF_X)) {
        LOGE("Hook SKIP (invalid address): %s @ %p", tag, fn); return false;
    }
    const int result = DobbyHook(fn, hook, orig);
    const bool ok = result == 0 && *orig != nullptr;
    LOGI("Hook %s: %s @ %p", ok ? "OK" : "FAIL", tag, fn);
    return ok;
}

template <typename T> static inline T& Field(void* obj, int off) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(obj) + off);
}

// === SPEED — UnityEngine.Time ===
static void  (*o_set_timeScale)(float, void*) = nullptr;
static float (*o_get_timeScale)(void*)         = nullptr;

static void h_set_timeScale(float v, void* mi) {
    g_gamescale.store(v, std::memory_order_release);
    float speed = g_speed.load(std::memory_order_acquire);
    if (o_set_timeScale) o_set_timeScale(v * speed, mi);
}
static float h_get_timeScale(void* mi) {
    if (g_speed.load(std::memory_order_acquire) != 1.0f)
        return g_gamescale.load(std::memory_order_acquire);
    return o_get_timeScale ? o_get_timeScale(mi) : g_gamescale.load(std::memory_order_acquire);
}

// === DAME — Fomula class (AtkPack.damage @ 0x10) ===
// Dame idx: 0=off(1x), 1=x5, 2=x10, 3=x20
static const int kDameMults[] = {1, 5, 10, 20};

static inline void MultDmg(void* pack) {
    int idx = g_dame_idx.load(std::memory_order_acquire);
    if (!pack || idx <= 0 || idx > 3) return;
    Field<int64_t>(pack, 0x10) *= kDameMults[idx];
}

typedef void* (*t_GetAtk1)(int32_t, void*);
typedef void* (*t_GetAtk0)(void*);
typedef void* (*t_GetFriendAtk4)(int32_t, int32_t, int32_t, int32_t, void*);
typedef void* (*t_GetFriendSkill5)(int32_t, int32_t, int32_t, int32_t, bool, void*);

static t_GetAtk1        o_GetPlayerAtk       = nullptr;
static t_GetAtk1        o_GetPlayerSkill     = nullptr;
static t_GetAtk0        o_GetPlayerSword     = nullptr;
static t_GetAtk0        o_GetPlayerLightning = nullptr;
static t_GetFriendAtk4  o_GetFriendAtk       = nullptr;
static t_GetFriendSkill5 o_GetFriendSkill    = nullptr;

static void* h_GetPlayerAtk(int32_t r, void* mi)      { void* p=o_GetPlayerAtk(r,mi);            MultDmg(p); return p; }
static void* h_GetPlayerSkill(int32_t r, void* mi)    { void* p=o_GetPlayerSkill(r,mi);           MultDmg(p); return p; }
static void* h_GetPlayerSword(void* mi)               { void* p=o_GetPlayerSword(mi);             MultDmg(p); return p; }
static void* h_GetPlayerLightning(void* mi)           { void* p=o_GetPlayerLightning(mi);         MultDmg(p); return p; }
static void* h_GetFriendAtk(int32_t id, int32_t at, int32_t bp, int32_t ar, void* mi) {
    void* p=o_GetFriendAtk(id,at,bp,ar,mi); MultDmg(p); return p;
}
static void* h_GetFriendSkill(int32_t sr, int32_t id, int32_t at, int32_t bp, bool crit, void* mi) {
    void* p=o_GetFriendSkill(sr,id,at,bp,crit,mi); MultDmg(p); return p;
}

// === FREE SHOP — PayManager.PayAct ===
// PayAct(productId, priceId, yesAction) [3 params] mo Google Play dialog
// GrantProductRewardById(productId, out payAmount) [2 params, private] cap do
typedef void (*t_PayAct)(void* self, void* productId, int32_t priceId, void* yesAction, void* mi);
typedef bool (*t_GrantRewardById)(void* self, void* productId, float* payAmount, void* mi);

static t_PayAct          o_PayAct         = nullptr;
static t_GrantRewardById g_GrantRewardById = nullptr;

static void h_PayAct(void* self, void* productId, int32_t priceId, void* yesAction, void* mi) {
    if (!g_free_shop.load(std::memory_order_acquire) || !self) {
        if (o_PayAct) o_PayAct(self, productId, priceId, yesAction, mi);
        return;
    }
    float payAmount = 0.0f;
    bool ok = g_GrantRewardById ? g_GrantRewardById(self, productId, &payAmount, nullptr) : false;
    LOGI("FreeShop: GrantRewardById -> %s amount=%.2f", ok ? "OK" : "FAIL", payAmount);
    if (yesAction && il2cpp_object_get_class && il2cpp_runtime_invoke) {
        void* cls = il2cpp_object_get_class(yesAction);
        void* invM = cls ? FindMethod(cls, "Invoke", 0) : nullptr;
        if (invM) { void* exc = nullptr; il2cpp_runtime_invoke(invM, yesAction, nullptr, &exc); }
    }
}

// === WORKER THREAD — hook install ===
static void* h_t(void*) {
    for (int i = 0; i < 120; ++i) { if (isIl2cppLoaded()) break; sleep(1); }
    if (!InitIl2cpp()) { LOGE("[%s] InitIl2cpp FAILED", BUILD_ID); return nullptr; }

    size_t last = 0; int stable = 0;
    void* domain = il2cpp_domain_get();
    void* thread = (domain && il2cpp_thread_attach) ? il2cpp_thread_attach(domain) : nullptr;
    for (int i = 0; i < 60 && stable < 5; ++i) {
        sleep(1);
        size_t count = 0;
        void** asm_ = il2cpp_domain_get_assemblies ? il2cpp_domain_get_assemblies(domain, &count) : nullptr;
        if (asm_ && count == last && count > 0) ++stable; else { last = count; stable = 0; }
    }
    if (stable < 5) {
        LOGE("STOP: assemblies not ready");
        if (il2cpp_thread_detach) il2cpp_thread_detach(thread);
        return nullptr;
    }
    LOGI("Assemblies stable: %zu", last);

    unsigned installed = 0;

    // Speed — Time.set/get_timeScale
    uintptr_t addr_setTS = Tim_Ham(OBFUSCATE("UnityEngine"), OBFUSCATE("Time"), OBFUSCATE("set_timeScale"), 1);
    uintptr_t addr_getTS = Tim_Ham(OBFUSCATE("UnityEngine"), OBFUSCATE("Time"), OBFUSCATE("get_timeScale"), 0);
    installed += DoHook((void*)addr_setTS, (void*)h_set_timeScale, (void**)&o_set_timeScale, "Time.set_timeScale");
    installed += DoHook((void*)addr_getTS, (void*)h_get_timeScale, (void**)&o_get_timeScale, "Time.get_timeScale");
    if (o_get_timeScale) g_gamescale.store(o_get_timeScale(nullptr), std::memory_order_release);

    // Dame — Fomula class attack-side multiplier
    uintptr_t addr;
    addr = Tim_Ham("", OBFUSCATE("Fomula"), OBFUSCATE("GetPlayerAtk"),   1);
    installed += DoHook((void*)addr, (void*)h_GetPlayerAtk,   (void**)&o_GetPlayerAtk,   "Fomula.GetPlayerAtk");
    addr = Tim_Ham("", OBFUSCATE("Fomula"), OBFUSCATE("GetPlayerSkill"),  1);
    installed += DoHook((void*)addr, (void*)h_GetPlayerSkill,  (void**)&o_GetPlayerSkill,  "Fomula.GetPlayerSkill");
    addr = Tim_Ham("", OBFUSCATE("Fomula"), OBFUSCATE("GetPlayerSword"),  0);
    installed += DoHook((void*)addr, (void*)h_GetPlayerSword,  (void**)&o_GetPlayerSword,  "Fomula.GetPlayerSword");
    addr = Tim_Ham("", OBFUSCATE("Fomula"), OBFUSCATE("GetPlayerLightning"), 0);
    installed += DoHook((void*)addr, (void*)h_GetPlayerLightning, (void**)&o_GetPlayerLightning, "Fomula.GetPlayerLightning");
    addr = Tim_Ham("", OBFUSCATE("Fomula"), OBFUSCATE("GetFriendAtk"),   4);
    installed += DoHook((void*)addr, (void*)h_GetFriendAtk,   (void**)&o_GetFriendAtk,   "Fomula.GetFriendAtk");
    addr = Tim_Ham("", OBFUSCATE("Fomula"), OBFUSCATE("GetFriendSkill"),  5);
    installed += DoHook((void*)addr, (void*)h_GetFriendSkill,  (void**)&o_GetFriendSkill,  "Fomula.GetFriendSkill");

    // Free Shop — PayManager.PayAct + cache GrantProductRewardById (private)
    {
        void* pmKlass = FindClassAnywhere(OBFUSCATE("PayManager"));
        if (pmKlass) {
            void* grantM = FindMethod(pmKlass, OBFUSCATE("GrantProductRewardById"), 2);
            if (grantM) g_GrantRewardById = (t_GrantRewardById)*(void**)grantM;
            LOGI("GrantRewardById: %p", (void*)g_GrantRewardById);
        }
        uintptr_t addr_payact = Tim_Ham("", OBFUSCATE("PayManager"), OBFUSCATE("PayAct"), 3);
        installed += DoHook((void*)addr_payact, (void*)h_PayAct, (void**)&o_PayAct, "PayManager.PayAct");
    }

    LOGI("[%s] HOOK DONE: %u installed", BUILD_ID, installed);
    if (il2cpp_thread_detach) il2cpp_thread_detach(thread);
    return nullptr;
}

// === PARSE CONFIG ===
static bool ParseMenuConfig(const char* text, float f[7], unsigned* count_out) {
    if (!text || !f || !count_out) return false;
    float values[7] = {}; unsigned count = 0;
    const char* cursor = text;
    while (true) {
        while (isspace(static_cast<unsigned char>(*cursor))) ++cursor;
        if (!*cursor) break;
        if (count == 7) return false;
        char* end = nullptr; errno = 0;
        const float value = strtof(cursor, &end);
        if (end == cursor || errno == ERANGE || !std::isfinite(value) ||
            (*end && !isspace(static_cast<unsigned char>(*end)))) return false;
        values[count++] = value;
        cursor = end;
    }
    if (count != 1 && count != 2 && count != 7) return false;
    memcpy(f, values, count * sizeof(float));
    *count_out = count;
    return true;
}

// === CONFIG THREAD ===
static const char* const kConfigPaths[] = {
    "/data/data/com.hamod.vn/files/mods/.virtual-users/u0/com.yibu.CapybaraSurvivorGOGOGO/config.txt",
    "/data/user/0/com.hamod.vn/files/mods/.virtual-users/u0/com.yibu.CapybaraSurvivorGOGOGO/config.txt",
    "/data/data/com.hamod.vn/files/mods/config_com.yibu.CapybaraSurvivorGOGOGO.txt",
    "/data/user/0/com.hamod.vn/files/mods/config_com.yibu.CapybaraSurvivorGOGOGO.txt",
    nullptr
};

static void* Doc_Config_Ngam(void*) {
    LOGI("[%s] CONFIG begin: pid=%d", BUILD_ID, getpid());
    for (int i = 0; kConfigPaths[i]; ++i) LOGI("CONFIG candidate[%d]=%s", i, kConfigPaths[i]);

    float last_f[7] = {}; int last_status = -1, last_path = -1;
    float s_last_speed = -1.0f;

    while (true) {
        FILE* file = nullptr; int selected = -1;
        for (int i = 0; kConfigPaths[i]; ++i) {
            file = fopen(kConfigPaths[i], "r");
            if (file) { selected = i; break; }
        }
        if (!file) {
            if (last_status != 0) LOGE("CONFIG missing; using default values");
            last_status = 0; usleep(500000); continue;
        }
        char text[256] = {};
        const size_t length = fread(text, 1, sizeof(text) - 1, file);
        const bool read_ok = !ferror(file) && length < sizeof(text) - 1;
        fclose(file);

        float f[7] = {}; unsigned count = 0;
        memcpy(f, last_f, sizeof(f));
        if (!read_ok || memchr(text, '\0', length) || !ParseMenuConfig(text, f, &count)) {
            if (last_status != 1 || last_path != selected)
                LOGE("CONFIG invalid @ %s", kConfigPaths[selected]);
            last_status = 1; last_path = selected; usleep(500000); continue;
        }

        // index 0: Speed seekbar (1-10, clamp)
        float speed = f[0] < 1.0f ? 1.0f : (f[0] > 10.0f ? 10.0f : f[0]);
        g_speed.store(speed, std::memory_order_release);

        // index 1: Dame seekbar (0-3)
        int didx = (int)f[1]; if (didx < 0) didx = 0; if (didx > 3) didx = 3;
        g_dame_idx.store(didx, std::memory_order_release);

        // index 2: Free Shop toggle
        g_free_shop.store(f[2] == 1.0f, std::memory_order_release);

        // Speed active apply — goi chu dong khi speed thay doi
        if (o_set_timeScale && speed != s_last_speed) {
            float gs = g_gamescale.load(std::memory_order_acquire);
            o_set_timeScale(gs * speed, nullptr);
            s_last_speed = speed;
            LOGI("[Speed] Applied timeScale=%.2f (game=%.2f x mod=%.1fx)", gs * speed, gs, speed);
        }

        g_config_loaded_once.store(true, std::memory_order_release);
        if (last_status != 2 || last_path != selected || memcmp(f, last_f, sizeof(f)))
            LOGI("CONFIG ok: %s | Speed=%.0fx Dame=%d FreeShop=%d (fields=%u)",
                 kConfigPaths[selected], speed, didx, (int)(f[2]==1.0f), count);
        last_status = 2; last_path = selected;
        memcpy(last_f, f, sizeof(f));
        usleep(500000);
    }
    return nullptr;
}

// === ENTRY POINT ===
__attribute__((constructor))
void l_m() {
    LOGI("[%s] MOD injected!", BUILD_ID);
    pthread_t t1, t2;
    int rc = pthread_create(&t1, nullptr, h_t, nullptr);
    if (rc == 0) pthread_detach(t1); else LOGE("Worker create failed: %d", rc);
    rc = pthread_create(&t2, nullptr, Doc_Config_Ngam, nullptr);
    if (rc == 0) pthread_detach(t2); else LOGE("Config thread create failed: %d", rc);
}
