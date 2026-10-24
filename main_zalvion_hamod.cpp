// HAMOD -- Zalvion (com.fenragames.zalvion)
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

#define BUILD_ID "ZLV_v1"
#define LOG_TAG  "ZLV_MOD"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ===== Feature globals =====
// menu.js index 0: seekbar speed 0-4 → SPD_MUL[5]
// menu.js index 1: toggle free shop + VIP + bỏ QC
static std::atomic<int>   g_speed_idx{1};   // default x1
static std::atomic<bool>  g_shop{false};
static std::atomic<float> g_ts_game{1.0f};
static std::atomic<bool>  g_config_loaded_once{false};
static const float SPD_MUL[5] = {0.5f, 1.0f, 2.0f, 3.0f, 5.0f};

// ===== IL2CPP API =====
static void*       (*il2cpp_domain_get)()                                        = nullptr;
static void*       (*il2cpp_thread_attach)(void*)                                = nullptr;
static void        (*il2cpp_thread_detach)(void*)                                = nullptr;
static void**      (*il2cpp_domain_get_assemblies)(void*, size_t*)               = nullptr;
static void*       (*il2cpp_assembly_get_image)(void*)                           = nullptr;
static void*       (*il2cpp_class_from_name)(void*, const char*, const char*)    = nullptr;
static void*       (*il2cpp_class_get_method_from_name)(void*, const char*, int) = nullptr;
static void*       (*il2cpp_class_get_methods)(void*, void**)                    = nullptr;
static const char* (*il2cpp_method_get_name)(void*)                              = nullptr;
static uint32_t    (*il2cpp_method_get_param_count)(void*)                       = nullptr;
static size_t      (*il2cpp_image_get_class_count)(void*)                        = nullptr;
static void*       (*il2cpp_image_get_class)(void*, size_t)                      = nullptr;
static const char* (*il2cpp_class_get_name_raw)(void*)                           = nullptr;

// ===== ELF image tracker =====
struct LoadedIl2CppImage {
    uintptr_t         base;
    const ElfW(Phdr)* phdr;
    size_t            phnum;
    const char*       path;
};
static LoadedIl2CppImage g_il2cpp_image = {};
static void* g_il2cpp_handle = nullptr;

// ===== ImageContains =====
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
    memcpy(value, reinterpret_cast<const void*>(address), sizeof(*value)); return true;
}
static uintptr_t ImageDynamicAddress(ElfW(Addr) value, size_t bytes) {
    if (!value) return 0;
    if (ImageContains(static_cast<uintptr_t>(value), bytes)) return value;
    if (value > UINTPTR_MAX - g_il2cpp_image.base) return 0;
    uintptr_t address = g_il2cpp_image.base + value;
    return ImageContains(address, bytes) ? address : 0;
}

// ===== Il2CppExports (ELF symbol parser) =====
class Il2CppExports {
    uintptr_t symbols_ = 0, strings_ = 0, sysv_ = 0, gnu_ = 0;
    size_t string_size_ = 0;
    void* SymbolAt(uint32_t index, const char* name) const {
        uintptr_t address = ImageArrayAddress(symbols_, index, sizeof(ElfW(Sym)));
        if (!ImageContains(address, sizeof(ElfW(Sym)))) return nullptr;
        ElfW(Sym) symbol; memcpy(&symbol, reinterpret_cast<const void*>(address), sizeof(symbol));
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
        ElfW(Addr) sym = 0, str = 0, sysv = 0, gnu = 0; size_t stride = sizeof(ElfW(Sym)); bool terminated = false;
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
            ImageReadU32(gnu_, &bucket_count); ImageReadU32(gnu_ + 4, &first_symbol); ImageReadU32(gnu_ + 8, &bloom_count);
            uintptr_t buckets = ImageArrayAddress(gnu_ + 16, bloom_count, sizeof(ElfW(Addr)));
            uintptr_t chains  = ImageArrayAddress(buckets, bucket_count, sizeof(uint32_t));
            if (bucket_count && bloom_count && buckets && chains) {
                uint32_t hash = 5381, index = 0;
                for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p) hash = hash * 33 + *p;
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
            uintptr_t buckets = sysv_ + 8, chains = ImageArrayAddress(buckets, bucket_count, sizeof(uint32_t));
            if (bucket_count && symbol_count && chains) {
                uint32_t hash = 0, index = 0;
                for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p) {
                    hash = (hash << 4) + *p; uint32_t hi = hash & 0xf0000000u;
                    if (hi) hash ^= hi >> 24; hash &= ~hi;
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

// ===== isIl2cppLoaded =====
static bool isIl2cppLoaded() {
    bool found = false;
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* data) -> int {
        if (!info->dlpi_name || !info->dlpi_phdr || !info->dlpi_phnum) return 0;
        const char* leaf = strrchr(info->dlpi_name, '/');
        leaf = leaf ? leaf + 1 : info->dlpi_name;
        if (strcmp(leaf, "libil2cpp.so") != 0) return 0;
        g_il2cpp_image = {info->dlpi_addr, info->dlpi_phdr, info->dlpi_phnum, info->dlpi_name};
        *static_cast<bool*>(data) = true;
        return 1;
    }, &found);
    return found;
}

// ===== GetIl2cppHandle =====
static void* GetIl2cppHandle() {
    if (g_il2cpp_handle) return g_il2cpp_handle;
    const int flags = RTLD_NOW | RTLD_NOLOAD;
    if (g_il2cpp_image.path && *g_il2cpp_image.path)
        g_il2cpp_handle = dlopen(g_il2cpp_image.path, flags);
    if (!g_il2cpp_handle)
        g_il2cpp_handle = dlopen(OBFUSCATE("libil2cpp.so"), flags);
    if (!g_il2cpp_handle) LOGI("GetHandle unavailable; using mapped ELF exports");
    return g_il2cpp_handle;
}

// ===== InitIl2cpp =====
static bool InitIl2cpp() {
    if (!isIl2cppLoaded()) return false;
    LOGI("[%s] Resolve BEGIN base=%p", BUILD_ID, reinterpret_cast<void*>(g_il2cpp_image.base));
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
#define LOAD_API(fn, req) fn = reinterpret_cast<decltype(fn)>(resolve(#fn, req))
    LOAD_API(il2cpp_domain_get,              true);
    LOAD_API(il2cpp_thread_attach,           true);
    LOAD_API(il2cpp_domain_get_assemblies,   true);
    LOAD_API(il2cpp_assembly_get_image,      true);
    LOAD_API(il2cpp_class_from_name,         true);
    LOAD_API(il2cpp_class_get_method_from_name, true);
    LOAD_API(il2cpp_thread_detach,           false);
    LOAD_API(il2cpp_class_get_methods,       false);
    LOAD_API(il2cpp_method_get_name,         false);
    LOAD_API(il2cpp_method_get_param_count,  false);
    LOAD_API(il2cpp_image_get_class_count,   false);
    LOAD_API(il2cpp_image_get_class,         false);
    il2cpp_class_get_name_raw = reinterpret_cast<decltype(il2cpp_class_get_name_raw)>(
        resolve("il2cpp_class_get_name", false));
#undef LOAD_API
    LOGI("[%s] Resolve END handle=%u ELF=%u missing=%u", BUILD_ID, from_handle, from_image, missing);
    return missing == 0;
}

// ===== Tim_Ham =====
static uintptr_t Tim_Ham(const char* ns, const char* cls, const char* method, int argc) {
    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies ||
        !il2cpp_assembly_get_image || !il2cpp_class_from_name || !il2cpp_class_get_method_from_name) return 0;
    void* domain = il2cpp_domain_get(); if (!domain) return 0;
    size_t n = 0; void** asms = il2cpp_domain_get_assemblies(domain, &n);
    for (size_t i = 0; i < n; ++i) {
        void* img = il2cpp_assembly_get_image(asms[i]); if (!img) continue;
        void* k = il2cpp_class_from_name(img, ns, cls); if (!k) continue;
        void* m = il2cpp_class_get_method_from_name(k, method, argc); if (!m) continue;
        return reinterpret_cast<uintptr_t>(*(void**)m);
    }
    return 0;
}

static void* FindClassAnywhere(const char* className) {
    void* domain = il2cpp_domain_get(); if (!domain) return nullptr;
    size_t nasm = 0; void** asms = il2cpp_domain_get_assemblies(domain, &nasm);
    for (size_t i = 0; i < nasm; ++i) {
        void* img = il2cpp_assembly_get_image(asms[i]); if (!img) continue;
        if (il2cpp_image_get_class && il2cpp_image_get_class_count && il2cpp_class_get_name_raw) {
            size_t nc = il2cpp_image_get_class_count(img);
            for (size_t j = 0; j < nc; ++j) {
                void* kls = il2cpp_image_get_class(img, j); if (!kls) continue;
                const char* n = il2cpp_class_get_name_raw(kls);
                if (n && strcmp(n, className) == 0) return kls;
            }
        } else {
            void* k = il2cpp_class_from_name(img, "", className); if (k) return k;
        }
    }
    LOGE("FindClassAnywhere[%s] NOT FOUND", className); return nullptr;
}

// ===== DoHook =====
static bool DoHook(void* fn, void* hook, void** orig, const char* tag) {
    if (!hook || !orig || !ImageContains(reinterpret_cast<uintptr_t>(fn), 1, PF_X)) {
        LOGE("Hook SKIP (invalid address): %s @ %p", tag, fn); return false;
    }
    const int result = DobbyHook(fn, hook, orig);
    const bool ok = result == 0 && *orig != nullptr;
    LOGI("Hook %s: %s @ %p", ok ? "OK" : "FAIL", tag, fn);
    return ok;
}

// ===== Hook: Speed =====
typedef void (*t_set_ts)(float, void*);
static t_set_ts o_set_ts = nullptr;
static void h_set_ts(float v, void* mi) {
    g_ts_game.store(v, std::memory_order_relaxed);
    int s = g_speed_idx.load(std::memory_order_relaxed);
    float mul = (s >= 0 && s < 5) ? SPD_MUL[s] : 1.0f;
    if (o_set_ts) o_set_ts(v * mul, mi);
}

typedef float (*t_get_ts)(void*);
static t_get_ts o_get_ts = nullptr;
static float h_get_ts(void* mi) {
    (void)mi;
    return g_ts_game.load(std::memory_order_relaxed);
}

// ===== Hook: Free Shop =====

// Cache ShopScreen.GrantContents(Product) — private static, 1 param (product, mi)
typedef void (*t_GrantContents)(void*, void*);
static t_GrantContents g_fn_GrantContents = nullptr;

// Cache IapManager.OwnedAdd(string sku) — private static, 1 param (sku, mi)
typedef void (*t_OwnedAdd)(void*, void*);
static t_OwnedAdd g_fn_OwnedAdd = nullptr;

// IapManager.CanBuy(string sku) — static, 1 param → bool
typedef bool (*t_CanBuy)(void*, void*);
static t_CanBuy o_CanBuy = nullptr;
static bool h_CanBuy(void* sku, void* mi) {
    if (g_shop.load(std::memory_order_relaxed)) return true;
    return o_CanBuy ? o_CanBuy(sku, mi) : false;
}

// VipStatus.get_IsVip() — static, 0 params → bool
// VIP server-side; GrantContents không grant được → hook getter
typedef bool (*t_IsVip)(void*);
static t_IsVip o_IsVip = nullptr;
static bool h_IsVip(void* mi) {
    if (g_shop.load(std::memory_order_relaxed)) return true;
    return o_IsVip ? o_IsVip(mi) : false;
}

// AdService.get_UseFakeAds() — private static, 0 params → bool
// Khi true: ShowRewarded gọi ShowFakeAd(onComplete) → bypass QC revive
typedef bool (*t_UseFakeAds)(void*);
static t_UseFakeAds o_UseFakeAds = nullptr;
static bool h_UseFakeAds(void* mi) {
    if (g_shop.load(std::memory_order_relaxed)) return true;
    return o_UseFakeAds ? o_UseFakeAds(mi) : false;
}

// ShopScreen.PurchaseRealAsync(Product) — instance, 1 param
typedef void (*t_PurchaseRealAsync)(void*, void*, void*);
static t_PurchaseRealAsync o_PurchaseRealAsync = nullptr;
static void h_PurchaseRealAsync(void* _this, void* product, void* mi) {
    if (g_shop.load(std::memory_order_relaxed)) {
        LOGI("FreeShop: PurchaseRealAsync -> GrantContents");
        if (g_fn_GrantContents) g_fn_GrantContents(product, nullptr);
        else LOGE("FreeShop: GrantContents not cached!");
        if (g_fn_OwnedAdd && product) {
            void* sku = *(void**)((uintptr_t)product + 0x10);
            if (sku) { g_fn_OwnedAdd(sku, nullptr); LOGI("FreeShop: OwnedAdd OK"); }
        }
        return;
    }
    if (o_PurchaseRealAsync) o_PurchaseRealAsync(_this, product, mi);
}

// ShopScreen.PurchaseLocal(Product) — instance, 1 param
typedef void (*t_PurchaseLocal)(void*, void*, void*);
static t_PurchaseLocal o_PurchaseLocal = nullptr;
static void h_PurchaseLocal(void* _this, void* product, void* mi) {
    if (g_shop.load(std::memory_order_relaxed)) {
        LOGI("FreeShop: PurchaseLocal -> GrantContents");
        if (g_fn_GrantContents) g_fn_GrantContents(product, nullptr);
        else LOGE("FreeShop: GrantContents not cached!");
        if (g_fn_OwnedAdd && product) {
            void* sku = *(void**)((uintptr_t)product + 0x10);
            if (sku) { g_fn_OwnedAdd(sku, nullptr); LOGI("FreeShop: OwnedAdd OK"); }
        }
        return;
    }
    if (o_PurchaseLocal) o_PurchaseLocal(_this, product, mi);
}

// ===== Worker thread =====
static void* h_t(void*) {
    for (int i = 0; i < 120; ++i) {
        if (isIl2cppLoaded()) break;
        sleep(1);
    }
    if (!g_il2cpp_image.base) { LOGE("libil2cpp.so not found"); return nullptr; }
    if (!InitIl2cpp()) { LOGE("InitIl2cpp FAILED"); return nullptr; }

    void* domain = il2cpp_domain_get();
    void* thread = (domain && il2cpp_thread_attach) ? il2cpp_thread_attach(domain) : nullptr;

    size_t last = 0; int stable = 0;
    for (int i = 0; i < 60 && stable < 5; ++i) {
        sleep(1);
        size_t count = 0;
        void** a = il2cpp_domain_get_assemblies ? il2cpp_domain_get_assemblies(domain, &count) : nullptr;
        if (a && count == last && count > 0) ++stable; else { last = count; stable = 0; }
    }
    if (stable < 5) {
        LOGE("Assemblies not ready");
        if (il2cpp_thread_detach) il2cpp_thread_detach(thread);
        return nullptr;
    }
    LOGI("Assemblies stable: %zu", last);

    unsigned installed = 0;

    // Speed
    {
        uintptr_t a = Tim_Ham(OBFUSCATE("UnityEngine"), OBFUSCATE("Time"), OBFUSCATE("set_timeScale"), 1);
        installed += DoHook((void*)a, (void*)h_set_ts, (void**)&o_set_ts, "Time.set_timeScale");
        a = Tim_Ham(OBFUSCATE("UnityEngine"), OBFUSCATE("Time"), OBFUSCATE("get_timeScale"), 0);
        installed += DoHook((void*)a, (void*)h_get_ts, (void**)&o_get_ts, "Time.get_timeScale");
    }

    // Free Shop + VIP + No Ads
    {
        // IapManager: CanBuy hook, OwnedAdd cache
        void* kIap = FindClassAnywhere(OBFUSCATE("IapManager"));
        if (kIap && il2cpp_class_get_method_from_name) {
            void* mCan = il2cpp_class_get_method_from_name(kIap, OBFUSCATE("CanBuy"), 1);
            if (mCan) installed += DoHook(*(void**)mCan, (void*)h_CanBuy, (void**)&o_CanBuy, "IapManager.CanBuy");
            else LOGE("IapManager.CanBuy(1) not found");

            void* mAdd = il2cpp_class_get_method_from_name(kIap, OBFUSCATE("OwnedAdd"), 1);
            if (mAdd) {
                g_fn_OwnedAdd = (decltype(g_fn_OwnedAdd)) *(void**)mAdd;
                LOGI("OwnedAdd cached: %p", (void*)g_fn_OwnedAdd);
            } else LOGE("IapManager.OwnedAdd(1) not found");
        } else LOGE("IapManager class NOT FOUND");

        // VipStatus.get_IsVip → true when g_shop
        {
            uintptr_t a = Tim_Ham(OBFUSCATE(""), OBFUSCATE("VipStatus"), OBFUSCATE("get_IsVip"), 0);
            if (a) installed += DoHook((void*)a, (void*)h_IsVip, (void**)&o_IsVip, "VipStatus.get_IsVip");
            else LOGE("VipStatus.get_IsVip(0) not found");
        }

        // AdService.get_UseFakeAds → true when g_shop
        {
            uintptr_t a = Tim_Ham(OBFUSCATE(""), OBFUSCATE("AdService"), OBFUSCATE("get_UseFakeAds"), 0);
            if (a) installed += DoHook((void*)a, (void*)h_UseFakeAds, (void**)&o_UseFakeAds, "AdService.get_UseFakeAds");
            else LOGE("AdService.get_UseFakeAds(0) not found");
        }

        // ShopScreen: GrantContents cache, PurchaseRealAsync hook, PurchaseLocal hook
        void* kShop = FindClassAnywhere(OBFUSCATE("ShopScreen"));
        if (kShop && il2cpp_class_get_method_from_name) {
            void* mGrant = il2cpp_class_get_method_from_name(kShop, OBFUSCATE("GrantContents"), 1);
            if (mGrant) {
                g_fn_GrantContents = (decltype(g_fn_GrantContents)) *(void**)mGrant;
                LOGI("GrantContents cached: %p", (void*)g_fn_GrantContents);
            } else LOGE("ShopScreen.GrantContents(1) not found");

            void* mPRA = il2cpp_class_get_method_from_name(kShop, OBFUSCATE("PurchaseRealAsync"), 1);
            if (mPRA) installed += DoHook(*(void**)mPRA, (void*)h_PurchaseRealAsync, (void**)&o_PurchaseRealAsync, "ShopScreen.PurchaseRealAsync");
            else LOGE("ShopScreen.PurchaseRealAsync(1) not found");

            void* mPL = il2cpp_class_get_method_from_name(kShop, OBFUSCATE("PurchaseLocal"), 1);
            if (mPL) installed += DoHook(*(void**)mPL, (void*)h_PurchaseLocal, (void**)&o_PurchaseLocal, "ShopScreen.PurchaseLocal");
            else LOGE("ShopScreen.PurchaseLocal(1) not found");
        } else LOGE("ShopScreen class NOT FOUND");
    }

    LOGI("[%s] HOOK DONE: %u installed", BUILD_ID, installed);
    if (il2cpp_thread_detach) il2cpp_thread_detach(thread);
    return nullptr;
}

// ===== ParseMenuConfig =====
// Config format: 7 floats "f0 f1 f2 f3 f4 f5 f6"
// f0 = speed seekbar 0-4 (0=x0.5, 1=x1, 2=x2, 3=x3, 4=x5)
// f1 = free shop + VIP + no ads toggle (0.0 or 1.0)
static bool ParseMenuConfig(const char* text, int* speed_idx, bool* shop) {
    if (!text || !speed_idx || !shop) return false;
    float values[7] = {}; unsigned count = 0;
    const char* cursor = text;
    while (true) {
        while (isspace((unsigned char)*cursor)) ++cursor;
        if (!*cursor) break;
        if (count == 7) return false;
        char* end = nullptr; errno = 0;
        float v = strtof(cursor, &end);
        if (end == cursor || errno == ERANGE || !std::isfinite(v) ||
            (*end && !isspace((unsigned char)*end))) return false;
        values[count++] = v;
        cursor = end;
    }
    if (count < 2) return false;
    int spd = (int)(values[0] + 0.5f);
    if (spd < 0) spd = 0; if (spd > 4) spd = 4;
    *speed_idx = spd;
    *shop = (values[1] >= 0.5f);
    return true;
}

// ===== Doc_Config_Ngam =====
static void* Doc_Config_Ngam(void*) {
    char root[768];
    const char* configured_root = getenv("MODMENU_LOG_DIR");
    const char* source_root = configured_root && *configured_root ? configured_root :
        "/data/data/menu.nextgen.launcher/files";
    int root_length = snprintf(root, sizeof(root), "%s", source_root);
    if (root_length < 0 || (size_t)root_length >= sizeof(root)) { LOGE("CONFIG: path too long"); return nullptr; }
    size_t root_size = strlen(root);
    while (root_size > 1 && root[root_size - 1] == '/') root[--root_size] = '\0';
    const char* leaf = strrchr(root, '/'); leaf = leaf ? leaf + 1 : root;
    const bool root_is_mods = strcmp(leaf, "mods") == 0;

    const char* names[] = {
        "config_com.fenragames.zalvion.txt",
        "config_COM.FENRAGAMES.ZALVION.txt"
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
    LOGI("[%s] CONFIG begin pid=%d", BUILD_ID, getpid());
    for (size_t i = 0; i < path_count; ++i) LOGI("CONFIG candidate[%zu]=%s", i, paths[i]);

    int last_status = -1, last_path = -1, last_spd = -1; bool last_shop = false;
    static float s_last_applied_speed = 1.0f;

    while (true) {
        FILE* file = nullptr; int selected = -1;
        for (size_t i = 0; i < path_count; ++i) {
            file = fopen(paths[i], "r"); if (file) { selected = (int)i; break; }
        }
        if (!file) {
            if (last_status != 0) LOGE("CONFIG missing; values retained");
            last_status = 0; usleep(500000); continue;
        }
        char text[256] = {};
        const size_t length = fread(text, 1, sizeof(text) - 1, file);
        const bool read_ok = !ferror(file) && length < sizeof(text) - 1;
        fclose(file);
        int spd = 1; bool shop = false;
        if (!read_ok || memchr(text, '\0', length) || !ParseMenuConfig(text, &spd, &shop)) {
            if (last_status != 1 || last_path != selected)
                LOGE("CONFIG invalid: %s (expected >=2 floats)", paths[selected]);
            last_status = 1; last_path = selected; usleep(500000); continue;
        }
        g_speed_idx.store(spd, std::memory_order_release);
        g_shop.store(shop, std::memory_order_release);
        g_config_loaded_once.store(true, std::memory_order_release);

        float new_speed = SPD_MUL[spd];
        if (o_set_ts && new_speed != s_last_applied_speed) {
            float cur = g_ts_game.load(std::memory_order_relaxed);
            o_set_ts(cur * new_speed, nullptr);
            s_last_applied_speed = new_speed;
            LOGI("Speed applied: x%.1f (ts_game=%.2f -> %.2f)", new_speed, cur, cur * new_speed);
        }

        if (last_status != 2 || last_path != selected || last_spd != spd || last_shop != shop)
            LOGI("CONFIG applied: path=%s spd=x%.1f shop=%d", paths[selected], SPD_MUL[spd], shop);
        last_status = 2; last_path = selected; last_spd = spd; last_shop = shop;
        usleep(500000);
    }
    return nullptr;
}

// ===== Entry point =====
__attribute__((constructor))
void l_m() {
    LOGI("[%s] MOD injected!", BUILD_ID);
    pthread_t t1, t2;
    int rc = pthread_create(&t1, nullptr, h_t, nullptr);
    if (rc == 0) pthread_detach(t1); else LOGE("Worker create failed: %d", rc);
    rc = pthread_create(&t2, nullptr, Doc_Config_Ngam, nullptr);
    if (rc == 0) pthread_detach(t2); else LOGE("Config thread create failed: %d", rc);
}
