// ============================================================
//  HAMOD MOD — Winter Breaker (com.winter.breaker)
//  Speed · Free Shop · Remove Ads · Damage
//  menu.js: index 0=speed(1-10) 1=shop 2=noads 3=damage(1-20)
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

#define BUILD_ID "WB_v1"
#define LOG_TAG  "WINTERBREAKER_HAMOD"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ═══════════════════════════════════════════════════════════
//  TRẠNG THÁI — std::atomic (index khớp menu.js)
// ═══════════════════════════════════════════════════════════
static std::atomic<float> g_speed{1.0f};           // index 0  seekbar 1-10
static std::atomic<bool>  g_shop{false};            // index 1  toggle
static std::atomic<bool>  g_noads{false};           // index 2  toggle
static std::atomic<float> g_damage{1.0f};           // index 3  seekbar 1-20
static std::atomic<bool>  g_config_loaded_once{false};
static std::atomic<float> g_gameScale{1.0f};        // timescale gốc của game

// ═══════════════════════════════════════════════════════════
//  ELF IMAGE INFO
// ═══════════════════════════════════════════════════════════
struct LoadedIl2CppImage {
    uintptr_t         base;
    const ElfW(Phdr)* phdr;
    size_t            phnum;
    const char*       path;
};
static LoadedIl2CppImage g_il2cpp_image = {};
static void* g_il2cpp_handle = nullptr;

// ═══════════════════════════════════════════════════════════
//  ImageContains — validate mọi truy cập bộ nhớ
// ═══════════════════════════════════════════════════════════
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

// ═══════════════════════════════════════════════════════════
//  Il2CppExports — ELF symbol resolver (GNU_HASH + SYSV_HASH)
// ═══════════════════════════════════════════════════════════
class Il2CppExports {
    uintptr_t symbols_ = 0, strings_ = 0, sysv_ = 0, gnu_ = 0;
    size_t string_size_ = 0;

    void* SymbolAt(uint32_t index, const char* name) const {
        uintptr_t address = ImageArrayAddress(symbols_, index, sizeof(ElfW(Sym)));
        if (!ImageContains(address, sizeof(ElfW(Sym)))) return nullptr;
        ElfW(Sym) symbol;
        memcpy(&symbol, reinterpret_cast<const void*>(address), sizeof(symbol));
        if (symbol.st_shndx == SHN_UNDEF || symbol.st_name >= string_size_) return nullptr;
        unsigned type    = symbol.st_info & 0xf;
        unsigned binding = symbol.st_info >> 4;
        if ((type != STT_FUNC && type != STT_NOTYPE) ||
            (binding != STB_GLOBAL && binding != STB_WEAK)) return nullptr;
        size_t length = strlen(name) + 1;
        if (length > string_size_ - symbol.st_name) return nullptr;
        if (memcmp(reinterpret_cast<const void*>(strings_ + symbol.st_name), name, length))
            return nullptr;
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
        if (!count || count > 65536 ||
            !ImageContains(dynamic_address, count * sizeof(ElfW(Dyn)))) return false;
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
            ImageReadU32(gnu_,     &bucket_count);
            ImageReadU32(gnu_ + 4, &first_symbol);
            ImageReadU32(gnu_ + 8, &bloom_count);
            uintptr_t buckets = ImageArrayAddress(gnu_ + 16, bloom_count, sizeof(ElfW(Addr)));
            uintptr_t chains  = ImageArrayAddress(buckets, bucket_count, sizeof(uint32_t));
            if (bucket_count && bloom_count && buckets && chains) {
                uint32_t hash = 5381, index = 0;
                for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p)
                    hash = hash * 33 + *p;
                if (ImageReadU32(ImageArrayAddress(buckets, hash % bucket_count, 4), &index) &&
                    index >= first_symbol) {
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
            ImageReadU32(sysv_,     &bucket_count);
            ImageReadU32(sysv_ + 4, &symbol_count);
            uintptr_t buckets = sysv_ + 8;
            uintptr_t chains  = ImageArrayAddress(buckets, bucket_count, sizeof(uint32_t));
            if (bucket_count && symbol_count && chains) {
                uint32_t hash = 0, index = 0;
                for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p) {
                    hash = (hash << 4) + *p;
                    uint32_t high = hash & 0xf0000000u;
                    if (high) hash ^= high >> 24;
                    hash &= ~high;
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

// ═══════════════════════════════════════════════════════════
//  isIl2cppLoaded + GetIl2cppHandle
// ═══════════════════════════════════════════════════════════
static bool isIl2cppLoaded() {
    bool found = false;
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* data) -> int {
        const char* path = info->dlpi_name;
        const char* leaf = path ? strrchr(path, '/') : nullptr;
        leaf = leaf ? leaf + 1 : path;
        if (!leaf || strcmp(leaf, "libil2cpp.so") != 0 ||
            !info->dlpi_phdr || !info->dlpi_phnum) return 0;
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

// ═══════════════════════════════════════════════════════════
//  IL2CPP API
// ═══════════════════════════════════════════════════════════
static void*       (*il2cpp_domain_get)()                                        = nullptr;
static void*       (*il2cpp_thread_attach)(void*)                                = nullptr;
static void        (*il2cpp_thread_detach)(void*)                                = nullptr;
static void**      (*il2cpp_domain_get_assemblies)(void*, size_t*)               = nullptr;
static void*       (*il2cpp_assembly_get_image)(void*)                           = nullptr;
static void*       (*il2cpp_class_from_name)(void*, const char*, const char*)    = nullptr;
static void*       (*il2cpp_class_get_methods)(void*, void**)                    = nullptr;
static const char* (*il2cpp_method_get_name)(void*)                              = nullptr;
static uint32_t    (*il2cpp_method_get_param_count)(void*)                       = nullptr;
static void*       (*il2cpp_runtime_invoke)(void*, void*, void**, void**)        = nullptr;
static void*       (*il2cpp_object_get_class)(void*)                             = nullptr;
static void*       (*il2cpp_class_get_method_from_name)(void*, const char*, int) = nullptr;
static size_t      (*il2cpp_image_get_class_count)(void*)                        = nullptr;
static void*       (*il2cpp_image_get_class)(void*, size_t)                      = nullptr;
static const char* (*il2cpp_class_get_name_raw)(void*)                           = nullptr;

static bool InitIl2cpp() {
    if (!isIl2cppLoaded()) return false;
    LOGI("[%s] Resolve BEGIN: base=%p", BUILD_ID,
         reinterpret_cast<void*>(g_il2cpp_image.base));
    void* handle = GetIl2cppHandle();
    Il2CppExports exports;
    const bool has_exports = exports.Init();
    unsigned from_handle = 0, from_image = 0, missing = 0;

    auto resolve = [&](const char* name, bool required) -> void* {
        void* fn = handle ? dlsym(handle, name) : nullptr;
        if (fn && ImageContains(reinterpret_cast<uintptr_t>(fn), 1, PF_X)) {
            ++from_handle; return fn;
        }
        fn = has_exports ? exports.Find(name) : nullptr;
        if (fn) { ++from_image; return fn; }
        if (required) { ++missing; LOGE("Required API missing: %s", name); }
        return nullptr;
    };

#define LOAD_API(sym, req) sym = reinterpret_cast<decltype(sym)>(resolve(#sym, req))
    LOAD_API(il2cpp_domain_get,                true);
    LOAD_API(il2cpp_thread_attach,             true);
    LOAD_API(il2cpp_domain_get_assemblies,     true);
    LOAD_API(il2cpp_assembly_get_image,        true);
    LOAD_API(il2cpp_class_from_name,           true);
    LOAD_API(il2cpp_class_get_method_from_name,true);
    LOAD_API(il2cpp_object_get_class,          true);
    LOAD_API(il2cpp_runtime_invoke,            true);
    LOAD_API(il2cpp_thread_detach,             false);
    LOAD_API(il2cpp_class_get_methods,         false);
    LOAD_API(il2cpp_method_get_name,           false);
    LOAD_API(il2cpp_method_get_param_count,    false);
    LOAD_API(il2cpp_image_get_class_count,     false);
    LOAD_API(il2cpp_image_get_class,           false);
    il2cpp_class_get_name_raw = reinterpret_cast<decltype(il2cpp_class_get_name_raw)>(
        resolve("il2cpp_class_get_name", false));
#undef LOAD_API
    LOGI("[%s] Resolve END: handle=%u mappedELF=%u missing=%u",
         BUILD_ID, from_handle, from_image, missing);
    return missing == 0;
}

// ═══════════════════════════════════════════════════════════
//  HELPERS
// ═══════════════════════════════════════════════════════════
static uintptr_t Tim_Ham(const char* ns, const char* cls, const char* method, int nParams) {
    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies || !il2cpp_assembly_get_image ||
        !il2cpp_class_from_name || !il2cpp_class_get_method_from_name) return 0;
    void* domain = il2cpp_domain_get();
    if (!domain) return 0;
    size_t n = 0;
    void** asm_ = il2cpp_domain_get_assemblies(domain, &n);
    if (!asm_) return 0;
    for (size_t i = 0; i < n; ++i) {
        void* img = il2cpp_assembly_get_image(asm_[i]);
        if (!img) continue;
        void* klass = il2cpp_class_from_name(img, ns, cls);
        if (!klass) continue;
        void* mi = il2cpp_class_get_method_from_name(klass, method, nParams);
        if (mi) return reinterpret_cast<uintptr_t>(*(void**)mi);
    }
    LOGE("Tim_Ham NOT FOUND: %s.%s(%d)", cls, method, nParams);
    return 0;
}
static void* FindClassAnywhere(const char* name) {
    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies || !il2cpp_assembly_get_image) return nullptr;
    void* domain = il2cpp_domain_get();
    size_t n = 0;
    void** asm_ = il2cpp_domain_get_assemblies(domain, &n);
    for (size_t i = 0; i < n; ++i) {
        void* img = il2cpp_assembly_get_image(asm_[i]);
        if (!img) continue;
        if (il2cpp_image_get_class && il2cpp_image_get_class_count && il2cpp_class_get_name_raw) {
            size_t nc = il2cpp_image_get_class_count(img);
            for (size_t j = 0; j < nc; ++j) {
                void* k = il2cpp_image_get_class(img, j);
                if (!k) continue;
                const char* kn = il2cpp_class_get_name_raw(k);
                if (kn && strcmp(kn, name) == 0) return k;
            }
        } else {
            void* k = il2cpp_class_from_name(img, "", name);
            if (k) return k;
        }
    }
    LOGE("FindClassAnywhere NOT FOUND: %s", name);
    return nullptr;
}
static bool DoHook(void* fn, void* hook, void** orig, const char* tag) {
    if (!hook || !orig || !ImageContains(reinterpret_cast<uintptr_t>(fn), 1, PF_X)) {
        LOGE("Hook SKIP (invalid addr): %s @ %p", tag, fn);
        return false;
    }
    const int r = DobbyHook(fn, hook, orig);
    const bool ok = r == 0 && *orig != nullptr;
    LOGI("Hook %s: %s @ %p", ok ? "OK" : "FAIL", tag, fn);
    return ok;
}

// Invoke Action<bool> callback (ShowRewardedVideo)
static bool InvokeActionBool(void* callback, bool value) {
    if (!callback || !il2cpp_object_get_class || !il2cpp_class_get_method_from_name || !il2cpp_runtime_invoke) return false;
    void* klass = il2cpp_object_get_class(callback);
    if (!klass) return false;
    void* mi = il2cpp_class_get_method_from_name(klass, OBFUSCATE("Invoke"), 1);
    if (!mi) { LOGE("InvokeActionBool: Invoke(1) not found"); return false; }
    void* exc = nullptr;
    int32_t bval = value ? 1 : 0;
    void* args[] = { &bval };
    il2cpp_runtime_invoke(mi, callback, args, &exc);
    if (exc) { LOGE("InvokeActionBool: exception"); return false; }
    return true;
}

// ═══════════════════════════════════════════════════════════
//  HOOKS
// ═══════════════════════════════════════════════════════════

// ── Speed ──
static float (*o_get_timeScale)(void*)        = nullptr;
static void  (*o_set_timeScale)(float, void*) = nullptr;

static float h_get_timeScale(void* mi) {
    float s = g_speed.load(std::memory_order_acquire);
    if (s != 1.0f) return g_gameScale.load(std::memory_order_acquire);
    return o_get_timeScale ? o_get_timeScale(mi) : 1.0f;
}
static void h_set_timeScale(float v, void* mi) {
    g_gameScale.store(v, std::memory_order_release);
    float s = g_speed.load(std::memory_order_acquire);
    if (o_set_timeScale) o_set_timeScale(v * s, mi);
}

// ── Free Shop — crystal path ──
static bool (*o_HasEnough)(void*, int32_t, int32_t, void*) = nullptr;
static bool h_HasEnough(void* self, int32_t itemId, int32_t amt, void* mi) {
    if (g_shop.load(std::memory_order_acquire)) return true;
    return o_HasEnough ? o_HasEnough(self, itemId, amt, mi) : false;
}

static void (*o_Remove)(void*, int32_t, int32_t, void*, void*) = nullptr;
static void h_Remove(void* self, int32_t itemId, int32_t amt, void* src, void* mi) {
    if (g_shop.load(std::memory_order_acquire)) return;
    if (o_Remove) o_Remove(self, itemId, amt, src, mi);
}

// ── Free Shop — IAP path ──
static void (*o_SL_BuyProduct)(void*, void*, void*, void*)  = nullptr;
static void (*g_SL_BuyProductDebug)(void*, void*, void*, void*) = nullptr;

static void h_SL_BuyProduct(void* self, void* id, void* callback, void* mi) {
    if (g_shop.load(std::memory_order_acquire) && g_SL_BuyProductDebug) {
        LOGI("[WB] FreeShop: IAP BuyProduct -> BuyProductDebug");
        g_SL_BuyProductDebug(self, id, callback, nullptr);
        return;
    }
    if (o_SL_BuyProduct) o_SL_BuyProduct(self, id, callback, mi);
}

// ── Remove Ads ──
static bool (*o_DontShowAds)(void*, void*) = nullptr;   // STATIC method
static bool h_DontShowAds(void* placement, void* mi) {
    if (g_noads.load(std::memory_order_acquire)) return true;
    return o_DontShowAds ? o_DontShowAds(placement, mi) : false;
}

static bool (*o_IsRewardedAvail)(void*, int32_t, void*) = nullptr;
static bool h_IsRewardedAvail(void* self, int32_t placement, void* mi) {
    if (g_noads.load(std::memory_order_acquire)) return true;
    return o_IsRewardedAvail ? o_IsRewardedAvail(self, placement, mi) : false;
}

static void (*o_ShowRewardedVideo)(void*, int32_t, void*, void*) = nullptr;
static void h_ShowRewardedVideo(void* self, int32_t placement, void* callback, void* mi) {
    if (g_noads.load(std::memory_order_acquire)) {
        LOGI("[WB] NoAds: ShowRewardedVideo -> auto callback(true)");
        if (!InvokeActionBool(callback, true)) LOGE("[WB] InvokeActionBool FAIL");
        return;
    }
    if (o_ShowRewardedVideo) o_ShowRewardedVideo(self, placement, callback, mi);
}

// ── Damage ──
static float (*o_GetCombatDamage)(void*, void*)         = nullptr;
static float (*o_GetDamageGTM)(void*, int32_t, void*)   = nullptr;
static float (*o_GetDamageGun)(void*, int32_t, void*)   = nullptr;

static float h_GetCombatDamage(void* self, void* mi) {
    float base = o_GetCombatDamage ? o_GetCombatDamage(self, mi) : 1.0f;
    return base * g_damage.load(std::memory_order_acquire);
}
static float h_GetDamageGTM(void* self, int32_t type, void* mi) {
    float base = o_GetDamageGTM ? o_GetDamageGTM(self, type, mi) : 1.0f;
    return base * g_damage.load(std::memory_order_acquire);
}
static float h_GetDamageGun(void* self, int32_t type, void* mi) {
    float base = o_GetDamageGun ? o_GetDamageGun(self, type, mi) : 1.0f;
    return base * g_damage.load(std::memory_order_acquire);
}

// ═══════════════════════════════════════════════════════════
//  WORKER THREAD — chờ il2cpp → cài hook
// ═══════════════════════════════════════════════════════════
static void* h_t(void*) {
    for (int i = 0; i < 120; ++i) {
        if (isIl2cppLoaded()) break;
        sleep(1);
    }
    if (!g_il2cpp_image.base) { LOGE("[%s] libil2cpp.so not found", BUILD_ID); return nullptr; }
    if (!InitIl2cpp())        { LOGE("[%s] InitIl2cpp FAIL", BUILD_ID);         return nullptr; }

    // Chờ assemblies ổn định
    size_t last = 0; int stable = 0;
    void* domain = il2cpp_domain_get();
    void* thread = (domain && il2cpp_thread_attach) ? il2cpp_thread_attach(domain) : nullptr;
    for (int i = 0; i < 60 && stable < 5; ++i) {
        sleep(1);
        size_t count = 0;
        void** asm_ = il2cpp_domain_get_assemblies ? il2cpp_domain_get_assemblies(domain, &count) : nullptr;
        if (asm_ && count == last && count > 0) ++stable; else { last = count; stable = 0; }
    }
    if (stable < 5) { LOGE("[%s] assemblies not ready", BUILD_ID); goto done; }
    LOGI("[%s] Assemblies stable: %zu", BUILD_ID, last);

    {
        unsigned installed = 0;
        uintptr_t addr;

        // Speed
        addr = Tim_Ham(OBFUSCATE("UnityEngine"), OBFUSCATE("Time"), OBFUSCATE("get_timeScale"), 0);
        installed += DoHook((void*)addr, (void*)h_get_timeScale, (void**)&o_get_timeScale, "Time.get_timeScale");
        if (o_get_timeScale) g_gameScale.store(o_get_timeScale(nullptr), std::memory_order_release);

        addr = Tim_Ham(OBFUSCATE("UnityEngine"), OBFUSCATE("Time"), OBFUSCATE("set_timeScale"), 1);
        installed += DoHook((void*)addr, (void*)h_set_timeScale, (void**)&o_set_timeScale, "Time.set_timeScale");

        // Free Shop — crystal
        addr = Tim_Ham(OBFUSCATE("WesternBreaker"), OBFUSCATE("InventoryModel"), OBFUSCATE("HasEnough"), 2);
        installed += DoHook((void*)addr, (void*)h_HasEnough, (void**)&o_HasEnough, "InventoryModel.HasEnough");

        addr = Tim_Ham(OBFUSCATE("WesternBreaker"), OBFUSCATE("InventoryModel"), OBFUSCATE("Remove"), 3);
        installed += DoHook((void*)addr, (void*)h_Remove, (void**)&o_Remove, "InventoryModel.Remove");

        // Free Shop — IAP
        {
            void* slKlass = FindClassAnywhere(OBFUSCATE("StoreListener"));
            if (slKlass && il2cpp_class_get_method_from_name) {
                void* mi_bp = il2cpp_class_get_method_from_name(slKlass, OBFUSCATE("BuyProduct"), 2);
                if (mi_bp) {
                    addr = reinterpret_cast<uintptr_t>(*(void**)mi_bp);
                    installed += DoHook((void*)addr, (void*)h_SL_BuyProduct, (void**)&o_SL_BuyProduct, "StoreListener.BuyProduct");
                }
                void* mi_dbg = il2cpp_class_get_method_from_name(slKlass, OBFUSCATE("BuyProductDebug"), 2);
                if (mi_dbg) g_SL_BuyProductDebug = (decltype(g_SL_BuyProductDebug))*(void**)mi_dbg;
                LOGI("BuyProductDebug: %s @ %p", g_SL_BuyProductDebug ? "OK" : "NULL", (void*)g_SL_BuyProductDebug);
            }
        }

        // Remove Ads
        addr = Tim_Ham(OBFUSCATE("GamestotySpace"), OBFUSCATE("GamestotyAds"), OBFUSCATE("DontShowAdsByPlacement"), 1);
        installed += DoHook((void*)addr, (void*)h_DontShowAds, (void**)&o_DontShowAds, "GamestotyAds.DontShowAdsByPlacement");

        addr = Tim_Ham(OBFUSCATE("WesternBreaker"), OBFUSCATE("GamestotyModel"), OBFUSCATE("IsRewardedVideoAvailable"), 1);
        installed += DoHook((void*)addr, (void*)h_IsRewardedAvail, (void**)&o_IsRewardedAvail, "GamestotyModel.IsRewardedVideoAvailable");

        addr = Tim_Ham(OBFUSCATE("WesternBreaker"), OBFUSCATE("GamestotyModel"), OBFUSCATE("ShowRewardedVideo"), 2);
        installed += DoHook((void*)addr, (void*)h_ShowRewardedVideo, (void**)&o_ShowRewardedVideo, "GamestotyModel.ShowRewardedVideo");

        // Damage
        addr = Tim_Ham(OBFUSCATE("WesternBreaker"), OBFUSCATE("GatheringToolItemSettings"), OBFUSCATE("GetCombatDamage"), 0);
        installed += DoHook((void*)addr, (void*)h_GetCombatDamage, (void**)&o_GetCombatDamage, "GatheringToolItemSettings.GetCombatDamage");

        addr = Tim_Ham(OBFUSCATE("WesternBreaker"), OBFUSCATE("GatheringToolItemSettings"), OBFUSCATE("GetDamage"), 1);
        installed += DoHook((void*)addr, (void*)h_GetDamageGTM, (void**)&o_GetDamageGTM, "GatheringToolItemSettings.GetDamage");

        addr = Tim_Ham(OBFUSCATE("WesternBreaker"), OBFUSCATE("Gun"), OBFUSCATE("GetDamage"), 1);
        installed += DoHook((void*)addr, (void*)h_GetDamageGun, (void**)&o_GetDamageGun, "Gun.GetDamage");

        LOGI("[%s] HOOK DONE: %u/11 installed", BUILD_ID, installed);
    }

done:
    if (il2cpp_thread_detach && thread) il2cpp_thread_detach(thread);
    return nullptr;
}

// ═══════════════════════════════════════════════════════════
//  CONFIG READER
//  HAMOD ghi 7 fields: f0=speed f1=shop f2=noads f3=damage f4-f6=unused
// ═══════════════════════════════════════════════════════════
static bool ParseMenuConfig(const char* text,
                             float* speed, bool* shop, bool* noads, float* damage) {
    if (!text || !speed || !shop || !noads || !damage) return false;
    float v[7] = {}; unsigned n = 0;
    const char* p = text;
    while (true) {
        while (isspace((unsigned char)*p)) ++p;
        if (!*p) break;
        if (n >= 7) return false;
        char* end = nullptr; errno = 0;
        float f = strtof(p, &end);
        if (end == p || errno == ERANGE || !std::isfinite(f) ||
            (*end && !isspace((unsigned char)*end))) return false;
        v[n++] = f; p = end;
    }
    if (n < 4) return false;
    if (v[1] != 0.0f && v[1] != 1.0f) return false;
    if (v[2] != 0.0f && v[2] != 1.0f) return false;
    *speed  = v[0] < 1.0f ? 1.0f : v[0] > 10.0f  ? 10.0f  : v[0];
    *shop   = v[1] == 1.0f;
    *noads  = v[2] == 1.0f;
    *damage = v[3] < 1.0f ? 1.0f : v[3] > 20.0f ? 20.0f : v[3];
    return true;
}

static void* Doc_Config_Ngam(void*) {
    char root[768];
    const char* env_root = getenv("MODMENU_LOG_DIR");
    const char* src = env_root && *env_root ? env_root :
        "/data/data/menu.nextgen.launcher/files";
    int rlen = snprintf(root, sizeof(root), "%s", src);
    if (rlen < 0 || (size_t)rlen >= sizeof(root)) { LOGE("CONFIG: root too long"); return nullptr; }
    size_t rsz = strlen(root);
    while (rsz > 1 && root[rsz - 1] == '/') root[--rsz] = '\0';
    const char* leaf = strrchr(root, '/'); leaf = leaf ? leaf + 1 : root;
    const bool root_is_mods = strcmp(leaf, "mods") == 0;

    // 4 candidates: mods/ + no-mods/ × lowercase + uppercase package
    const char* names[] = {
        "config_com.winter.breaker.txt",
        "config_COM.WINTER.BREAKER.TXT"
    };
    char paths[4][1024] = {}; size_t path_count = 0;
    for (unsigned location = 0; location < 2; ++location) {
        const bool append_mods = root_is_mods ? location == 1 : location == 0;
        for (const char* name : names) {
            int n = snprintf(paths[path_count], sizeof(paths[path_count]),
                             "%s/%s%s", root, append_mods ? "mods/" : "", name);
            if (n > 0 && (size_t)n < sizeof(paths[path_count])) ++path_count;
        }
    }
    LOGI("[%s] CONFIG begin: pid=%d", BUILD_ID, getpid());
    for (size_t i = 0; i < path_count; ++i) LOGI("CONFIG candidate[%zu]=%s", i, paths[i]);

    int last_status = -1, last_path = -1;
    bool  last_shop = false, last_noads = false;
    float last_speed = -1.0f, last_damage = -1.0f;
    static float s_applied_speed = -1.0f;

    while (true) {
        FILE* file = nullptr; int selected = -1;
        for (size_t i = 0; i < path_count; ++i) {
            file = fopen(paths[i], "r");
            if (file) { selected = (int)i; break; }
        }
        if (!file) {
            if (last_status != 0) LOGE("CONFIG missing; previous values retained");
            last_status = 0; usleep(500000); continue;
        }
        char text[256] = {};
        size_t length = fread(text, 1, sizeof(text) - 1, file);
        bool read_ok = !ferror(file) && length < sizeof(text) - 1;
        fclose(file);

        float speed = 1.0f, damage = 1.0f;
        bool  shop = false, noads = false;
        if (!read_ok || memchr(text, '\0', length) ||
            !ParseMenuConfig(text, &speed, &shop, &noads, &damage)) {
            if (last_status != 1 || last_path != selected)
                LOGE("CONFIG invalid: %s", paths[selected]);
            last_status = 1; last_path = selected; usleep(500000); continue;
        }

        g_speed.store(speed,   std::memory_order_release);
        g_shop.store(shop,     std::memory_order_release);
        g_noads.store(noads,   std::memory_order_release);
        g_damage.store(damage, std::memory_order_release);
        g_config_loaded_once.store(true, std::memory_order_release);

        // Active apply speed (game chỉ gọi set_timeScale 1 lần lúc startup)
        if (o_set_timeScale && speed != s_applied_speed) {
            float gs = g_gameScale.load(std::memory_order_acquire);
            o_set_timeScale(gs * speed, nullptr);
            s_applied_speed = speed;
            LOGI("[WB] Speed apply: gameScale=%.2f * speed=%.0f = %.2f", gs, speed, gs * speed);
        }

        if (last_status != 2 || last_path != selected || last_speed != speed ||
            last_shop != shop || last_noads != noads || last_damage != damage)
            LOGI("CONFIG ok: path=%s speed=%.0f shop=%d noads=%d damage=%.0f",
                 paths[selected], speed, shop, noads, damage);
        last_status = 2; last_path = selected;
        last_speed = speed; last_shop = shop; last_noads = noads; last_damage = damage;
        usleep(500000);
    }
    return nullptr;
}

// ═══════════════════════════════════════════════════════════
//  ENTRY
// ═══════════════════════════════════════════════════════════
__attribute__((constructor))
void l_m() {
    LOGI("[%s] MOD injected!", BUILD_ID);
    pthread_t t1, t2;
    int rc = pthread_create(&t1, nullptr, h_t, nullptr);
    if (rc == 0) pthread_detach(t1); else LOGE("Worker create failed: %d", rc);
    rc = pthread_create(&t2, nullptr, Doc_Config_Ngam, nullptr);
    if (rc == 0) pthread_detach(t2); else LOGE("Config thread create failed: %d", rc);
}
