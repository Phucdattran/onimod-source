// com.and1games.xzyx HAMOD mod
// Package: com.and1games.xzyx
// Features: Free Shop (toggle idx 0) · Speed x1–10 (seekbar idx 1)
// Based on HAMOD IL2CPP autoupdate skill (ELF symbol resolver pattern)

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
#include <Dobby/dobby.h>

#define BUILD_ID "XZYX_MOD"
#define LOG_TAG  "XZYX_MOD"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─────────────────────────────────────────────
// §2  Global state (index 0 = Free Shop toggle, index 1 = Speed seekbar)
// ─────────────────────────────────────────────
static std::atomic<bool>  g_feature0{false};  // Free Shop
static std::atomic<float> g_feature1{1.0f};   // Speed multiplier
static std::atomic<bool>  g_config_loaded_once{false};

// ─────────────────────────────────────────────
// §3  IL2CPP API pointers
// ─────────────────────────────────────────────
static void*       (*il2cpp_domain_get)()                                            = nullptr;
static void*       (*il2cpp_thread_attach)(void*)                                    = nullptr;
static void        (*il2cpp_thread_detach)(void*)                                    = nullptr;
static void**      (*il2cpp_domain_get_assemblies)(void*, size_t*)                   = nullptr;
static void*       (*il2cpp_assembly_get_image)(void*)                               = nullptr;
static void*       (*il2cpp_class_from_name)(void*, const char*, const char*)        = nullptr;
static void*       (*il2cpp_class_get_method_from_name)(void*, const char*, int)     = nullptr;
static void*       (*il2cpp_class_get_field_from_name)(void*, const char*)           = nullptr;
static int32_t     (*il2cpp_field_get_offset)(void*)                                 = nullptr;
static size_t      (*il2cpp_image_get_class_count)(void*)                            = nullptr;
static void*       (*il2cpp_image_get_class)(void*, size_t)                          = nullptr;
static const char* (*il2cpp_class_get_name_raw)(void*)                               = nullptr;
static void*       (*il2cpp_object_get_class)(void*)                                 = nullptr;

// ─────────────────────────────────────────────
// §4  ELF image info
// ─────────────────────────────────────────────
struct LoadedIl2CppImage {
    uintptr_t         base;
    const ElfW(Phdr)* phdr;
    size_t            phnum;
    const char*       path;
};
static LoadedIl2CppImage g_il2cpp_image = {};
static void*             g_il2cpp_handle = nullptr;

// ─────────────────────────────────────────────
// §5  ImageContains / ImageReadU32 / ImageDynamicAddress
// ─────────────────────────────────────────────
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

// ─────────────────────────────────────────────
// §6  Il2CppExports — ELF symbol resolver (no DobbySymbolResolver)
// ─────────────────────────────────────────────
class Il2CppExports {
    uintptr_t symbols_ = 0, strings_ = 0, sysv_ = 0, gnu_ = 0;
    size_t    string_size_ = 0;

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

// ─────────────────────────────────────────────
// §7  isIl2cppLoaded + GetIl2cppHandle
// ─────────────────────────────────────────────
static bool isIl2cppLoaded() {
    bool found = false;
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* data) -> int {
        const char* path = info->dlpi_name;
        if (!path) return 0;
        const char* leaf = strrchr(path, '/');
        leaf = leaf ? leaf + 1 : path;
        if (strcmp(leaf, "libil2cpp.so") != 0 || !info->dlpi_phdr || !info->dlpi_phnum) return 0;
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
        g_il2cpp_handle = dlopen("libil2cpp.so", flags);
    if (!g_il2cpp_handle) LOGI("[%s] handle unavailable; using mapped ELF", BUILD_ID);
    return g_il2cpp_handle;
}

// ─────────────────────────────────────────────
// §8  InitIl2cpp
// ─────────────────────────────────────────────
static bool InitIl2cpp() {
    if (!isIl2cppLoaded()) return false;
    LOGI("[%s] InitIl2cpp: base=%p", BUILD_ID, reinterpret_cast<void*>(g_il2cpp_image.base));
    void*    handle = GetIl2cppHandle();
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
        if (required) { ++missing; LOGE("[%s] Required API missing: %s", BUILD_ID, name); }
        return nullptr;
    };
#define LOAD_API(fn, req) fn = reinterpret_cast<decltype(fn)>(resolve(#fn, req))
    LOAD_API(il2cpp_domain_get,              true);
    LOAD_API(il2cpp_thread_attach,           true);
    LOAD_API(il2cpp_domain_get_assemblies,   true);
    LOAD_API(il2cpp_assembly_get_image,      true);
    LOAD_API(il2cpp_class_from_name,         true);
    LOAD_API(il2cpp_class_get_method_from_name, true);
    LOAD_API(il2cpp_object_get_class,        true);
    LOAD_API(il2cpp_thread_detach,           false);
    LOAD_API(il2cpp_class_get_field_from_name, false);
    LOAD_API(il2cpp_field_get_offset,        false);
    LOAD_API(il2cpp_image_get_class_count,   false);
    LOAD_API(il2cpp_image_get_class,         false);
    il2cpp_class_get_name_raw = reinterpret_cast<decltype(il2cpp_class_get_name_raw)>(
        resolve("il2cpp_class_get_name", false));
#undef LOAD_API
    LOGI("[%s] InitIl2cpp END: handle=%u mapped=%u missing=%u",
         BUILD_ID, from_handle, from_image, missing);
    return missing == 0;
}

// ─────────────────────────────────────────────
// §9  Tim_Ham + FindKlass
// ─────────────────────────────────────────────
static uintptr_t Tim_Ham(const char* ns, const char* className, const char* methodName, int args) {
    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies ||
        !il2cpp_assembly_get_image || !il2cpp_class_from_name ||
        !il2cpp_class_get_method_from_name) return 0;
    void* domain = il2cpp_domain_get();
    if (!domain) return 0;
    size_t sz = 0;
    void** asm_ = il2cpp_domain_get_assemblies(domain, &sz);
    if (!asm_) return 0;
    for (size_t i = 0; i < sz; ++i) {
        void* img = il2cpp_assembly_get_image(asm_[i]);
        if (!img) continue;
        void* klass = il2cpp_class_from_name(img, ns, className);
        if (klass) {
            void* method = il2cpp_class_get_method_from_name(klass, methodName, args);
            if (method) return (uintptr_t)*(void**)method;
        }
    }
    LOGE("[%s] Tim_Ham NOT FOUND: %s::%s.%s(%d)", BUILD_ID, ns, className, methodName, args);
    return 0;
}

static void* FindKlass(const char* ns, const char* className) {
    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies ||
        !il2cpp_assembly_get_image || !il2cpp_class_from_name) return nullptr;
    void* domain = il2cpp_domain_get();
    if (!domain) return nullptr;
    size_t sz = 0;
    void** asm_ = il2cpp_domain_get_assemblies(domain, &sz);
    if (!asm_) return nullptr;
    for (size_t i = 0; i < sz; ++i) {
        void* img = il2cpp_assembly_get_image(asm_[i]);
        if (!img) continue;
        void* klass = il2cpp_class_from_name(img, ns, className);
        if (klass) return klass;
    }
    LOGE("[%s] FindKlass NOT FOUND: %s::%s", BUILD_ID, ns, className);
    return nullptr;
}

// ─────────────────────────────────────────────
// §10  DoHook
// ─────────────────────────────────────────────
static bool DoHook(void* fn, void* hook, void** orig, const char* tag) {
    if (!fn || !hook || !orig ||
        !ImageContains(reinterpret_cast<uintptr_t>(fn), 1, PF_X)) {
        LOGE("[%s] Hook SKIP (invalid addr): %s @ %p", BUILD_ID, tag, fn);
        return false;
    }
    const int result = DobbyHook(fn, hook, orig);
    const bool ok = result == 0 && *orig != nullptr;
    LOGI("[%s] Hook %s: %s @ %p", BUILD_ID, ok ? "OK" : "FAIL", tag, fn);
    return ok;
}

// ─────────────────────────────────────────────
// §11  Speed hooks
//   UnityEngine.Time.get_timeScale (static, 0 params) → float
//   UnityEngine.Time.set_timeScale (static, 1 param: float) → void
// ─────────────────────────────────────────────
static float  (*o_getTS)(void*)         = nullptr;
static void   (*o_setTS)(float, void*)  = nullptr;
static float  s_last_game_ts            = 1.0f;

static float h_getTS(void* mi) {
    return o_getTS ? o_getTS(mi) : 1.0f;
}

static void h_setTS(float value, void* mi) {
    s_last_game_ts = value;
    float spd = g_feature1.load(std::memory_order_acquire);
    if (o_setTS) o_setTS(value * spd, mi);
}

// ─────────────────────────────────────────────
// §12  Free Shop hooks
//   IAPClientManager (namespace "")
//   TryPurchase(instance, 2 params: string productId, Action<ProductPurchaseRsp> onResult) → void
//     ProductPurchaseRsp: 0=Success
//   GrantOnceRewards(instance, 1 param: string productId) → void  [cached, not hooked]
//   HasActiveEntitlement(instance, 1 param: string productId) → bool
//   GetEntitlementRemainingDays(instance, 1 param: string productId) → int
//   TryFreeRewardAd(instance, 0 params) → bool
//   GetAdFreeRemainingToday(instance, 1 param: string productId) → int
// ─────────────────────────────────────────────

// Cached address for GrantOnceRewards (private, called directly — not hooked)
static uintptr_t s_addr_GrantOnceRewards = 0;

typedef void (*t_TryPurchase)(void* self, void* productId, void* onResult, void* mi);
typedef bool (*t_HasActiveEntitlement)(void* self, void* productId, void* mi);
typedef int  (*t_GetEntitlementRemainingDays)(void* self, void* productId, void* mi);
typedef bool (*t_TryFreeRewardAd)(void* self, void* mi);
typedef int  (*t_GetAdFreeRemainingToday)(void* self, void* productId, void* mi);

static t_TryPurchase                o_TryPurchase                = nullptr;
static t_HasActiveEntitlement       o_HasActiveEntitlement       = nullptr;
static t_GetEntitlementRemainingDays o_GetEntitlementRemainingDays = nullptr;
static t_TryFreeRewardAd            o_TryFreeRewardAd            = nullptr;
static t_GetAdFreeRemainingToday    o_GetAdFreeRemainingToday    = nullptr;

static void h_TryPurchase(void* self, void* productId, void* onResult, void* mi) {
    if (g_feature0.load(std::memory_order_acquire)) {
        // Grant consumable/once rewards
        if (s_addr_GrantOnceRewards) {
            typedef void (*t_Grant)(void* self, void* productId, void* mi);
            ((t_Grant)s_addr_GrantOnceRewards)(self, productId, nullptr);
            LOGI("[%s] FreeShop: GrantOnceRewards called", BUILD_ID);
        }
        // Invoke onResult(0 = Success)
        if (onResult && il2cpp_object_get_class && il2cpp_class_get_method_from_name) {
            void* cbKlass = il2cpp_object_get_class(onResult);
            if (cbKlass) {
                void* invokeMI = il2cpp_class_get_method_from_name(cbKlass, "Invoke", 1);
                if (invokeMI) {
                    typedef void (*t_Invoke)(void* self, int32_t rsp, void* mi);
                    void* invokeAddr = *(void**)invokeMI;
                    ((t_Invoke)invokeAddr)(onResult, 0 /*Success*/, nullptr);
                    LOGI("[%s] FreeShop: TryPurchase -> Success", BUILD_ID);
                } else {
                    LOGE("[%s] FreeShop: Invoke(1) not found on callback", BUILD_ID);
                }
            } else {
                LOGE("[%s] FreeShop: object_get_class returned null", BUILD_ID);
            }
        }
        return;
    }
    if (o_TryPurchase) o_TryPurchase(self, productId, onResult, mi);
}

static bool h_HasActiveEntitlement(void* self, void* productId, void* mi) {
    if (g_feature0.load(std::memory_order_acquire)) return true;
    return o_HasActiveEntitlement ? o_HasActiveEntitlement(self, productId, mi) : false;
}

static int h_GetEntitlementRemainingDays(void* self, void* productId, void* mi) {
    if (g_feature0.load(std::memory_order_acquire)) return 30;
    return o_GetEntitlementRemainingDays ? o_GetEntitlementRemainingDays(self, productId, mi) : 0;
}

static bool h_TryFreeRewardAd(void* self, void* mi) {
    if (g_feature0.load(std::memory_order_acquire)) return true;
    return o_TryFreeRewardAd ? o_TryFreeRewardAd(self, mi) : false;
}

static int h_GetAdFreeRemainingToday(void* self, void* productId, void* mi) {
    if (g_feature0.load(std::memory_order_acquire)) return 20;
    return o_GetAdFreeRemainingToday ? o_GetAdFreeRemainingToday(self, productId, mi) : 0;
}

// ─────────────────────────────────────────────
// §13  SetupHooks — chạy sau InitIl2cpp + assemblies stable
// ─────────────────────────────────────────────
static unsigned SetupHooks() {
    unsigned installed = 0;

    // Speed
    uintptr_t a_getTS = Tim_Ham("UnityEngine", "Time", "get_timeScale", 0);
    uintptr_t a_setTS = Tim_Ham("UnityEngine", "Time", "set_timeScale", 1);
    installed += DoHook((void*)a_getTS, (void*)h_getTS, (void**)&o_getTS, "Time.get_timeScale");
    installed += DoHook((void*)a_setTS, (void*)h_setTS, (void**)&o_setTS, "Time.set_timeScale");

    // Free Shop — IAPClientManager hooks
    uintptr_t a_tryPurchase = Tim_Ham("", "IAPClientManager", "TryPurchase", 2);
    installed += DoHook((void*)a_tryPurchase, (void*)h_TryPurchase,
                        (void**)&o_TryPurchase, "IAPClientManager.TryPurchase");

    uintptr_t a_hasEntitlement = Tim_Ham("", "IAPClientManager", "HasActiveEntitlement", 1);
    installed += DoHook((void*)a_hasEntitlement, (void*)h_HasActiveEntitlement,
                        (void**)&o_HasActiveEntitlement, "IAPClientManager.HasActiveEntitlement");

    uintptr_t a_getDays = Tim_Ham("", "IAPClientManager", "GetEntitlementRemainingDays", 1);
    installed += DoHook((void*)a_getDays, (void*)h_GetEntitlementRemainingDays,
                        (void**)&o_GetEntitlementRemainingDays, "IAPClientManager.GetEntitlementRemainingDays");

    uintptr_t a_freeAd = Tim_Ham("", "IAPClientManager", "TryFreeRewardAd", 0);
    installed += DoHook((void*)a_freeAd, (void*)h_TryFreeRewardAd,
                        (void**)&o_TryFreeRewardAd, "IAPClientManager.TryFreeRewardAd");

    uintptr_t a_adFreeToday = Tim_Ham("", "IAPClientManager", "GetAdFreeRemainingToday", 1);
    installed += DoHook((void*)a_adFreeToday, (void*)h_GetAdFreeRemainingToday,
                        (void**)&o_GetAdFreeRemainingToday, "IAPClientManager.GetAdFreeRemainingToday");

    // Cache GrantOnceRewards (private, not hooked — called directly in h_TryPurchase)
    s_addr_GrantOnceRewards = Tim_Ham("", "IAPClientManager", "GrantOnceRewards", 1);
    LOGI("[%s] GrantOnceRewards addr: %s @ %p",
         BUILD_ID, s_addr_GrantOnceRewards ? "OK" : "FAIL", (void*)s_addr_GrantOnceRewards);

    return installed;
}

// ─────────────────────────────────────────────
// §14  ParseMenuConfig (7-field HAMOD format)
// ─────────────────────────────────────────────
static bool ParseMenuConfig(const char* text, bool* shop, float* speed,
                            bool* has_speed, unsigned* field_count) {
    if (!text || !shop || !speed || !has_speed || !field_count) return false;
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
    if ((count != 1 && count != 2 && count != 7) ||
        (values[0] != 0.0f && values[0] != 1.0f)) return false;
    *shop      = values[0] == 1.0f;
    *has_speed = count >= 2;
    if (*has_speed)
        *speed = values[1] < 1.0f ? 1.0f : (values[1] > 10.0f ? 10.0f : values[1]);
    *field_count = count;
    return true;
}

// ─────────────────────────────────────────────
// §15  Worker thread — chờ IL2CPP, cài hook
// ─────────────────────────────────────────────
static void* h_t(void*) {
    for (int i = 0; i < 120; ++i) {
        if (isIl2cppLoaded()) break;
        sleep(1);
    }
    if (!InitIl2cpp()) {
        LOGE("[%s] InitIl2cpp FAILED — abort", BUILD_ID);
        return nullptr;
    }
    // Chờ assemblies stable
    void* domain = il2cpp_domain_get();
    void* thread = (domain && il2cpp_thread_attach) ? il2cpp_thread_attach(domain) : nullptr;
    size_t last = 0; int stable = 0;
    for (int i = 0; i < 60 && stable < 5; ++i) {
        sleep(1);
        size_t count = 0;
        void** asm_ = il2cpp_domain_get_assemblies ? il2cpp_domain_get_assemblies(domain, &count) : nullptr;
        if (asm_ && count == last && count > 0) ++stable; else { last = count; stable = 0; }
    }
    LOGI("[%s] Assemblies stable: %zu", BUILD_ID, last);

    unsigned installed = SetupHooks();
    LOGI("[%s] HOOK DONE: %u installed", BUILD_ID, installed);

    if (il2cpp_thread_detach && thread) il2cpp_thread_detach(thread);
    return nullptr;
}

// ─────────────────────────────────────────────
// §16  Config reader thread
// ─────────────────────────────────────────────
static void* Doc_Config_Ngam(void*) {
    char root[768];
    const char* configured_root = getenv("MODMENU_LOG_DIR");
    const char* source_root = configured_root && *configured_root ? configured_root :
        "/data/data/menu.nextgen.launcher/files";
    const int root_length = snprintf(root, sizeof(root), "%s", source_root);
    if (root_length < 0 || static_cast<size_t>(root_length) >= sizeof(root)) {
        LOGE("[%s] CONFIG: MODMENU_LOG_DIR too long", BUILD_ID); return nullptr;
    }
    size_t root_size = strlen(root);
    while (root_size > 1 && root[root_size - 1] == '/') root[--root_size] = '\0';
    const char* leaf = strrchr(root, '/'); leaf = leaf ? leaf + 1 : root;
    const bool root_is_mods = strcmp(leaf, "mods") == 0;
    const char* names[] = {
        "config_com.and1games.xzyx.txt",
        "config_COM.AND1GAMES.XZYX.txt"
    };
    char paths[4][1024] = {}; size_t path_count = 0;
    for (unsigned location = 0; location < 2; ++location) {
        const bool append_mods = root_is_mods ? location == 1 : location == 0;
        for (const char* name : names) {
            const int n = snprintf(paths[path_count], sizeof(paths[path_count]),
                                   "%s/%s%s", root, append_mods ? "mods/" : "", name);
            if (n > 0 && static_cast<size_t>(n) < sizeof(paths[path_count])) ++path_count;
        }
    }
    LOGI("[%s] CONFIG begin: pid=%d", BUILD_ID, getpid());
    for (size_t i = 0; i < path_count; ++i) LOGI("[%s] CONFIG candidate[%zu]=%s", BUILD_ID, i, paths[i]);

    int last_status = -1, last_path = -1;
    bool last_shop = false; float last_speed = 1.0f;
    while (true) {
        FILE* file = nullptr; int selected = -1;
        for (size_t i = 0; i < path_count; ++i) {
            file = fopen(paths[i], "r");
            if (file) { selected = (int)i; break; }
        }
        if (!file) {
            if (last_status != 0) LOGE("[%s] CONFIG missing; values retained", BUILD_ID);
            last_status = 0; usleep(500000); continue;
        }
        char text[256] = {};
        const size_t length = fread(text, 1, sizeof(text) - 1, file);
        const bool read_ok = !ferror(file) && length < sizeof(text) - 1;
        fclose(file);
        bool shop = false, has_speed = false; unsigned field_count = 0;
        float speed = g_feature1.load(std::memory_order_acquire);
        if (!read_ok || memchr(text, '\0', length) ||
            !ParseMenuConfig(text, &shop, &speed, &has_speed, &field_count)) {
            if (last_status != 1 || last_path != selected)
                LOGE("[%s] CONFIG invalid: path=%s expected 1/2/7 fields", BUILD_ID, paths[selected]);
            last_status = 1; last_path = selected; usleep(500000); continue;
        }
        if (has_speed) {
            g_feature1.store(speed, std::memory_order_release);
            // Active apply: game gọi set_timeScale 1 lần lúc startup nên hook thụ động không đủ
            static float s_last_applied = 1.0f;
            if (o_setTS && speed != s_last_applied) {
                o_setTS(s_last_game_ts * speed, nullptr);
                s_last_applied = speed;
                LOGI("[%s] Speed applied: base=%.2f x%.1f = %.2f",
                     BUILD_ID, s_last_game_ts, speed, s_last_game_ts * speed);
            }
        }
        g_config_loaded_once.store(true, std::memory_order_release);
        g_feature0.store(shop, std::memory_order_release);
        if (last_status != 2 || last_path != selected || last_shop != shop || last_speed != speed)
            LOGI("[%s] CONFIG applied: path=%s shop=%d speed=%.1f fields=%u",
                 BUILD_ID, paths[selected], shop, speed, field_count);
        last_status = 2; last_path = selected; last_shop = shop; last_speed = speed;
        usleep(500000);
    }
    return nullptr;
}

// ─────────────────────────────────────────────
// §17  Entry point
// ─────────────────────────────────────────────
__attribute__((constructor))
void l_m() {
    LOGI("[%s] MOD injected!", BUILD_ID);
    pthread_t t1, t2;
    int rc = pthread_create(&t1, nullptr, h_t, nullptr);
    if (rc == 0) pthread_detach(t1); else LOGE("[%s] Worker create failed: %d", BUILD_ID, rc);
    rc = pthread_create(&t2, nullptr, Doc_Config_Ngam, nullptr);
    if (rc == 0) pthread_detach(t2); else LOGE("[%s] Config thread create failed: %d", BUILD_ID, rc);
}
