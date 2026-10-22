// ============================================================
//  LGL MOD — wool.thread.saga.triple.match.sort v1.2230
//  Chức năng: Speed · Free Shop · Bỏ Ads (Banner + Interstitial)
//
//  Free Shop: hook DbtPurchasingService.PurchaseProduct
//    → không qua SDK → tạo TransactionInfo giả (productId đúng)
//    → gọi PurchasingService.HandleTransactionFinished (protected)
//    → HandleTransactionFinished command chạy
//    → ClaimIAPProductRewardItems → phần thưởng được phát
//
//  Bỏ Ads:
//    GetRemoveAdsState() → 1  (SDK nhận biết gói bỏ ads)
//    ShowBannerStatic()  → no-op
//    ShowInterstitialResult() → false
//    (Rewarded video vẫn hoạt động bình thường)
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

#define LOG_TAG   "WOO_MOD"
#define GAME_NAME "🧶 Wool Thread Saga"
#define GAME_PACKAGE "wool.thread.saga.triple.match.sort"

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
void*       (*il2cpp_object_new)(void*)                                    = nullptr;

// ═══════════════════════════════════════════════════════════
//  2. ĐỌC SYMBOL libil2cpp.so TRONG BỘ NHỚ (KHÔNG dùng dlopen)
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
        const uint32_t* chain   = buckets + nbuckets;
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
    LOGI("InitIL2CppAPI: %s%s", ok ? "OK" : "FAIL",
         il2cpp_object_new ? " (object_new OK)" : " (object_new FAIL - FreeShop may not work)");
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
static float g_speed = 1.0f;   // [0] SeekBar tốc độ 1–10
static bool  g_shop  = false;  // [1] Free Shop
static bool  g_noads = false;  // [2] Bỏ Ads (banner + interstitial)

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
//  dump.cs:
//    internal class Isis.Economy.Purchasing.Dbt.Services.DbtPurchasingService
//      public override void PurchaseProduct(string productId, PurchasingPayload payload)
//    abstract class Isis.Economy.Purchasing.Services.PurchasingService
//      protected void HandleTransactionFinished(TransactionInfo transactionInfo) // 0x152A054
//    class Chiron.Purchasing.TransactionInfo
//      <ProductId>  // 0x10 (string)
//      <Quantity>   // 0x28 (int)
//      <State>      // 0x2C (TransactionState enum: 1=Purchased)
// ═══════════════════════════════════════════════════════════
static void* g_txInfoKlass = nullptr;
typedef void (*t_HandleTxFinished)(void* self, void* ti, void* mi);
static t_HandleTxFinished s_HandleTxFinished = nullptr;
static void* s_HandleTxFinishedMI = nullptr; // MethodInfo* — bắt buộc truyền, nullptr crash

typedef void (*t_PurchaseProduct)(void* self, void* productId, void* payload, void* mi);
static t_PurchaseProduct o_PurchaseProduct = nullptr;

void h_PurchaseProduct(void* self, void* productId, void* payload, void* mi) {
    T_COUNT(PurchaseProduct);
    if (g_shop) {
        if (il2cpp_object_new && g_txInfoKlass && s_HandleTxFinished) {
            void* ti = il2cpp_object_new(g_txInfoKlass);
            if (ti) {
                // Ghi trực tiếp vào field theo offset từ dump.cs
                *(void**)((uint8_t*)ti + 0x10) = productId; // ProductId (string managed)
                *(int*)  ((uint8_t*)ti + 0x28) = 1;          // Quantity = 1
                *(int*)  ((uint8_t*)ti + 0x2C) = 1;          // State = Purchased
                s_HandleTxFinished(self, ti, s_HandleTxFinishedMI);
                LOGI("FreeShop: PurchaseProduct giam -> HandleTransactionFinished gia OK");
                return;
            }
        } else {
            LOGE("FreeShop: thieu txInfoKlass=%p HandleTxFin=%p object_new=%p",
                 g_txInfoKlass, (void*)s_HandleTxFinished, (void*)il2cpp_object_new);
        }
    }
    if (o_PurchaseProduct) o_PurchaseProduct(self, productId, payload, mi);
}

// ═══════════════════════════════════════════════════════════
//  7. BỎ ADS
//
//  dump.cs:
//    class ZMYSDK.AndroidSDK : ZMYSDK.ISDK
//      override int    GetRemoveAdsState()                      // 0x2469664
//      override void   ShowBannerStatic(int pos)                // 0x2468990
//      override bool   ShowInterstitialResult(string, string, string) // 0x2468DF4
//  (Rewarded video — ZMYSDKManager.ShowVide — không hook,
//   player vẫn xem được video tự nguyện để lấy phần thưởng)
// ═══════════════════════════════════════════════════════════

// GetRemoveAdsState() → 1 khi g_noads: SDK nhận biết đã mua gói bỏ ads
typedef int (*t_GetRemoveAdsState)(void* self, void* mi);
static t_GetRemoveAdsState o_GetRemoveAdsState = nullptr;
int h_GetRemoveAdsState(void* self, void* mi) {
    T_COUNT(GetRemoveAdsState);
    if (g_noads) return 1;
    return o_GetRemoveAdsState ? o_GetRemoveAdsState(self, mi) : 0;
}

// ShowBannerStatic(int pos) → không gọi gốc khi g_noads
typedef void (*t_ShowBannerStatic)(void* self, int pos, void* mi);
static t_ShowBannerStatic o_ShowBannerStatic = nullptr;
void h_ShowBannerStatic(void* self, int pos, void* mi) {
    T_COUNT(ShowBannerStatic);
    if (g_noads) return;
    if (o_ShowBannerStatic) o_ShowBannerStatic(self, pos, mi);
}

// ShowInterstitialResult(string, string, string) → false khi g_noads
typedef bool (*t_ShowInterstitialResult)(void* self, void* a, void* b, void* c, void* mi);
static t_ShowInterstitialResult o_ShowInterstitialResult = nullptr;
bool h_ShowInterstitialResult(void* self, void* a, void* b, void* c, void* mi) {
    T_COUNT(ShowInterstitialResult);
    if (g_noads) return false;
    return o_ShowInterstitialResult ? o_ShowInterstitialResult(self, a, b, c, mi) : false;
}

// ═══════════════════════════════════════════════════════════
//  8. GẮN HOOK
// ═══════════════════════════════════════════════════════════
static void ResolveAll() {
    LOGI("=== Resolve START ===");

    // Speed
    T_HOOK("UnityEngine", "Time", "get_timeScale", 0, getTS);
    T_HOOK("UnityEngine", "Time", "set_timeScale", 1, setTS);
    if (o_getTS) g_gameScale = o_getTS(nullptr);

    // Free Shop — hook entry point của purchase flow (DbtPurchasingService)
    // namespace: Isis.Economy.Purchasing.Dbt.Services, class: DbtPurchasingService
    T_HOOK("Isis.Economy.Purchasing.Dbt.Services", "DbtPurchasingService", "PurchaseProduct", 2, PurchaseProduct);

    // Chuẩn bị tạo TransactionInfo giả (cần klass + hàm HandleTransactionFinished)
    g_txInfoKlass = FindClass("Chiron.Purchasing", "TransactionInfo");
    LOGI("TransactionInfo klass: %s", g_txInfoKlass ? "OK" : "FAIL");

    void* psKlass = FindClass("Isis.Economy.Purchasing.Services", "PurchasingService");
    if (psKlass) {
        void* mi = FindMethod(psKlass, "HandleTransactionFinished", 1);
        if (mi) {
            s_HandleTxFinished   = (t_HandleTxFinished) MethodFnPtr(mi);
            s_HandleTxFinishedMI = mi;
            LOGI("HandleTransactionFinished: OK @ %p MI=%p", (void*)s_HandleTxFinished, mi);
        } else {
            LOGE("HandleTransactionFinished(1): FAIL (FreeShop se khong chay)");
        }
    } else {
        LOGE("PurchasingService klass: FAIL (FreeShop se khong chay)");
    }

    // Bỏ Ads (banner + interstitial, namespace ZMYSDK class AndroidSDK)
    T_HOOK("ZMYSDK", "AndroidSDK", "GetRemoveAdsState",      0, GetRemoveAdsState);
    T_HOOK("ZMYSDK", "AndroidSDK", "ShowBannerStatic",       1, ShowBannerStatic);
    T_HOOK("ZMYSDK", "AndroidSDK", "ShowInterstitialResult", 3, ShowInterstitialResult);

    TraceReport();
    LOGI("=== Resolve DONE ===");
}

// ═══════════════════════════════════════════════════════════
//  9. MENU
// ═══════════════════════════════════════════════════════════
jobjectArray GetFeatureList(JNIEnv* env, jobject) {
    const char* features[] = {
        OBFUSCATE("Category_" GAME_NAME),
        OBFUSCATE("0_SeekBar_⚡ Tốc Độ_1_10"),
        OBFUSCATE("1_Toggle_🛒 Miễn Phí Cửa Hàng"),
        OBFUSCATE("2_Toggle_🚫 Bỏ Quảng Cáo"),
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
        case 0:
            g_speed = (value < 1) ? 1.0f : (float) value;
            // KHÔNG gọi ApplySpeed() ở đây — Changes() chạy trên JNI thread chưa attach IL2CPP
            // Speed sẽ tự áp dụng khi hook set_timeScale được game gọi tiếp theo
            LOGI("Speed: cap nhat %.1f (se ap dung khi game goi timeScale tiep theo)", g_speed);
            break;
        case 1:
            g_shop = boolean;
            LOGI("FreeShop: %d (txInfoKlass=%p HandleTxFin=%p)",
                 g_shop, g_txInfoKlass, (void*)s_HandleTxFinished);
            break;
        case 2:
            g_noads = boolean;
            LOGI("NoAds: %d", g_noads);
            break;
    }
}

// ═══════════════════════════════════════════════════════════
//  10. KHỞI TẠO — KHÔNG SỬA
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
