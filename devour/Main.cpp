// ==============================================================
//  Main.cpp - com.multicastgames.devour v2.1.2
//  Features: Speed + Free Shop
//  Theo bản mẫu LGL v2.2 — v8: spoof CurrencyModel.Get + TrySpend
// ==============================================================
#include <pthread.h>
#include <jni.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <link.h>
#include <android/log.h>

#include "Includes/obfuscate.h"
#include "Includes/Logger.h"
#include "Includes/Utils.h"
#include "Menu/Setup.h"
#include <Dobby/dobby.h>
#include "MenuStrings.h"
#include "Includes/EngineVIP.h"

#define LOG_TAG   "DEVOUR_MOD"
#define GAME_NAME "Devour"
#define GAME_PACKAGE "com.multicastgames.devour"
#include "Includes/LGLTrace.h"

// ═══════════════════════════════════════════════════════════
//  1. IL2CPP API (KHÔNG dùng static — LGLTrace.h cần extern)
// ═══════════════════════════════════════════════════════════
uintptr_t il2cpp_base = 0;

void*       (*il2cpp_domain_get)()                                         = nullptr;
void*       (*il2cpp_thread_attach)(void*)                                 = nullptr;
void**      (*il2cpp_domain_get_assemblies)(void*, size_t*)                = nullptr;
void*       (*il2cpp_assembly_get_image)(void*)                            = nullptr;
size_t      (*il2cpp_image_get_class_count)(void*)                         = nullptr;
void*       (*il2cpp_image_get_class)(void*, size_t)                       = nullptr;
void*       (*il2cpp_class_from_name)(void*, const char*, const char*)     = nullptr;
const char* (*il2cpp_class_get_name)(void*)                                = nullptr;
void*       (*il2cpp_class_get_methods)(void*, void**)                     = nullptr;
const char* (*il2cpp_method_get_name)(void*)                               = nullptr;
uint32_t    (*il2cpp_method_get_param_count)(void*)                        = nullptr;
void*       (*il2cpp_class_get_field_from_name)(void*, const char*)        = nullptr;
size_t      (*il2cpp_field_get_offset)(void*)                              = nullptr;
void*       (*il2cpp_string_new)(const char*)                              = nullptr;
void*       (*il2cpp_object_new)(void*)                                    = nullptr;

// ═══════════════════════════════════════════════════════════
//  2. ĐỌC SYMBOL libil2cpp.so TRONG BỘ NHỚ — KHÔNG CẦN SỬA
// ═══════════════════════════════════════════════════════════
static const ElfW(Phdr)* g_il2cpp_phdr  = nullptr;
static ElfW(Half)        g_il2cpp_phnum = 0;

static void* ElfSym(const char* name) {
    if (!il2cpp_base || !g_il2cpp_phdr) return nullptr;
    uintptr_t base = il2cpp_base;
    auto fix = [base](uintptr_t p) { return p < base ? base + p : p; };

    const ElfW(Dyn)* dyn = nullptr;
    for (int i = 0; i < g_il2cpp_phnum; i++)
        if (g_il2cpp_phdr[i].p_type == PT_DYNAMIC)
            dyn = (const ElfW(Dyn)*)(base + g_il2cpp_phdr[i].p_vaddr);
    if (!dyn) return nullptr;

    const ElfW(Sym)* symtab = nullptr; const char* strtab = nullptr;
    const uint32_t* gnu = nullptr; const uint32_t* sysv = nullptr;
    for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; ++d) {
        switch (d->d_tag) {
            case DT_SYMTAB:   symtab = (const ElfW(Sym)*) fix(d->d_un.d_ptr); break;
            case DT_STRTAB:   strtab = (const char*)      fix(d->d_un.d_ptr); break;
            case DT_GNU_HASH: gnu    = (const uint32_t*)  fix(d->d_un.d_ptr); break;
            case DT_HASH:     sysv   = (const uint32_t*)  fix(d->d_un.d_ptr); break;
        }
    }
    if (!symtab || !strtab) return nullptr;

    size_t nsyms = 0;
    if (sysv) {
        nsyms = sysv[1];
    } else if (gnu) {
        uint32_t nbuckets = gnu[0], symoff = gnu[1], bloom = gnu[2];
        const uint32_t* buckets = gnu + 4 + bloom * (sizeof(ElfW(Addr)) / 4);
        const uint32_t* chain = buckets + nbuckets;
        uint32_t last = 0;
        for (uint32_t b = 0; b < nbuckets; b++) if (buckets[b] > last) last = buckets[b];
        if (last >= symoff) while (!(chain[last - symoff] & 1)) last++;
        nsyms = last + 1;
    }
    for (size_t i = 0; i < nsyms; i++) {
        const ElfW(Sym)& s = symtab[i];
        if (!s.st_name || s.st_shndx == SHN_UNDEF || !s.st_value) continue;
        if (strcmp(strtab + s.st_name, name) == 0) return (void*)(base + s.st_value);
    }
    return nullptr;
}

bool InitIL2CppAPI() {
    #define LOAD(sym) sym = (decltype(sym)) ElfSym(#sym)
    LOAD(il2cpp_domain_get);
    LOAD(il2cpp_thread_attach);
    LOAD(il2cpp_domain_get_assemblies);
    LOAD(il2cpp_assembly_get_image);
    LOAD(il2cpp_image_get_class_count);
    LOAD(il2cpp_image_get_class);
    LOAD(il2cpp_class_from_name);
    LOAD(il2cpp_class_get_name);
    LOAD(il2cpp_class_get_methods);
    LOAD(il2cpp_method_get_name);
    LOAD(il2cpp_method_get_param_count);
    LOAD(il2cpp_class_get_field_from_name);
    LOAD(il2cpp_field_get_offset);
    LOAD(il2cpp_string_new);
    LOAD(il2cpp_object_new);
    #undef LOAD
    bool ok = il2cpp_domain_get && il2cpp_domain_get_assemblies && il2cpp_assembly_get_image &&
              il2cpp_class_from_name && il2cpp_class_get_methods && il2cpp_method_get_name;
    LOGI("InitIL2CppAPI: %s", ok ? "OK" : "FAIL");
    return ok;
}

int dl_cb(struct dl_phdr_info* info, size_t, void* data) {
    if (info->dlpi_name && strstr(info->dlpi_name, "libil2cpp.so")) {
        il2cpp_base    = info->dlpi_addr;
        g_il2cpp_phdr  = info->dlpi_phdr;
        g_il2cpp_phnum = info->dlpi_phnum;
        *(bool*)data = true;
        return 1;
    }
    return 0;
}
bool IsIL2CppLoaded() {
    bool found = false;
    dl_iterate_phdr(dl_cb, &found);
    return found;
}

// ═══════════════════════════════════════════════════════════
//  3. HELPERS — KHÔNG CẦN SỬA
// ═══════════════════════════════════════════════════════════
static inline void* MethodFnPtr(void* mi) { return mi ? *(void**)mi : nullptr; }

void* FindClass(const char* ns, const char* name) {
    size_t n = 0;
    void** list = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &n);
    for (size_t i = 0; i < n; i++) {
        void* img = il2cpp_assembly_get_image(list[i]);
        if (!img) continue;
        if (void* k = il2cpp_class_from_name(img, ns, name)) return k;
    }
    return nullptr;
}

void* FindClassAnywhere(const char* name) {
    size_t n = 0;
    void** list = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &n);
    for (size_t i = 0; i < n; i++) {
        void* img = il2cpp_assembly_get_image(list[i]);
        if (!img) continue;
        if (il2cpp_image_get_class_count && il2cpp_image_get_class && il2cpp_class_get_name) {
            size_t nc = il2cpp_image_get_class_count(img);
            for (size_t j = 0; j < nc; j++) {
                void* k = il2cpp_image_get_class(img, j);
                const char* kn = k ? il2cpp_class_get_name(k) : nullptr;
                if (kn && strcmp(kn, name) == 0) return k;
            }
        } else if (void* k = il2cpp_class_from_name(img, "", name)) return k;
    }
    return nullptr;
}

void* FindMethod(void* klass, const char* name, int nParams) {
    if (!klass) return nullptr;
    void* iter = nullptr;
    while (void* m = il2cpp_class_get_methods(klass, &iter)) {
        const char* n = il2cpp_method_get_name(m);
        if (!n || strcmp(n, name) != 0) continue;
        if (nParams >= 0 && il2cpp_method_get_param_count &&
            (int)il2cpp_method_get_param_count(m) != nParams) continue;
        return m;
    }
    return nullptr;
}

static void* GetClass(const char* ns, const char* cls) {
    return (ns && *ns) ? FindClass(ns, cls) : FindClassAnywhere(cls);
}

void* MethodAddr(const char* ns, const char* cls, const char* method, int nParams) {
    void* k = GetClass(ns, cls);
    if (!k) { LOGE("NOT FOUND class: %s.%s", ns, cls); return nullptr; }
    void* m = FindMethod(k, method, nParams);
    if (!m) { LOGE("NOT FOUND: %s.%s(%d)", cls, method, nParams); return nullptr; }
    return MethodFnPtr(m);
}

int FieldOffset(const char* ns, const char* cls, const char* field) {
    void* k = GetClass(ns, cls);
    void* f = (k && il2cpp_class_get_field_from_name) ? il2cpp_class_get_field_from_name(k, field) : nullptr;
    if (!f || !il2cpp_field_get_offset) { LOGE("NOT FOUND field: %s.%s", cls, field); return -1; }
    int off = (int) il2cpp_field_get_offset(f);
    LOGI("Field %s.%s @ 0x%X", cls, field, off);
    return off;
}

static bool DoHook(void* addr, void* hook, void** orig, const char* name) {
    if (!addr) { LOGE("Hook SKIP (NULL): %s", name); return false; }
    int r = DobbyHook(addr, hook, orig);
    if (r == 0) { LOGI("Hook OK: %s @ %p", name, addr); return true; }
    LOGE("Hook FAIL(%d): %s @ %p", r, name, addr);
    return false;
}

#define HOOK(ns, cls, method, nParams, fn) \
    DoHook(MethodAddr(ns, cls, method, nParams), (void*)h_##fn, (void**)&o_##fn, cls "." method)

template <typename T> static inline T& Field(void* obj, int off) {
    return *(T*)((uintptr_t)obj + off);
}

// ═══════════════════════════════════════════════════════════
//  4. TRẠNG THÁI
// ═══════════════════════════════════════════════════════════
// Speed
static float g_speed = 1.0f;
static float g_gameScale = 1.0f;

// Free Shop
static bool g_free_shop = false;
static bool g_unlock_shop = false;
static void* g_currency_mdl = nullptr;

// ═══════════════════════════════════════════════════════════
//  5. SPEED — KHÔNG CẦN SỬA
// ═══════════════════════════════════════════════════════════
void  (*o_setTS)(float, void*) = nullptr;
float (*o_getTS)(void*)        = nullptr;

void h_setTS(float v, void* mi) { T_COUNT(setTS); g_gameScale = v; if (o_setTS) o_setTS(v * g_speed, mi); }
float h_getTS(void* mi) {
    T_COUNT(getTS);
    if (g_speed != 1.0f) return g_gameScale;
    return o_getTS ? o_getTS(mi) : g_gameScale;
}
static void ApplySpeed() {
    if (!o_setTS) { LOGE("Speed KHONG chay: chua hook duoc Time.set_timeScale"); return; }
    o_setTS(g_gameScale * g_speed, nullptr);
    LOGI("Speed x%.0f: game dat %.2f -> thuc te %.2f", g_speed, g_gameScale, g_gameScale * g_speed);
}

// ═══════════════════════════════════════════════════════════
//  6. HOOK FREE SHOP
// ═══════════════════════════════════════════════════════════

// get_CanPurchase -> capture currencyModel + true
bool (*o_GetCanPurchase)(void*, void*) = nullptr;
bool h_GetCanPurchase(void* self, void* mi) {
    T_COUNT(GetCanPurchase);
    if (!g_currency_mdl && self) {
        void* cm = Field<void*>(self, 0x60);  // currencyModel @ 0x60
        if (cm) {
            g_currency_mdl = cm;
            LOGI("FreeShop: currencyModel=%p", cm);
        }
    }
    if (g_unlock_shop || g_free_shop) return true;
    return o_GetCanPurchase ? o_GetCanPurchase(self, mi) : false;
}

// get_HasClaimed -> false
bool (*o_GetHasClaimed)(void*, void*) = nullptr;
bool h_GetHasClaimed(void* self, void* mi) {
    T_COUNT(GetHasClaimed);
    if (g_unlock_shop || g_free_shop) return false;
    return o_GetHasClaimed ? o_GetHasClaimed(self, mi) : false;
}

// IAPManager.IsAvailableToPurchase -> true
bool (*o_IsAvailToPurch)(void*, int, void*) = nullptr;
bool h_IsAvailToPurch(void* self, int key, void* mi) {
    T_COUNT(IsAvailToPurch);
    if (g_unlock_shop || g_free_shop) return true;
    return o_IsAvailToPurch ? o_IsAvailToPurch(self, key, mi) : false;
}

// CurrencyModel.Get(string key) -> spoof 999999 khi free_shop ON
// -> UI hien 999999, HasEnough tra true, bypass check so du
double (*o_CurrencyGet)(void*, void*, void*) = nullptr;
static char g_seen_keys[32][64];
static int  g_seen_count = 0;
double h_CurrencyGet(void* self, void* key, void* mi) {
    T_COUNT(CurrencyGet);
    if (!g_currency_mdl && self) {
        g_currency_mdl = self;
        LOGI("FreeShop: currencyModel captured=%p", self);
    }
    // Log tat ca currency keys de biet ten dung
    if (self && key && g_seen_count < 32) {
        int len = *(int*)((uintptr_t)key + 0x10);
        if (len > 0 && len < 63) {
            char buf[64] = {};
            const uint16_t* chars = (const uint16_t*)((uintptr_t)key + 0x14);
            for (int i = 0; i < len && i < 63; i++) buf[i] = (char)chars[i];
            bool seen = false;
            for (int i = 0; i < g_seen_count; i++)
                if (strcmp(g_seen_keys[i], buf) == 0) { seen = true; break; }
            if (!seen) {
                strncpy(g_seen_keys[g_seen_count++], buf, 63);
                LOGI("CurrencyKey #%d = '%s'", g_seen_count, buf);
            }
        }
    }
    // SPOOF: tra ve 999999 cho tat ca currency khi free_shop bat
    if (g_free_shop || g_unlock_shop) return 999999.0;
    return o_CurrencyGet ? o_CurrencyGet(self, key, mi) : 0.0;
}

// CurrencyModel.Add -> chi log key (de biet ten key gems khi game tu goi)
void (*o_CurrencyAdd)(void*, void*, double, void*) = nullptr;
void h_CurrencyAdd(void* self, void* key, double amount, void* mi) {
    T_COUNT(CurrencyAdd);
    if (key) {
        int len = *(int*)((uintptr_t)key + 0x10);
        if (len > 0 && len < 64) {
            char buf[64] = {};
            const uint16_t* chars = (const uint16_t*)((uintptr_t)key + 0x14);
            for (int i = 0; i < len && i < 63; i++) buf[i] = (char)chars[i];
            LOGI("CurrencyAdd: key='%s' amount=%.0f", buf, amount);
        }
    }
    if (o_CurrencyAdd) o_CurrencyAdd(self, key, amount, mi);
}

// CurrencyModel.TrySpend -> khong tru tien khi free_shop bat
bool (*o_TrySpend)(void*, void*, double, void*) = nullptr;
bool h_TrySpend(void* self, void* key, double amount, void* mi) {
    T_COUNT(TrySpend);
    if (g_free_shop || g_unlock_shop) {
        LOGI("TrySpend bypassed (free shop)");
        return true;  // Thanh cong nhung KHONG tru so du
    }
    return o_TrySpend ? o_TrySpend(self, key, amount, mi) : false;
}

// PurchaseProcessingService.StartPurchase -> bypass Google Play (khong mo popup)
void (*o_StartPurchase)(void*, int, void*) = nullptr;
void h_StartPurchase(void* self, int productKey, void* mi) {
    T_COUNT(StartPurchase);
    if (!g_free_shop) {
        if (o_StartPurchase) o_StartPurchase(self, productKey, mi);
        return;
    }
    // Bypass: khong goi original -> Google Play popup KHONG hien
    // Balance da duoc spoof 999999 boi h_CurrencyGet
    LOGI("FreeShop: bypass IAP productKey=%d (spoof balance ON)", productKey);
}

// ═══════════════════════════════════════════════════════════
//  7. GẮN HOOK
// ═══════════════════════════════════════════════════════════
static void ResolveAll() {
    LOGI("=== Resolve START ===");

    // Speed
    T_HOOK("UnityEngine", "Time", "get_timeScale", 0, getTS);
    T_HOOK("UnityEngine", "Time", "set_timeScale", 1, setTS);
    if (o_getTS) g_gameScale = o_getTS(nullptr);

    // Free Shop - mo khoa UI
    T_HOOK("Codebase.Shop",               "ShopLotModel",              "get_CanPurchase",         0, GetCanPurchase);
    T_HOOK("Codebase.Shop",               "ShopLotModel",              "get_HasClaimed",           0, GetHasClaimed);
    T_HOOK("Codebase.In_apps",            "IAPManager",                "IsAvailableToPurchase",    1, IsAvailToPurch);

    // Free Shop - bypass thanh toan
    T_HOOK("Codebase.Services.Purchases", "PurchaseProcessingService", "StartPurchase",            1, StartPurchase);

    // CurrencyModel - spoof balance + bypass deduction
    T_HOOK("Codebase.Economy", "CurrencyModel", "Get",      1, CurrencyGet);
    T_HOOK("Codebase.Economy", "CurrencyModel", "Add",      2, CurrencyAdd);
    T_HOOK("Codebase.Economy", "CurrencyModel", "TrySpend", 2, TrySpend);

    TraceReport();
    LOGI("=== Resolve DONE ===");
}

// ═══════════════════════════════════════════════════════════
//  8. MENU
// ═══════════════════════════════════════════════════════════
jobjectArray GetFeatureList(JNIEnv* env, jobject) {
    const char* features[] = {
        OBFUSCATE("Category_" GAME_NAME " - Toc Do"),
        OBFUSCATE("0_SeekBar_Toc Do (x)_1_10"),
        OBFUSCATE("Category_Free Shop"),
        OBFUSCATE("1_Toggle_Mo Khoa Shop (hien nut Mua)"),
        OBFUSCATE("2_Toggle_Free Shop (Spoof So Du + Bypass IAP)"),
    };
    int total = sizeof(features) / sizeof(features[0]);
    jobjectArray ret = env->NewObjectArray(
        total, env->FindClass("java/lang/String"), env->NewStringUTF(""));
    for (int i = 0; i < total; i++) {
        jstring s = env->NewStringUTF(features[i]);
        env->SetObjectArrayElement(ret, i, s);
        env->DeleteLocalRef(s);
    }
    return ret;
}

void Changes(JNIEnv* env, jclass, jobject,
             jint featNum, jstring featName, jint value, jboolean boolean, jstring str) {
    T_STEP("bam tinh nang so %d (value=%d, bat=%d)", featNum, value, boolean);
    LOGI("Menu: tinh nang %d -> value=%d bat=%d", featNum, value, boolean);
    switch (featNum) {
        case 0: // Speed
            g_speed = (value < 1) ? 1.0f : (float) value;
            ApplySpeed();
            break;
        case 1: // Mo khoa shop
            g_unlock_shop = boolean;
            LOGI("Mo khoa shop: %d", g_unlock_shop);
            break;
        case 2: // Free shop: spoof balance + bypass IAP
            g_free_shop = boolean;
            LOGI("Free shop: %d (spoof Get=999999 + TrySpend bypass)", g_free_shop);
            break;
    }
}

// ═══════════════════════════════════════════════════════════
//  9. KHỞI TẠO — KHÔNG CẦN SỬA
// ═══════════════════════════════════════════════════════════
static volatile bool g_il2cpp_ready = false;
static int (*o_il2cpp_init)(const char*) = nullptr;
static int h_il2cpp_init(const char* domain_name) {
    int r = o_il2cpp_init ? o_il2cpp_init(domain_name) : 0;
    g_il2cpp_ready = true;
    return r;
}

void* h_t(void*) {
    T_STEP("cho libil2cpp.so");
    while (!IsIL2CppLoaded()) usleep(100000);
    LOGI("libil2cpp.so base: 0x%" PRIxPTR, il2cpp_base);

    if (!InitIL2CppAPI()) { LOGE("InitIL2CppAPI FAIL - dung"); return nullptr; }

    T_STEP("hook il2cpp_init");
    void* initAddr = ElfSym("il2cpp_init");
    bool hooked = initAddr && DobbyHook(initAddr, (void*)h_il2cpp_init, (void**)&o_il2cpp_init) == 0;
    LOGI("Hook il2cpp_init: %s", hooked ? "OK" : "FAIL (dung cho theo thoi gian)");

    T_STEP("cho il2cpp_init");
    for (int i = 0; i < 300 && !g_il2cpp_ready; i++) usleep(100000);
    if (g_il2cpp_ready) LOGI("il2cpp_init xong");
    else                LOGE("Khong thay il2cpp_init -> cho them 5s");
    for (int i = 0; i < 20; i++) usleep(250000);

    T_STEP("attach + cho assemblies");
    void* domain = il2cpp_domain_get();
    if (domain && il2cpp_thread_attach) il2cpp_thread_attach(domain);
    { size_t last = 0; int stable = 0;
      while (stable < 5) {
          size_t n = 0;
          il2cpp_domain_get_assemblies(domain, &n);
          if (n > 0 && n == last) stable++; else { last = n; stable = 0; }
          usleep(100000);
      }
      LOGI("Assemblies on dinh: %zu", last); }

    T_STEP("ResolveAll");
    ResolveAll();
    isGameReady = true;
    LOGI("✅ MOD READY!");

    TraceStartCallReports();
    return nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_com_android_support_Main_CheckOverlayPermission(JNIEnv*, jclass, jobject) {}

__attribute__((constructor))
void l_m() {
    LOGI(".so injected");
    pthread_t ptid;
    pthread_create(&ptid, nullptr, h_t, nullptr);
}

#include "Includes/JNI_Boilerplate.h"
