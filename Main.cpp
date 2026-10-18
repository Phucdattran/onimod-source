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
#include <sys/mman.h>
#include <atomic>
#include <cmath>
#include <android/log.h>
#include "Includes/obfuscate.h"
#include <Dobby/dobby.h>

#define BUILD_ID "CTQ_AUTO"
#define LOG_TAG  "HAMOD_CTQ"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ==========================================
// BIẾN ĐIỀU KHIỂN TỪ MENU
// index 0 → toggle Free Shop
// index 1 → seekbar Speed (1–20)
// ==========================================
std::atomic<bool> g_feature0{false};
static std::atomic<bool> g_config_loaded_once{false};

// ==========================================
// IL2CPP API POINTERS
// ==========================================
void*       (*il2cpp_domain_get)()                                        = nullptr;
void*       (*il2cpp_thread_attach)(void*)                                = nullptr;
void        (*il2cpp_thread_detach)(void*)                                = nullptr;
void**      (*il2cpp_domain_get_assemblies)(void*, size_t*)               = nullptr;
void*       (*il2cpp_assembly_get_image)(void*)                           = nullptr;
void*       (*il2cpp_class_from_name)(void*, const char*, const char*)    = nullptr;
void*       (*il2cpp_class_get_methods)(void*, void**)                    = nullptr;
const char* (*il2cpp_method_get_name)(void*)                              = nullptr;
uint32_t    (*il2cpp_method_get_param_count)(void*)                       = nullptr;
void*       (*il2cpp_runtime_invoke)(void*, void*, void**, void**)        = nullptr;
void*       (*il2cpp_object_get_class)(void*)                             = nullptr;
void*       (*il2cpp_class_get_method_from_name)(void*, const char*, int) = nullptr;
void*       (*il2cpp_class_get_field_from_name)(void*, const char*)       = nullptr;
int32_t     (*il2cpp_field_get_offset)(void*)                             = nullptr;
size_t      (*il2cpp_image_get_class_count)(void*)                        = nullptr;
void*       (*il2cpp_image_get_class)(void*, size_t)                      = nullptr;
const char* (*il2cpp_class_get_name_raw)(void*)                           = nullptr;
void*       (*il2cpp_method_get_param)(void*, uint32_t)                   = nullptr;
int         (*il2cpp_type_get_type)(void*)                                = nullptr;
bool        (*il2cpp_type_is_byref)(void*)                                = nullptr;
void        (*il2cpp_format_exception)(const void*, char*, int)           = nullptr;

// ==========================================
// ELF IMAGE INFO
// ==========================================
struct LoadedIl2CppImage {
    uintptr_t        base;
    const ElfW(Phdr)* phdr;
    size_t           phnum;
    const char*      path;
};
static LoadedIl2CppImage g_il2cpp_image = {};
static void* g_il2cpp_handle = nullptr;

// ==========================================
// IMAGE VALIDATION HELPERS
// ==========================================
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

// ==========================================
// ELF SYMBOL RESOLVER (không dùng DobbySymbolResolver)
// ==========================================
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

// ==========================================
// isIl2cppLoaded — capture phdr info
// ==========================================
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

// ==========================================
// InitIl2cpp
// ==========================================
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
    LOAD_API(il2cpp_domain_get,                 true);
    LOAD_API(il2cpp_thread_attach,              true);
    LOAD_API(il2cpp_domain_get_assemblies,      true);
    LOAD_API(il2cpp_assembly_get_image,         true);
    LOAD_API(il2cpp_class_from_name,            true);
    LOAD_API(il2cpp_class_get_method_from_name, true);
    LOAD_API(il2cpp_object_get_class,           true);
    LOAD_API(il2cpp_runtime_invoke,             true);
    LOAD_API(il2cpp_thread_detach,              false);
    LOAD_API(il2cpp_class_get_methods,          false);
    LOAD_API(il2cpp_method_get_name,            false);
    LOAD_API(il2cpp_method_get_param_count,     false);
    LOAD_API(il2cpp_method_get_param,           false);
    LOAD_API(il2cpp_type_get_type,              false);
    LOAD_API(il2cpp_type_is_byref,              false);
    LOAD_API(il2cpp_format_exception,           false);
    LOAD_API(il2cpp_class_get_field_from_name,  false);
    LOAD_API(il2cpp_field_get_offset,           false);
    LOAD_API(il2cpp_image_get_class_count,      false);
    LOAD_API(il2cpp_image_get_class,            false);
    il2cpp_class_get_name_raw = reinterpret_cast<decltype(il2cpp_class_get_name_raw)>(
        resolve("il2cpp_class_get_name", false));
#undef LOAD_API

    LOGI("[%s] Resolve END: handle=%u mappedELF=%u missing=%u",
         BUILD_ID, from_handle, from_image, missing);
    return missing == 0;
}

// ==========================================
// Tim_Ham — tìm method address qua assemblies
// ==========================================
static uintptr_t Tim_Ham(const char* ns, const char* className,
                         const char* methodName, int argsCount) {
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

// ==========================================
// Tim_Ham_Send_String
// Tìm đúng overload: Send(System.String id, GrantSource source)
// Phân biệt với Send(List<String> ids, GrantSource source)
// bằng cách kiểm tra param[0] có type STRING (0x0E)
// ==========================================
static uintptr_t Tim_Ham_Send_String() {
    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies ||
        !il2cpp_assembly_get_image || !il2cpp_class_from_name ||
        !il2cpp_class_get_methods || !il2cpp_method_get_name ||
        !il2cpp_method_get_param_count || !il2cpp_method_get_param ||
        !il2cpp_type_get_type) return 0;
    void* domain = il2cpp_domain_get();
    if (!domain) return 0;
    size_t size = 0;
    void** assemblies = il2cpp_domain_get_assemblies(domain, &size);
    if (!assemblies) return 0;
    for (size_t i = 0; i < size; ++i) {
        void* image = il2cpp_assembly_get_image(assemblies[i]);
        if (!image) continue;
        void* klass = il2cpp_class_from_name(image, OBFUSCATE(""), OBFUSCATE("PurchaseManager"));
        if (!klass) continue;
        void* iter = nullptr;
        while (void* method = il2cpp_class_get_methods(klass, &iter)) {
            const char* name = il2cpp_method_get_name(method);
            if (!name || strcmp(name, OBFUSCATE("Send")) != 0) continue;
            if (il2cpp_method_get_param_count(method) != 2) continue;
            // param[0] phải là System.String (IL2CPP_TYPE_STRING = 0x0E)
            void* p0 = il2cpp_method_get_param(method, 0);
            if (!p0 || il2cpp_type_get_type(p0) != 0x0E) continue;
            uintptr_t addr = (uintptr_t)*(void**)method;
            if (addr) {
                LOGI("[%s] Send(String,GrantSource): 0x%" PRIxPTR, BUILD_ID, addr);
                return addr;
            }
        }
    }
    LOGE("[%s] Send(String,GrantSource): NOT FOUND", BUILD_ID);
    return 0;
}

// ==========================================
// DoHook — validate địa chỉ trước khi hook
// ==========================================
static bool DoHook(void* fn, void* hook, void** orig, const char* tag) {
    if (!hook || !orig || !ImageContains(reinterpret_cast<uintptr_t>(fn), 1, PF_X)) {
        LOGE("Hook SKIP (invalid addr): %s @ %p", tag, fn);
        return false;
    }
    const int result = DobbyHook(fn, hook, orig);
    const bool ok = result == 0 && *orig != nullptr;
    LOGI("Hook %s: %s @ %p", ok ? "OK" : "FAIL", tag, fn);
    return ok;
}

// ==========================================
// oni_patch_at — patch bằng địa chỉ tuyệt đối
// ==========================================
static void oni_patch_at(uintptr_t addr, uint32_t ins) {
    if (!ImageContains(addr, 4, PF_X)) {
        LOGE("oni_patch_at: invalid addr 0x%" PRIxPTR, addr);
        return;
    }
    uintptr_t page_start = addr & ~0xFFF;
    mprotect((void*)page_start, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
    *(uint32_t*)addr = ins;
    __builtin___clear_cache((char*)addr, (char*)addr + 4);
}

// ==========================================
// ĐỊA CHỈ CÁC HÀM (tìm runtime, không hardcode)
// ==========================================
static uintptr_t g_addr_BuyProduct    = 0;  // IAPManager.BuyProductByID
static uintptr_t g_addr_GetInstance   = 0;  // PurchaseManager.get_Instance
static uintptr_t g_addr_Send          = 0;  // PurchaseManager.Send(String, GrantSource)
static uintptr_t g_addr_SetProgress   = 0;  // PurchaseManager.SetProgress(bool)
static uintptr_t g_addr_HandleFail    = 0;  // PurchaseManager.HandlePurchaseFailure
static uintptr_t g_addr_ReportGrant   = 0;  // PurchaseManager.ReportUnauthorizedGrant
static uintptr_t g_addr_RefuseUnlock  = 0;  // PurchaseManager.RefuseUnlock
static uintptr_t g_addr_AntiTrigger   = 0;  // AntiCheatData.TriggerCheatCode
static uintptr_t g_addr_AntiAutoban   = 0;  // AntiCheatData.CheckAutoBan
static uintptr_t g_addr_HasAnyCheat   = 0;  // AntiCheatData.HasAnyCheat

static bool ResolveAllMethods() {
    g_addr_BuyProduct   = Tim_Ham(OBFUSCATE("com.F4A.MobileThird"), OBFUSCATE("IAPManager"),
                                  OBFUSCATE("BuyProductByID"), 1);
    g_addr_GetInstance  = Tim_Ham(OBFUSCATE(""), OBFUSCATE("PurchaseManager"),
                                  OBFUSCATE("get_Instance"), 0);
    g_addr_Send         = Tim_Ham_Send_String();
    g_addr_SetProgress  = Tim_Ham(OBFUSCATE(""), OBFUSCATE("PurchaseManager"),
                                  OBFUSCATE("SetProgress"), 1);
    g_addr_HandleFail   = Tim_Ham(OBFUSCATE(""), OBFUSCATE("PurchaseManager"),
                                  OBFUSCATE("HandlePurchaseFailure"), 2);
    g_addr_ReportGrant  = Tim_Ham(OBFUSCATE(""), OBFUSCATE("PurchaseManager"),
                                  OBFUSCATE("ReportUnauthorizedGrant"), 1);
    g_addr_RefuseUnlock = Tim_Ham(OBFUSCATE(""), OBFUSCATE("PurchaseManager"),
                                  OBFUSCATE("RefuseUnlock"), 1);
    g_addr_AntiTrigger  = Tim_Ham(OBFUSCATE(""), OBFUSCATE("AntiCheatData"),
                                  OBFUSCATE("TriggerCheatCode"), 1);
    g_addr_AntiAutoban  = Tim_Ham(OBFUSCATE(""), OBFUSCATE("AntiCheatData"),
                                  OBFUSCATE("CheckAutoBan"), 1);
    g_addr_HasAnyCheat  = Tim_Ham(OBFUSCATE(""), OBFUSCATE("AntiCheatData"),
                                  OBFUSCATE("HasAnyCheat"), 0);

    LOGI("[%s] RESOLVE RESULTS:", BUILD_ID);
    LOGI("  BuyProduct:   0x%" PRIxPTR, g_addr_BuyProduct);
    LOGI("  GetInstance:  0x%" PRIxPTR, g_addr_GetInstance);
    LOGI("  Send:         0x%" PRIxPTR, g_addr_Send);
    LOGI("  SetProgress:  0x%" PRIxPTR, g_addr_SetProgress);
    LOGI("  HandleFail:   0x%" PRIxPTR, g_addr_HandleFail);
    LOGI("  ReportGrant:  0x%" PRIxPTR, g_addr_ReportGrant);
    LOGI("  RefuseUnlock: 0x%" PRIxPTR, g_addr_RefuseUnlock);
    LOGI("  AntiTrigger:  0x%" PRIxPTR, g_addr_AntiTrigger);
    LOGI("  AntiAutoban:  0x%" PRIxPTR, g_addr_AntiAutoban);
    LOGI("  HasAnyCheat:  0x%" PRIxPTR, g_addr_HasAnyCheat);

    return g_addr_BuyProduct && g_addr_GetInstance && g_addr_Send &&
           g_addr_SetProgress && g_addr_HandleFail && g_addr_HasAnyCheat;
}

// ==========================================
// HOOK HANDLERS
// ==========================================

// Lớp 1: Mua trực tiếp
int (*o_BuyProductByID)(void*, void*, void*) = nullptr;
int h_BuyProductByID(void* _this, void* productId, void* methodInfo) {
    if (g_feature0.load(std::memory_order_acquire) && productId != nullptr) {
        if (g_addr_GetInstance) {
            void* pm_inst = ((void* (*)())(g_addr_GetInstance))();
            if (pm_inst && g_addr_Send && g_addr_SetProgress) {
                ((bool (*)(void*, void*, int, void*))(g_addr_Send))(pm_inst, productId, 1, nullptr);
                ((void (*)(void*, bool, void*))(g_addr_SetProgress))(pm_inst, false, nullptr);
            }
        }
        return 4;
    }
    return o_BuyProductByID ? o_BuyProductByID(_this, productId, methodInfo) : 0;
}

// Lớp 2: Hủy thanh toán -> Nhận đồ
void (*o_HandlePurchaseFailure)(void*, void*, int, void*) = nullptr;
void h_HandlePurchaseFailure(void* _this, void* id, int reason, void* methodInfo) {
    if (g_feature0.load(std::memory_order_acquire) && id != nullptr) {
        if (g_addr_Send && g_addr_SetProgress) {
            ((bool (*)(void*, void*, int, void*))(g_addr_Send))(_this, id, 1, nullptr);
            ((void (*)(void*, bool, void*))(g_addr_SetProgress))(_this, false, nullptr);
        }
        return;
    }
    if (o_HandlePurchaseFailure) o_HandlePurchaseFailure(_this, id, reason, methodInfo);
}

// Bịt mắt anti-cheat
bool (*o_HasAnyCheat)(void*, void*) = nullptr;
bool h_HasAnyCheat(void* _this, void* methodInfo) {
    return false;
}

//==========================================
// ParseMenuConfig — đọc field đầu tiên từ HAMOD (1 hoặc 7 fields)
// ==========================================
static bool ParseMenuConfig(const char* text, bool* shop, unsigned* field_count) {
    if (!text || !shop || !field_count) return false;
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
    if ((count != 1 && count != 7) ||
        (values[0] != 0.0f && values[0] != 1.0f)) return false;
    *shop        = values[0] == 1.0f;
    *field_count = count;
    return true;
}

// ==========================================
// Doc_Config_Ngam — đọc config HAMOD
// Package: co.imba.namquocsonha
// ==========================================
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
    const char* names[] = {
        "config_co.imba.namquocsonha.txt",
        "config_CO.IMBA.NAMQUOCSONHA.txt"
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
    bool last_shop = false;
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
        bool shop = false; unsigned field_count = 0;
        if (!read_ok || memchr(text, '\0', length) ||
            !ParseMenuConfig(text, &shop, &field_count)) {
            if (last_status != 1 || last_path != selected)
                LOGE("CONFIG invalid: path=%s expected 1/7 fields", paths[selected]);
            last_status = 1; last_path = selected; usleep(500000); continue;
        }
        g_config_loaded_once.store(true, std::memory_order_release);
        g_feature0.store(shop, std::memory_order_release);
        if (last_status != 2 || last_path != selected || last_shop != shop)
            LOGI("CONFIG applied: path=%s shop=%d fields=%u",
                 paths[selected], shop, field_count);
        last_status = 2; last_path = selected; last_shop = shop;
        usleep(500000);
    }
    return nullptr;
}

// ==========================================
// WORKER THREAD
// ==========================================
void* h_t(void*) {
    for (int i = 0; i < 120; ++i) {
        if (isIl2cppLoaded()) break;
        sleep(1);
    }
    if (!InitIl2cpp()) {
        LOGE("[%s] InitIl2cpp FAILED — aborting", BUILD_ID);
        return nullptr;
    }

    void* domain = il2cpp_domain_get();
    void* thread = (domain && il2cpp_thread_attach) ? il2cpp_thread_attach(domain) : nullptr;

    // ==========================================
    // PHA 1 — NGAY SAU InitIl2cpp (không chờ stable)
    // Anti-cheat phải bị bịt sớm nhất có thể,
    // trước khi game kịp gọi CheckAutoBan/TriggerCheatCode
    // ==========================================
    g_addr_AntiTrigger  = Tim_Ham(OBFUSCATE(""), OBFUSCATE("AntiCheatData"),
                                  OBFUSCATE("TriggerCheatCode"), 1);
    g_addr_AntiAutoban  = Tim_Ham(OBFUSCATE(""), OBFUSCATE("AntiCheatData"),
                                  OBFUSCATE("CheckAutoBan"), 1);
    g_addr_ReportGrant  = Tim_Ham(OBFUSCATE(""), OBFUSCATE("PurchaseManager"),
                                  OBFUSCATE("ReportUnauthorizedGrant"), 1);
    g_addr_RefuseUnlock = Tim_Ham(OBFUSCATE(""), OBFUSCATE("PurchaseManager"),
                                  OBFUSCATE("RefuseUnlock"), 1);
    g_addr_HasAnyCheat  = Tim_Ham(OBFUSCATE(""), OBFUSCATE("AntiCheatData"),
                                  OBFUSCATE("HasAnyCheat"), 0);

    LOGI("[%s] PHASE1 anti-cheat:", BUILD_ID);
    LOGI("  AntiTrigger:  0x%" PRIxPTR, g_addr_AntiTrigger);
    LOGI("  AntiAutoban:  0x%" PRIxPTR, g_addr_AntiAutoban);
    LOGI("  ReportGrant:  0x%" PRIxPTR, g_addr_ReportGrant);
    LOGI("  RefuseUnlock: 0x%" PRIxPTR, g_addr_RefuseUnlock);
    LOGI("  HasAnyCheat:  0x%" PRIxPTR, g_addr_HasAnyCheat);

    if (g_addr_AntiTrigger)  oni_patch_at(g_addr_AntiTrigger,  0xD65F03C0);
    if (g_addr_AntiAutoban)  oni_patch_at(g_addr_AntiAutoban,  0xD65F03C0);
    if (g_addr_ReportGrant)  oni_patch_at(g_addr_ReportGrant,  0xD65F03C0);
    if (g_addr_RefuseUnlock) oni_patch_at(g_addr_RefuseUnlock, 0xD65F03C0);

    unsigned installed = 0;
    if (g_addr_HasAnyCheat)
        installed += DoHook((void*)g_addr_HasAnyCheat,
                            (void*)h_HasAnyCheat, (void**)&o_HasAnyCheat,
                            "HasAnyCheat");
    LOGI("[%s] PHASE1 done", BUILD_ID);

    // ==========================================
    // PHA 2 — Chờ assemblies stable rồi hook Free Shop
    // ==========================================
    size_t last = 0; int stable = 0;
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

    // Tìm phần còn lại (Free Shop)
    g_addr_BuyProduct   = Tim_Ham(OBFUSCATE("com.F4A.MobileThird"), OBFUSCATE("IAPManager"),
                                  OBFUSCATE("BuyProductByID"), 1);
    g_addr_GetInstance  = Tim_Ham(OBFUSCATE(""), OBFUSCATE("PurchaseManager"),
                                  OBFUSCATE("get_Instance"), 0);
    g_addr_Send         = Tim_Ham_Send_String();
    g_addr_SetProgress  = Tim_Ham(OBFUSCATE(""), OBFUSCATE("PurchaseManager"),
                                  OBFUSCATE("SetProgress"), 1);
    g_addr_HandleFail   = Tim_Ham(OBFUSCATE(""), OBFUSCATE("PurchaseManager"),
                                  OBFUSCATE("HandlePurchaseFailure"), 2);

    LOGI("[%s] PHASE2 free shop:", BUILD_ID);
    LOGI("  BuyProduct:  0x%" PRIxPTR, g_addr_BuyProduct);
    LOGI("  GetInstance: 0x%" PRIxPTR, g_addr_GetInstance);
    LOGI("  Send:        0x%" PRIxPTR, g_addr_Send);
    LOGI("  SetProgress: 0x%" PRIxPTR, g_addr_SetProgress);
    LOGI("  HandleFail:  0x%" PRIxPTR, g_addr_HandleFail);

    if (g_addr_BuyProduct)
        installed += DoHook((void*)g_addr_BuyProduct,
                            (void*)h_BuyProductByID, (void**)&o_BuyProductByID,
                            "BuyProductByID");
    if (g_addr_HandleFail)
        installed += DoHook((void*)g_addr_HandleFail,
                            (void*)h_HandlePurchaseFailure, (void**)&o_HandlePurchaseFailure,
                            "HandlePurchaseFailure");

    LOGI("[%s] HOOK DONE: %u installed", BUILD_ID, installed);
    if (il2cpp_thread_detach) il2cpp_thread_detach(thread);
    return nullptr;
}

// ==========================================
// ENTRY POINT
// ==========================================
__attribute__((constructor))
void l_m() {
    LOGI("[%s] MOD injected!", BUILD_ID);
    pthread_t t1, t2;
    int rc = pthread_create(&t1, nullptr, h_t, nullptr);
    if (rc == 0) pthread_detach(t1);
    else LOGE("Worker create failed: %d", rc);
    rc = pthread_create(&t2, nullptr, Doc_Config_Ngam, nullptr);
    if (rc == 0) pthread_detach(t2);
    else LOGE("Config thread create failed: %d", rc);
}
