// ============================================================
//  LGL MOD — com.mobibrain.championforge (v1.0.12)
//  Test qua LGL Test Loader (máy root), sau đó ghép APK.
//
//  Tính năng (menu LGL):
//    [0]  Speed      — SeekBar 1–10 (nhân timeScale)
//    [1]  Free Shop  — mua đồ không tốn tiền thật
//
//  Free Shop (Kunpo IAP framework):
//    - get_Buying      (Kar98k.IAP.Kunpo, Kunpo_IAP)  → false: xóa _isBuying stuck
//    - FailCondition   (Kar98k.IAP.Kunpo, Kunpo_IAP)  → false: không fail
//    - CancelCondition (Kar98k.IAP.Kunpo, Kunpo_IAP)  → false: không cancel
//    - MapGoogleBuyResult (IapUnityGoogle)             → 0 (success): đổi mã lỗi Google Pay
//
//  Cách dùng Free Shop:
//    1. Chờ Toast "LGL Mod: lib XX:XX:XX" hiện lên
//    2. Đợi thêm 50 giây (mod cần thời gian init)
//    3. Bật toggle "Free Shop" trong menu LGL
//    4. Bấm mua item trong shop
//    5. Dialog Google Pay hiện → bấm Hủy/Back
//    6. Free Shop hook đổi kết quả thành Success → item cấp về
//
//  Lỗi "你有一笔未完成的订单":
//    Nếu popup này hiện = _isBuying đang stuck true từ lần mua trước.
//    Hook get_Buying → false sẽ fix vấn đề này.
// ============================================================

#define LOG_TAG      "CHAMPIONFORGE_MOD"
#define GAME_NAME    "Champion Forge"
#define GAME_PACKAGE "com.mobibrain.championforge"

#include "Includes/LGLTrace.h"

#include <pthread.h>
#include <jni.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <link.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <android/log.h>

#include "Includes/obfuscate.h"
#include "Includes/Logger.h"
#include "Includes/Utils.h"
#include "Menu/Setup.h"
#include <Dobby/dobby.h>
#include "MenuStrings.h"
#include "Includes/EngineVIP.h"

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

// ═══════════════════════════════════════════════════════════
//  2. ElfSym — KHÔNG dùng dlopen (chỉ đọc bộ nhớ)
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
    LOAD(il2cpp_domain_get);       LOAD(il2cpp_thread_attach);
    LOAD(il2cpp_domain_get_assemblies); LOAD(il2cpp_assembly_get_image);
    LOAD(il2cpp_image_get_class_count); LOAD(il2cpp_image_get_class);
    LOAD(il2cpp_class_from_name);  LOAD(il2cpp_class_get_name);
    LOAD(il2cpp_class_get_methods); LOAD(il2cpp_method_get_name);
    LOAD(il2cpp_method_get_param_count);
    LOAD(il2cpp_class_get_field_from_name); LOAD(il2cpp_field_get_offset);
    #undef LOAD
    bool ok = il2cpp_domain_get && il2cpp_domain_get_assemblies &&
              il2cpp_assembly_get_image && il2cpp_class_from_name &&
              il2cpp_class_get_methods && il2cpp_method_get_name;
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
//  3. HELPERS — tìm class / method theo TÊN
// ═══════════════════════════════════════════════════════════
static inline void* MethodFnPtr(void* mi) { return mi ? *(void**)mi : nullptr; }

void* FindClass(const char* ns, const char* name) {
    size_t n = 0;
    void** list = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &n);
    for (size_t i = 0; i < n; i++) {
        void* img = il2cpp_assembly_get_image(list[i]); if (!img) continue;
        if (void* k = il2cpp_class_from_name(img, ns, name)) return k;
    }
    return nullptr;
}
void* FindClassAnywhere(const char* name) {
    size_t n = 0;
    void** list = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &n);
    for (size_t i = 0; i < n; i++) {
        void* img = il2cpp_assembly_get_image(list[i]); if (!img) continue;
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
        const char* n = il2cpp_method_get_name(m); if (!n || strcmp(n, name) != 0) continue;
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
    if (!k) { LOGE("NOT FOUND class: %s", cls); return nullptr; }
    void* m = FindMethod(k, method, nParams);
    if (!m) { LOGE("NOT FOUND: %s.%s(%d)", cls, method, nParams); return nullptr; }
    return MethodFnPtr(m);
}
template <typename T> static inline T& Field(void* obj, int off) {
    return *(T*)((uintptr_t)obj + off);
}
static bool DoHook(void* addr, void* hook, void** orig, const char* name) {
    if (!addr) { LOGE("Hook SKIP (NULL): %s", name); return false; }
    int r = DobbyHook(addr, hook, orig);
    if (r == 0) { LOGI("Hook OK: %s @ %p", name, addr); return true; }
    LOGE("Hook FAIL(%d): %s @ %p", r, name, addr); return false;
}
// T_HOOK từ LGLTrace.h — dùng trực tiếp (không redefine để TraceReport() đếm đúng)

// ═══════════════════════════════════════════════════════════
//  4. TRẠNG THÁI CHỨC NĂNG
// ═══════════════════════════════════════════════════════════
static float g_speed    = 1.0f;   // [0] Speed (SeekBar 1–10)
static bool  g_freeShop = false;  // [1] Free Shop

// ═══════════════════════════════════════════════════════════
//  5. SPEED — hook Time.get_timeScale + Time.set_timeScale
//     Nhân hệ số g_speed vào giá trị game đặt.
//     Getter trả giá trị game gốc (không nhân dồn).
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
    if (g_speed != 1.0f) return g_gameScale;  // trả giá trị game gốc, tránh nhân dồn
    return o_getTS ? o_getTS(mi) : g_gameScale;
}
static void ApplySpeed() {
    if (!o_setTS) { LOGE("Speed KHONG chay: chua hook Time.set_timeScale"); return; }
    o_setTS(g_gameScale * g_speed, nullptr);
    LOGI("Speed x%.0f: game dat %.2f -> thuc te %.2f", g_speed, g_gameScale, g_gameScale * g_speed);
}

// ═══════════════════════════════════════════════════════════
//  6. FREE SHOP — Kunpo IAP Framework
//
//  Luồng mua hàng (Kunpo IAP):
//    BuyProduct → BuyProductRun (coroutine) →
//      [kiểm tra get_Buying()] → StepMakePurchaseOrder →
//      StepCallPayment → StepVerification →
//      if (FailCondition) → fail / else → FinishPurchaseSuccessOrder
//
//  Hook:
//    get_Buying      → false  (xóa _isBuying stuck → cho phép bắt đầu mua)
//    FailCondition   → false  (không bao giờ fail)
//    CancelCondition → false  (không bao giờ cancel)
//    MapGoogleBuyResult → 0   (Google Pay error code → success)
// ═══════════════════════════════════════════════════════════

// get_Buying() → bool (STATIC, 0 params) @ 0x31CD8E8 UNIQUE
// Trả về _isBuying static field. Hook → false để xóa trạng thái "đang mua" bị kẹt.
bool (*o_getBuying)(void*) = nullptr;
bool h_getBuying(void* mi) {
    T_COUNT(getBuying);
    return false;  // luôn "không đang mua" → cho phép bắt đầu purchase mới
}

// FailCondition(buyProduct) → bool (instance, 1 param)
// Trả về true nếu purchase thất bại. Hook → false để không bao giờ fail.
bool (*o_FailCondition)(void*, void*, void*)   = nullptr;
bool h_FailCondition(void* self, void* buyProduct, void* mi) {
    T_COUNT(FailCondition);
    if (g_freeShop) return false;
    return o_FailCondition ? o_FailCondition(self, buyProduct, mi) : false;
}

// CancelCondition(buyProduct) → bool (instance, 1 param)
// Trả về true nếu purchase bị huỷ. Hook → false để không bao giờ cancel.
bool (*o_CancelCondition)(void*, void*, void*) = nullptr;
bool h_CancelCondition(void* self, void* buyProduct, void* mi) {
    T_COUNT(CancelCondition);
    if (g_freeShop) return false;
    return o_CancelCondition ? o_CancelCondition(self, buyProduct, mi) : false;
}

// MapGoogleBuyResult(BuyResultCode, GooglePayment) → int32 (instance, 2 params)
// Chuyển đổi kết quả Google Play thành mã nội bộ. 0 = success.
// Hook → 0 để mọi kết quả Google Pay đều được coi là thành công.
int (*o_MapGBuyResult)(void*, int, void*, void*) = nullptr;
int h_MapGBuyResult(void* self, int buyResultCode, void* googlePayment, void* mi) {
    T_COUNT(MapGBuyResult);
    if (g_freeShop) {
        LOGI("FreeShop: MapGoogleBuyResult(%d) -> 0 (success)", buyResultCode);
        return 0;
    }
    return o_MapGBuyResult ? o_MapGBuyResult(self, buyResultCode, googlePayment, mi) : buyResultCode;
}

// ═══════════════════════════════════════════════════════════
//  7. ResolveAll
// ═══════════════════════════════════════════════════════════
static void ResolveAll() {
    LOGI("=== Resolve START ===");

    // [0] Speed: UnityEngine.Time
    T_HOOK("UnityEngine", "Time", "set_timeScale", 1, setTS);
    T_HOOK("UnityEngine", "Time", "get_timeScale", 0, getTS);

    // [1] Free Shop: Kunpo IAP
    // get_Buying: static 0 params — fix _isBuying stuck (gây popup "未完成的订单")
    T_HOOK("Kar98k.IAP.Kunpo", "Kunpo_IAP", "get_Buying",      0, getBuying);
    // FailCondition + CancelCondition: base class, offset UNIQUE
    T_HOOK("Kar98k.IAP.Kunpo", "Kunpo_IAP", "FailCondition",   1, FailCondition);
    T_HOOK("Kar98k.IAP.Kunpo", "Kunpo_IAP", "CancelCondition", 1, CancelCondition);
    // MapGoogleBuyResult: IapUnityGoogle (namespace rỗng, offset UNIQUE)
    T_HOOK("", "IapUnityGoogle", "MapGoogleBuyResult", 2, MapGBuyResult);

    TraceReport();
    LOGI("=== Resolve DONE ===");
}

// ═══════════════════════════════════════════════════════════
//  8. MENU — GetFeatureList + Changes
// ═══════════════════════════════════════════════════════════
jobjectArray GetFeatureList(JNIEnv* env, jobject) {
    const char* features[] = {
        OBFUSCATE("Category_" GAME_NAME),
        OBFUSCATE("0_SeekBar_Speed_1_10"),
        OBFUSCATE("1_Toggle_Free Shop"),
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
        case 0: {
            float spd = (float)value;
            if (spd < 1.0f) spd = 1.0f;
            if (spd > 10.0f) spd = 10.0f;
            g_speed = spd;
            LOGI("Speed: x%.0f", g_speed);
            ApplySpeed();
            break;
        }
        case 1:
            g_freeShop = boolean;
            LOGI("Free Shop: %s", g_freeShop ? "BAT" : "TAT");
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
    LOGI("Hook il2cpp_init: %s", hooked ? "OK" : "FAIL");

    T_STEP("cho il2cpp_init");
    for (int i = 0; i < 300 && !g_il2cpp_ready; i++) usleep(100000);
    if (g_il2cpp_ready) {
        LOGI("il2cpp_init xong - cho them 2s on dinh");
        for (int i = 0; i < 8; i++) usleep(250000);  // 2s
    } else {
        // il2cpp_init da chay truoc khi mod load - kiem tra domain luon
        void* testDomain = il2cpp_domain_get();
        if (testDomain) {
            LOGI("il2cpp da san sang (domain OK) - tiep tuc ngay");
            g_il2cpp_ready = true;
        } else {
            LOGE("Khong thay il2cpp_init va domain NULL -> cho them 5s");
            for (int i = 0; i < 20; i++) usleep(250000);
        }
    }

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
    LOGI("MOD READY!");
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
