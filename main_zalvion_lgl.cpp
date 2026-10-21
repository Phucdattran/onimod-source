// ============================================================
//  LGL MOD — Zalvion (com.fenragames.zalvion v1.0.1)
//  Speed + Free Shop (IapManager.Buy -> Finish trực tiếp)
// ============================================================

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

#define LOG_TAG   "ZALVION_MOD"
#define GAME_NAME "⚔️ Zalvion"
#define GAME_PACKAGE "com.fenragames.zalvion"
#include "Includes/LGLTrace.h"

// ═══════════════════════════════════════════════════════════
//  1. IL2CPP API
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

// ═══════════════════════════════════════════════════════════
//  2. ELF SYMBOL RESOLVER — KHÔNG SỬA
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
    if (sysv) { nsyms = sysv[1]; }
    else if (gnu) {
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
bool IsIL2CppLoaded() { bool f = false; dl_iterate_phdr(dl_cb, &f); return f; }

// ═══════════════════════════════════════════════════════════
//  3. HELPERS
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
static bool DoHook(void* addr, void* hook, void** orig, const char* name) {
    if (!addr) { LOGE("Hook SKIP (NULL): %s", name); return false; }
    int r = DobbyHook(addr, hook, orig);
    if (r == 0) { LOGI("Hook OK: %s @ %p", name, addr); return true; }
    LOGE("Hook FAIL(%d): %s @ %p", r, name, addr);
    return false;
}
template <typename T> static inline T& Field(void* obj, int off) {
    return *(T*)((uintptr_t)obj + off);
}

// ═══════════════════════════════════════════════════════════
//  4. TRẠNG THÁI
// ═══════════════════════════════════════════════════════════
static float g_speed  = 1.0f;   // [0] Speed
static bool  g_shop   = false;  // [1] Free Shop

// ═══════════════════════════════════════════════════════════
//  5. SPEED
// ═══════════════════════════════════════════════════════════
static float g_gameScale = 1.0f;
void  (*o_setTS)(float, void*) = nullptr;
float (*o_getTS)(void*)        = nullptr;

void h_setTS(float v, void* mi) {
    T_COUNT(setTS);
    g_gameScale = v;
    if (o_setTS) o_setTS(v * g_speed, mi);
}
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
//  6. FREE SHOP
//
//  Luồng trong game:
//    ShopItemDetailPopup.OnBuyClicked
//      -> ShopScreen.PurchaseAsync(product)
//           -> ShopScreen.PurchaseRealAsync(product)  [real-money IAP]
//           -> ShopScreen.PurchaseLocal(product)      [giá vàng]
//
//  Vấn đề: OwnsCosmeticProduct → true (từ session cũ) → shop hiện "đã sở hữu"
//          → không có nút Buy → PurchaseRealAsync/PurchaseLocal không bao giờ chạy
//
//  Chiến lược:
//    - CanBuy              → true  (bỏ check IAP connected)
//    - PurchaseRealAsync   → GrantContents + OwnedAdd(sku)
//    - PurchaseLocal       → GrantContents + OwnedAdd(sku)
//    - VipStatus.get_IsVip → true  (VIP server-side, GrantContents ko grant được)
//    OwnedAdd(sku) sau purchase: OwnsCosmeticProduct tự trả true → hiện "Đã sở hữu"
// ═══════════════════════════════════════════════════════════

// Cache ShopScreen.GrantContents(Product p) — private static, 1 param
// ARM64 static: (void* product, void* mi)
typedef void (*t_GrantContents)(void*, void*);
static t_GrantContents g_fn_GrantContents = nullptr;

// Cache IapManager.OwnedAdd(string sku) — private static, 1 param
// Dùng sau GrantContents để OwnsCosmeticProduct tự trả true (hiện "Đã sở hữu")
typedef void (*t_OwnedAdd)(void*, void*);
static t_OwnedAdd g_fn_OwnedAdd = nullptr;

// VipStatus.get_IsVip() — static, 0 param → bool
// VIP là server-side, GrantContents không grant được → hook getter
typedef bool (*t_IsVip)(void*);
static t_IsVip o_IsVip = nullptr;
bool h_IsVip(void* mi) {
    T_COUNT(IsVip);
    if (g_shop) return true;
    return o_IsVip ? o_IsVip(mi) : false;
}

// ShopScreen.PurchaseRealAsync(Product p) — private instance, 1 param
// ARM64 instance: (void* this, void* product, void* mi)
typedef void (*t_PurchaseRealAsync)(void*, void*, void*);
static t_PurchaseRealAsync o_PurchaseRealAsync = nullptr;
void h_PurchaseRealAsync(void* _this, void* product, void* mi) {
    T_COUNT(PurchaseRealAsync);
    if (g_shop) {
        LOGI("FreeShop: PurchaseRealAsync -> GrantContents");
        if (g_fn_GrantContents) g_fn_GrantContents(product, nullptr);
        else LOGE("FreeShop: GrantContents chua cache!");
        // Mark sku owned -> OwnsCosmeticProduct = true lan sau
        if (g_fn_OwnedAdd && product) {
            void* sku = *(void**)((uintptr_t)product + 0x10);
            if (sku) { g_fn_OwnedAdd(sku, nullptr); LOGI("FreeShop: OwnedAdd(sku) OK"); }
        }
        return;
    }
    if (o_PurchaseRealAsync) o_PurchaseRealAsync(_this, product, mi);
}

// ShopScreen.PurchaseLocal(Product p) — private instance, 1 param
// ARM64 instance: (void* this, void* product, void* mi)
typedef void (*t_PurchaseLocal)(void*, void*, void*);
static t_PurchaseLocal o_PurchaseLocal = nullptr;
void h_PurchaseLocal(void* _this, void* product, void* mi) {
    T_COUNT(PurchaseLocal);
    if (g_shop) {
        LOGI("FreeShop: PurchaseLocal -> GrantContents");
        if (g_fn_GrantContents) g_fn_GrantContents(product, nullptr);
        else LOGE("FreeShop: GrantContents chua cache!");
        // Mark sku owned -> OwnsCosmeticProduct = true lan sau
        if (g_fn_OwnedAdd && product) {
            void* sku = *(void**)((uintptr_t)product + 0x10);
            if (sku) { g_fn_OwnedAdd(sku, nullptr); LOGI("FreeShop: OwnedAdd(sku) OK"); }
        }
        return;
    }
    if (o_PurchaseLocal) o_PurchaseLocal(_this, product, mi);
}

// IapManager.CanBuy(string sku) — static, 1 param → bool
typedef bool (*t_CanBuy)(void*, void*);
static t_CanBuy o_CanBuy = nullptr;
bool h_CanBuy(void* sku, void* mi) {
    T_COUNT(CanBuy);
    if (g_shop) return true;
    return o_CanBuy ? o_CanBuy(sku, mi) : false;
}

// AdService.get_UseFakeAds() — private static, 0 params → bool
// Khi true: ShowRewarded gọi ShowFakeAd(onComplete) thay vì real ad → bỏ QC revive
typedef bool (*t_UseFakeAds)(void*);
static t_UseFakeAds o_UseFakeAds = nullptr;
bool h_UseFakeAds(void* mi) {
    T_COUNT(UseFakeAds);
    if (g_shop) return true;
    return o_UseFakeAds ? o_UseFakeAds(mi) : false;
}

// ═══════════════════════════════════════════════════════════
//  7. GẮN HOOK
// ═══════════════════════════════════════════════════════════
static void ResolveAll() {
    LOGI("=== Resolve START ===");

    // [0] Speed
    T_HOOK("UnityEngine", "Time", "get_timeScale", 0, getTS);
    T_HOOK("UnityEngine", "Time", "set_timeScale", 1, setTS);
    if (o_getTS) g_gameScale = o_getTS(nullptr);

    // [1] Free Shop
    void* kIap = FindClassAnywhere(OBFUSCATE("IapManager"));
    if (kIap) {
        void* mCan = FindMethod(kIap, OBFUSCATE("CanBuy"), 1);
        if (mCan) DoHook(MethodFnPtr(mCan), (void*)h_CanBuy, (void**)&o_CanBuy, "IapManager.CanBuy");
        else      LOGE("IapManager.CanBuy(1) not found");

        // Cache OwnedAdd(sku) — private static, dùng để đánh dấu đã sở hữu sau purchase
        void* mAdd = FindMethod(kIap, OBFUSCATE("OwnedAdd"), 1);
        if (mAdd) {
            g_fn_OwnedAdd = (decltype(g_fn_OwnedAdd)) MethodFnPtr(mAdd);
            LOGI("OwnedAdd cached: %p", (void*)g_fn_OwnedAdd);
        } else {
            LOGE("IapManager.OwnedAdd(1) not found");
        }
    } else {
        LOGE("IapManager class NOT FOUND");
    }

    // VipStatus.get_IsVip() → true khi g_shop ON (VIP server-side, GrantContents ko grant được)
    T_HOOK("", "VipStatus", "get_IsVip", 0, IsVip);

    // AdService.get_UseFakeAds() → true khi g_shop ON → ShowRewarded dùng ShowFakeAd → bỏ QC
    T_HOOK("", "AdService", "get_UseFakeAds", 0, UseFakeAds);

    void* kShop = FindClassAnywhere(OBFUSCATE("ShopScreen"));
    if (kShop) {
        // Cache GrantContents (private static)
        void* mGrant = FindMethod(kShop, OBFUSCATE("GrantContents"), 1);
        if (mGrant) {
            g_fn_GrantContents = (decltype(g_fn_GrantContents)) MethodFnPtr(mGrant);
            LOGI("GrantContents cached: %p", (void*)g_fn_GrantContents);
        } else {
            LOGE("ShopScreen.GrantContents(1) not found");
        }

        // PurchaseRealAsync → GrantContents (gói real-money)
        void* mPRA = FindMethod(kShop, OBFUSCATE("PurchaseRealAsync"), 1);
        if (mPRA) DoHook(MethodFnPtr(mPRA), (void*)h_PurchaseRealAsync, (void**)&o_PurchaseRealAsync, "ShopScreen.PurchaseRealAsync");
        else      LOGE("ShopScreen.PurchaseRealAsync(1) not found");

        // PurchaseLocal → GrantContents (gói giá vàng, bỏ qua trừ vàng)
        void* mPL = FindMethod(kShop, OBFUSCATE("PurchaseLocal"), 1);
        if (mPL) DoHook(MethodFnPtr(mPL), (void*)h_PurchaseLocal, (void**)&o_PurchaseLocal, "ShopScreen.PurchaseLocal");
        else     LOGE("ShopScreen.PurchaseLocal(1) not found");
    } else {
        LOGE("ShopScreen class NOT FOUND");
    }

    TraceReport();
    LOGI("=== Resolve DONE ===");
}

// ═══════════════════════════════════════════════════════════
//  8. MENU
// ═══════════════════════════════════════════════════════════
jobjectArray GetFeatureList(JNIEnv* env, jobject) {
    const char* features[] = {
        OBFUSCATE("Category_" GAME_NAME),
        OBFUSCATE("0_SeekBar_⚡ Tốc Độ Game_1_10"),
        OBFUSCATE("1_Toggle_🛒 Miễn Phí Cửa Hàng"),
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
    T_STEP("bam tinh nang so %d (value=%d bat=%d)", featNum, value, boolean);
    switch (featNum) {
        case 0:
            g_speed = (value < 1) ? 1.0f : (float)value;
            ApplySpeed();
            break;
        case 1:
            g_shop = boolean;
            LOGI("FreeShop: %s", g_shop ? "BAT" : "TAT");
            break;
    }
}

// ═══════════════════════════════════════════════════════════
//  9. KHỞI TẠO — KHÔNG SỬA
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
    LOGI("Hook il2cpp_init: %s", hooked ? "OK" : "FAIL (cho theo thoi gian)");

    T_STEP("cho il2cpp_init");
    for (int i = 0; i < 300 && !g_il2cpp_ready; i++) usleep(100000);
    if (g_il2cpp_ready) LOGI("il2cpp_init xong");
    else                LOGE("Khong thay il2cpp_init -> cho them 5s");
    for (int i = 0; i < 20; i++) usleep(250000); // 5s

    T_STEP("attach + cho assemblies");
    void* domain = il2cpp_domain_get();
    if (domain && il2cpp_thread_attach) il2cpp_thread_attach(domain);
    { size_t last = 0; int stable = 0;
      while (stable < 5) {
          size_t n = 0; il2cpp_domain_get_assemblies(domain, &n);
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
