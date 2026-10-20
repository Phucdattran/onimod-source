// ============================================================
//  HAMOD — Draft Showdown (com.xiaomo.lastextract)
//  Free Shop + Speed — Voodoo SDK mới (HandlePurchase đã bị xóa)
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

#define BUILD_ID "LAST_v1"
#define LOG_TAG  "LAST_MOD"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ═══════════════════════════════════════════════════════════
//  TRẠNG THÁI MENU
//    index 0 → g_feature0 (Free Shop toggle)
//    index 1 → g_feature1 (Speed seekbar, 1–20)
// ═══════════════════════════════════════════════════════════
static std::atomic<bool>  g_feature0{false};
static std::atomic<float> g_feature1{1.0f};
static std::atomic<bool>  g_config_loaded_once{false};

// ═══════════════════════════════════════════════════════════
//  IL2CPP API
// ═══════════════════════════════════════════════════════════
void*       (*il2cpp_domain_get)()                                         = nullptr;
void*       (*il2cpp_thread_attach)(void*)                                 = nullptr;
void        (*il2cpp_thread_detach)(void*)                                 = nullptr;
void**      (*il2cpp_domain_get_assemblies)(void*, size_t*)                = nullptr;
void*       (*il2cpp_assembly_get_image)(void*)                            = nullptr;
void*       (*il2cpp_class_from_name)(void*, const char*, const char*)     = nullptr;
void*       (*il2cpp_class_get_methods)(void*, void**)                     = nullptr;
const char* (*il2cpp_method_get_name)(void*)                               = nullptr;
uint32_t    (*il2cpp_method_get_param_count)(void*)                        = nullptr;
void*       (*il2cpp_runtime_invoke)(void*, void*, void**, void**)         = nullptr;
void*       (*il2cpp_object_get_class)(void*)                              = nullptr;
void*       (*il2cpp_class_get_method_from_name)(void*, const char*, int)  = nullptr;
void*       (*il2cpp_class_get_field_from_name)(void*, const char*)        = nullptr;
int32_t     (*il2cpp_field_get_offset)(void*)                              = nullptr;
size_t      (*il2cpp_image_get_class_count)(void*)                         = nullptr;
void*       (*il2cpp_image_get_class)(void*, size_t)                       = nullptr;
const char* (*il2cpp_class_get_name_raw)(void*)                            = nullptr;
void*       (*il2cpp_method_get_param)(void*, uint32_t)                    = nullptr;
int         (*il2cpp_type_get_type)(void*)                                 = nullptr;
bool        (*il2cpp_type_is_byref)(void*)                                 = nullptr;
void        (*il2cpp_format_exception)(const void*, char*, int)            = nullptr;

// ═══════════════════════════════════════════════════════════
//  ELF IMAGE — capture từ dl_iterate_phdr
// ═══════════════════════════════════════════════════════════
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
        unsigned type = symbol.st_info & 0xf, binding = symbol.st_info >> 4;
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
//  isIl2cppLoaded() + GetIl2cppHandle() + InitIl2cpp()
// ═══════════════════════════════════════════════════════════
bool isIl2cppLoaded() {
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
    if (!g_il2cpp_handle)
        LOGI("GetHandle unavailable; will use mapped ELF exports");
    return g_il2cpp_handle;
}

bool InitIl2cpp() {
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

    LOGI("[%s] Resolve END: handle=%u mappedELF=%u missing=%u",
         BUILD_ID, from_handle, from_image, missing);
    return missing == 0;
}

// ═══════════════════════════════════════════════════════════
//  Tim_Ham() — tìm method address
// ═══════════════════════════════════════════════════════════
uintptr_t Tim_Ham(const char* ns, const char* className, const char* methodName, int argsCount) {
    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies ||
        !il2cpp_assembly_get_image || !il2cpp_class_from_name ||
        !il2cpp_class_get_method_from_name) return 0;
    void* domain = il2cpp_domain_get();
    if (!domain) return 0;
    size_t size = 0;
    void** assemblies = il2cpp_domain_get_assemblies(domain, &size);
    if (!assemblies) return 0;
    for (size_t i = 0; i < size; ++i) {
        void* image = il2cpp_assembly_get_image(assemblies[i]);
        if (!image) continue;
        void* klass = il2cpp_class_from_name(image, ns, className);
        if (klass) {
            void* method = il2cpp_class_get_method_from_name(klass, methodName, argsCount);
            if (method) return (uintptr_t)*(void**)method;
        }
    }
    return 0;
}

// ═══════════════════════════════════════════════════════════
//  DoHook() — validate địa chỉ trước khi hook
// ═══════════════════════════════════════════════════════════
bool DoHook(void* fn, void* hook, void** orig, const char* tag) {
    if (!hook || !orig || !ImageContains(reinterpret_cast<uintptr_t>(fn), 1, PF_X)) {
        LOGE("Hook SKIP (invalid address): %s @ %p", tag, fn);
        return false;
    }
    const int result = DobbyHook(fn, hook, orig);
    const bool ok = result == 0 && *orig != nullptr;
    LOGI("Hook %s: %s @ %p", ok ? "OK" : "FAIL", tag, fn);
    return ok;
}

// ═══════════════════════════════════════════════════════════
//  SPEED — hook Time.set_timeScale + get_timeScale
// ═══════════════════════════════════════════════════════════
void  (*o_set_timeScale)(float, void*) = nullptr;
float (*o_get_timeScale)(void*)        = nullptr;
static float g_game_timeScale = 1.0f;

void h_set_timeScale(float value, void* mi) {
    g_game_timeScale = value;
    float speed = g_feature1.load(std::memory_order_acquire);
    if (o_set_timeScale) o_set_timeScale(value * speed, mi);
}

float h_get_timeScale(void* mi) {
    float speed = g_feature1.load(std::memory_order_acquire);
    if (speed != 1.0f) return g_game_timeScale;
    return o_get_timeScale ? o_get_timeScale(mi) : g_game_timeScale;
}

// ═══════════════════════════════════════════════════════════
//  FREE SHOP
//
//  HAMOD không có Google Play → billing không bao giờ bắt đầu →
//  VoodooSauce gọi HandlePurchaseFailureByBundle(bundle, NOT_SUPPORTED, ...) →
//  _listenerHandler.OnPurchaseFailure(reason, productInfo=null, desc, storeId).
//  LGL cần user bấm hủy (billing dialog → huỷ → HandlePurchaseFailure với productInfo non-null).
//
//  Giải pháp HAMOD:
//  1. Hook IAPMediator.BuyIAPProduct → lưu (self_iap, productId=bundle+g_bundle_productId_offset, storeId)
//     Offset được resolve động tại runtime qua il2cpp_field_get_offset("ProductId") — dump.cs không đáng tin
//  2. Hook ListenerHandler.OnPurchaseFailure:
//     - Nếu productInfo=null → gọi IAPMediator.GetProductWithId(savedProductId, savedStoreId)
//     - Sau đó gọi ListenerHandler.OnPurchaseComplete → dispatch chain → cấp hàng + tắt spinner
//
//  dump.cs:
//    IAPMediator (VoodooSauce.dll, Voodoo.Sauce.IAP):
//      GetProductWithId(String, StoreIdentifier)  // 0xB76CB58
//      BuyIAPProduct(Bundle, IValidator, StoreIdentifier)  // 3 params
//    ListenerHandler (Voodoo.IAP.dll, Voodoo.Sauce.IAP):
//      OnPurchaseComplete(ProductReceivedInfo, StoreIdentifier)  // 0xB3CE6B8
//      OnPurchaseFailure(reason, ProductReceivedInfo, String, StoreIdentifier)  // 0xB3CEC40
//    Bundle (Voodoo.IAP.dll):
//      _serializerSettings  @ 0x10  ← NOT productId, JSON settings object (always non-null)
//      ProductId (String*)  @ 0x18  ← correct offset confirmed from dump.cs
// ═══════════════════════════════════════════════════════════

// Context lưu lại khi BuyIAPProduct được gọi
static void*   g_saved_iap_self  = nullptr;
static void*   g_saved_productId = nullptr;
static int32_t g_saved_storeId   = 0;
// Offset của Bundle.ProductId — resolve động tại runtime (dump.cs có thể sai)
static int32_t g_bundle_productId_offset = -1;

// IAPMediator.GetProductWithId(self, productId, storeId, mi) → ProductReceivedInfo
static void* (*g_IAP_GetProductWithId)(void*, void*, int32_t, void*) = nullptr;
static void* g_IAP_GetProductWithId_mi = nullptr;

// ListenerHandler.OnPurchaseComplete(self, productInfo, storeId, mi)
static void (*g_LH_OnPurchaseComplete)(void*, void*, int32_t, void*) = nullptr;
static void* g_LH_OnPurchaseComplete_mi = nullptr;

// Hook 1: IAPMediator.BuyIAPProduct(self, bundle, validator, storeId, mi)
// "bundle" param IS System.String (product ID) — confirmed from memory dump.
void (*o_BuyIAPProduct)(void*, void*, void*, int32_t, void*) = nullptr;
void h_BuyIAPProduct(void* self, void* bundle, void* validator,
                     int32_t storeId, void* mi) {
    if (g_feature0.load(std::memory_order_acquire) && bundle) {
        g_saved_iap_self  = self;
        g_saved_productId = bundle;  // bundle IS the product ID string
        g_saved_storeId   = storeId;
        LOGI("[FreeShop] saved productId=%p storeId=%d", bundle, storeId);
    }
    if (o_BuyIAPProduct) o_BuyIAPProduct(self, bundle, validator, storeId, mi);
}

// Hook 2: ListenerHandler.OnPurchaseFailure(self, reason, productInfo, description, storeId, mi)
void (*o_LH_OnPurchaseFailure)(void*, int32_t, void*, void*, int32_t, void*) = nullptr;
void h_LH_OnPurchaseFailure(void* self, int32_t reason, void* productInfo,
                             void* description, int32_t storeId, void* mi) {
    if (g_feature0.load(std::memory_order_acquire)) {
        // HAMOD: không có GP → productInfo=null → tái tạo từ context đã lưu
        if (!productInfo && g_saved_productId && g_IAP_GetProductWithId && g_saved_iap_self) {
            productInfo = g_IAP_GetProductWithId(g_saved_iap_self, g_saved_productId,
                                                  g_saved_storeId, g_IAP_GetProductWithId_mi);
            LOGI("[FreeShop] rebuilt productInfo=%p (reason=%d)", productInfo, reason);
        }
        if (productInfo && g_LH_OnPurchaseComplete) {
            LOGI("[FreeShop] Failure(reason=%d) -> OnPurchaseComplete", reason);
            g_LH_OnPurchaseComplete(self, productInfo, storeId, g_LH_OnPurchaseComplete_mi);
            g_saved_productId = nullptr;
            return;
        }
        LOGE("[FreeShop] fallback: productInfo=%p LH.OPC=%p savedPid=%p",
             productInfo, (void*)g_LH_OnPurchaseComplete, g_saved_productId);
        g_saved_productId = nullptr;
    }
    if (o_LH_OnPurchaseFailure)
        o_LH_OnPurchaseFailure(self, reason, productInfo, description, storeId, mi);
}

// ═══════════════════════════════════════════════════════════
//  h_t() — worker thread: cài hook
// ═══════════════════════════════════════════════════════════
void* h_t(void*) {
    for (int i = 0; i < 120; ++i) {
        if (isIl2cppLoaded()) break;
        sleep(1);
    }
    if (!InitIl2cpp()) {
        LOGE("[%s] InitIl2cpp FAILED", BUILD_ID);
        return nullptr;
    }

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

    // Speed
    uintptr_t addr_getTS = Tim_Ham(OBFUSCATE("UnityEngine"), OBFUSCATE("Time"),
                                    OBFUSCATE("get_timeScale"), 0);
    installed += DoHook((void*)addr_getTS, (void*)h_get_timeScale,
                        (void**)&o_get_timeScale, "Time.get_timeScale");

    uintptr_t addr_setTS = Tim_Ham(OBFUSCATE("UnityEngine"), OBFUSCATE("Time"),
                                    OBFUSCATE("set_timeScale"), 1);
    installed += DoHook((void*)addr_setTS, (void*)h_set_timeScale,
                        (void**)&o_set_timeScale, "Time.set_timeScale");

    // Free Shop — 2 hooks: BuyIAPProduct (save context) + OnPurchaseFailure (redirect to success)
    {
        size_t count = 0;
        void** assemblies = il2cpp_domain_get_assemblies(domain, &count);

        // IAPMediator (VoodooSauce.dll) → GetProductWithId pointer + BuyIAPProduct hook
        for (size_t i = 0; i < count; ++i) {
            void* img = il2cpp_assembly_get_image(assemblies[i]);
            if (!img) continue;
            void* klass = il2cpp_class_from_name(img, OBFUSCATE("Voodoo.Sauce.IAP"),
                                                  OBFUSCATE("IAPMediator"));
            if (!klass) klass = il2cpp_class_from_name(img, OBFUSCATE(""),
                                                         OBFUSCATE("IAPMediator"));
            if (!klass) continue;

            void* mGPWI = il2cpp_class_get_method_from_name(klass,
                              OBFUSCATE("GetProductWithId"), 2);
            if (mGPWI && *(void**)mGPWI) {
                g_IAP_GetProductWithId    = (decltype(g_IAP_GetProductWithId))*(void**)mGPWI;
                g_IAP_GetProductWithId_mi = mGPWI;
                LOGI("[FreeShop] IAPMediator.GetProductWithId: %p",
                     (void*)g_IAP_GetProductWithId);
            } else {
                LOGE("[FreeShop] IAPMediator.GetProductWithId(2) NOT FOUND");
            }

            void* mBuy = il2cpp_class_get_method_from_name(klass,
                             OBFUSCATE("BuyIAPProduct"), 3);
            if (mBuy && *(void**)mBuy) {
                installed += DoHook(*(void**)mBuy, (void*)h_BuyIAPProduct,
                                    (void**)&o_BuyIAPProduct,
                                    "IAPMediator.BuyIAPProduct") ? 1 : 0;
            } else {
                LOGE("[FreeShop] IAPMediator.BuyIAPProduct(3) NOT FOUND");
            }
            break;
        }

        // ListenerHandler (Voodoo.IAP.dll) → OnPurchaseComplete pointer + OnPurchaseFailure hook
        bool found_lh = false;
        for (size_t i = 0; i < count && !found_lh; ++i) {
            void* img = il2cpp_assembly_get_image(assemblies[i]);
            if (!img) continue;
            void* klass = il2cpp_class_from_name(img, OBFUSCATE("Voodoo.IAP"),
                                                  OBFUSCATE("ListenerHandler"));
            if (!klass) klass = il2cpp_class_from_name(img, OBFUSCATE("Voodoo.Sauce.IAP"),
                                                         OBFUSCATE("ListenerHandler"));
            if (!klass) klass = il2cpp_class_from_name(img, OBFUSCATE(""),
                                                         OBFUSCATE("ListenerHandler"));
            if (!klass) continue;
            found_lh = true;

            // Bundle (cùng assembly với ListenerHandler) — lấy offset ProductId động
            {
                void* kBundle = il2cpp_class_from_name(img, OBFUSCATE("Voodoo.Sauce.IAP"),
                                                        OBFUSCATE("Bundle"));
                if (!kBundle) kBundle = il2cpp_class_from_name(img, OBFUSCATE("Voodoo.IAP"),
                                                                OBFUSCATE("Bundle"));
                if (!kBundle) kBundle = il2cpp_class_from_name(img, OBFUSCATE(""),
                                                                OBFUSCATE("Bundle"));
                if (kBundle && il2cpp_class_get_field_from_name && il2cpp_field_get_offset) {
                    void* fPid = il2cpp_class_get_field_from_name(kBundle, OBFUSCATE("ProductId"));
                    if (fPid) {
                        g_bundle_productId_offset = il2cpp_field_get_offset(fPid);
                        LOGI("[FreeShop] Bundle.ProductId offset = +0x%X",
                             (uint32_t)g_bundle_productId_offset);
                    } else {
                        LOGE("[FreeShop] Bundle.ProductId field NOT FOUND");
                    }
                } else {
                    LOGE("[FreeShop] Bundle class NOT FOUND in this assembly");
                }
            }

            void* mOPC = il2cpp_class_get_method_from_name(klass,
                             OBFUSCATE("OnPurchaseComplete"), 2);
            if (mOPC && *(void**)mOPC) {
                g_LH_OnPurchaseComplete    = (decltype(g_LH_OnPurchaseComplete))*(void**)mOPC;
                g_LH_OnPurchaseComplete_mi = mOPC;
                LOGI("[FreeShop] LH.OnPurchaseComplete: %p", (void*)g_LH_OnPurchaseComplete);
            } else {
                LOGE("[FreeShop] LH.OnPurchaseComplete(2) NOT FOUND");
            }

            void* mOPF = il2cpp_class_get_method_from_name(klass,
                             OBFUSCATE("OnPurchaseFailure"), 4);
            if (mOPF && *(void**)mOPF) {
                installed += DoHook(*(void**)mOPF, (void*)h_LH_OnPurchaseFailure,
                                    (void**)&o_LH_OnPurchaseFailure,
                                    "ListenerHandler.OnPurchaseFailure") ? 1 : 0;
            } else {
                LOGE("[FreeShop] LH.OnPurchaseFailure(4) NOT FOUND");
            }
        }
        if (!found_lh)
            LOGE("[FreeShop] ListenerHandler NOT FOUND in any assembly");
    }

    LOGI("[%s] HOOK DONE: %u installed", BUILD_ID, installed);
    if (il2cpp_thread_detach) il2cpp_thread_detach(thread);
    return nullptr;
}

// ═══════════════════════════════════════════════════════════
//  ParseMenuConfig() — đọc 1/2/7 fields từ HAMOD config
// ═══════════════════════════════════════════════════════════
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
        *speed = values[1] < 1.0f ? 1.0f : (values[1] > 20.0f ? 20.0f : values[1]);
    *field_count = count;
    return true;
}

// ═══════════════════════════════════════════════════════════
//  Doc_Config_Ngam() — đọc config HAMOD, active apply speed
// ═══════════════════════════════════════════════════════════
void* Doc_Config_Ngam(void*) {
    char root[768];
    const char* configured_root = getenv("MODMENU_LOG_DIR");
    const char* source_root = configured_root && *configured_root ? configured_root :
        "/data/data/menu.nextgen.launcher/files";
    const int root_length = snprintf(root, sizeof(root), "%s", source_root);
    if (root_length < 0 || static_cast<size_t>(root_length) >= sizeof(root)) {
        LOGE("CONFIG: MODMENU_LOG_DIR too long"); return nullptr;
    }
    size_t root_size = strlen(root);
    while (root_size > 1 && root[root_size - 1] == '/') root[--root_size] = '\0';
    const char* leaf = strrchr(root, '/'); leaf = leaf ? leaf + 1 : root;
    const bool root_is_mods = strcmp(leaf, "mods") == 0;

    // Package: com.xiaomo.lastextract
    const char* names[] = {
        "config_com.xiaomo.lastextract.txt",
        "config_COM.XIAOMO.LASTEXTRACT.txt"
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
    for (size_t i = 0; i < path_count; ++i) LOGI("CONFIG candidate[%zu]=%s", i, paths[i]);

    int last_status = -1, last_path = -1;
    bool last_shop = false; float last_speed = 1.0f;
    static float s_last_applied_speed = 1.0f;

    while (true) {
        FILE* file = nullptr; int selected = -1;
        for (size_t i = 0; i < path_count; ++i) {
            file = fopen(paths[i], "r");
            if (file) { selected = (int)i; break; }
        }
        if (!file) {
            if (last_status != 0)
                LOGE("CONFIG missing; previous values retained");
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
                LOGE("CONFIG invalid: path=%s", paths[selected]);
            last_status = 1; last_path = selected; usleep(500000); continue;
        }

        g_feature0.store(shop, std::memory_order_release);
        if (has_speed) {
            g_feature1.store(speed, std::memory_order_release);
            // Active apply: game chỉ gọi set_timeScale 1 lần lúc start → hook thụ động không đủ
            if (o_set_timeScale && speed != s_last_applied_speed) {
                o_set_timeScale(g_game_timeScale * speed, nullptr);
                s_last_applied_speed = speed;
                LOGI("[Speed] Applied: game=%.2f speed=x%.0f -> actual=%.2f",
                     g_game_timeScale, speed, g_game_timeScale * speed);
            }
        }
        g_config_loaded_once.store(true, std::memory_order_release);

        if (last_status != 2 || last_path != selected || last_shop != shop || last_speed != speed)
            LOGI("CONFIG applied: path=%s shop=%d speed=%.2f fields=%u",
                 paths[selected], shop, speed, field_count);
        last_status = 2; last_path = selected; last_shop = shop; last_speed = speed;
        usleep(500000);
    }
    return nullptr;
}

// ═══════════════════════════════════════════════════════════
//  Entry point
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
