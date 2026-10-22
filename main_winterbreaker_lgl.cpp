// ============================================================
//  LGL MOD — Winter Breaker (com.winter.breaker v1.0.22)
//  Speed + Free Shop + Remove Ads + Damage
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

#define LOG_TAG   "WINTERBREAKER_MOD"
#define GAME_NAME "❄️ Winter Breaker"
#define GAME_PACKAGE "com.winter.breaker"
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
void*       (*il2cpp_object_get_class)(void*)                              = nullptr;

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
    LOAD(il2cpp_object_get_class);
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

// Invoke Action<bool> callback với value (true=mua thành công)
static bool InvokeBoolCallback(void* callback, bool value) {
    if (!callback || !il2cpp_object_get_class) return false;
    void* klass = il2cpp_object_get_class(callback);
    if (!klass) return false;
    void* mi = FindMethod(klass, OBFUSCATE("Invoke"), 1);
    if (!mi) { LOGE("InvokeBoolCallback: Invoke(1) not found"); return false; }
    typedef void (*t_Invoke)(void* self, int32_t val, void* mi);
    void* fn = MethodFnPtr(mi);
    if (!fn) return false;
    ((t_Invoke)fn)(callback, value ? 1 : 0, nullptr);
    return true;
}

// ═══════════════════════════════════════════════════════════
//  4. TRẠNG THÁI
// ═══════════════════════════════════════════════════════════
static float g_speed  = 1.0f;   // [0] seekbar
static bool  g_shop   = false;  // [1] toggle
static bool  g_noads  = false;  // [2] toggle
static float g_damage = 1.0f;   // [3] seekbar

// ═══════════════════════════════════════════════════════════
//  5. SPEED
//  UnityEngine.Time.get_timeScale (static, 0 params)
//  UnityEngine.Time.set_timeScale (static, 1 param)
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
//  WesternBreaker.StoreModel.BuyProduct(string id, Action<bool> callback)
//  instance, 2 params — hook -> invoke callback(true)
// ═══════════════════════════════════════════════════════════
typedef void (*t_BuyProduct)(void*, void*, void*, void*);
static t_BuyProduct o_BuyProduct = nullptr;

void h_BuyProduct(void* self, void* id, void* callback, void* mi) {
    T_COUNT(BuyProduct);
    if (g_shop) {
        LOGI("FreeShop: BuyProduct -> callback(true)");
        if (!InvokeBoolCallback(callback, true))
            LOGE("FreeShop: InvokeBoolCallback FAIL");
        return;
    }
    if (o_BuyProduct) o_BuyProduct(self, id, callback, mi);
}

// ═══════════════════════════════════════════════════════════
//  7. REMOVE ADS
//  GamestotySpace.GamestotyAds.DontShowAdsByPlacement(string placement)
//  private static, 1 param, bool — hook -> true (chặn tất cả vị trí QC)
// ═══════════════════════════════════════════════════════════
typedef bool (*t_DontShowAds)(void*, void*);
static t_DontShowAds o_DontShowAds = nullptr;

bool h_DontShowAds(void* placement, void* mi) {
    T_COUNT(DontShowAds);
    if (g_noads) return true;
    return o_DontShowAds ? o_DontShowAds(placement, mi) : false;
}

// ═══════════════════════════════════════════════════════════
//  8. DAMAGE MULTIPLIER
//  WesternBreaker.GatheringToolModel.get_GlobalDamageMultiplier() instance 0 params
//  WesternBreaker.Gun.get_GlobalDamageMultiplier()                instance 0 params
// ═══════════════════════════════════════════════════════════
typedef float (*t_getDmg)(void*, void*);
static t_getDmg o_GTM_getDmg = nullptr;
static t_getDmg o_Gun_getDmg = nullptr;

float h_GTM_getDmg(void* self, void* mi) {
    T_COUNT(GTM_getDmg);
    float base = o_GTM_getDmg ? o_GTM_getDmg(self, mi) : 1.0f;
    return base * g_damage;
}
float h_Gun_getDmg(void* self, void* mi) {
    T_COUNT(Gun_getDmg);
    float base = o_Gun_getDmg ? o_Gun_getDmg(self, mi) : 1.0f;
    return base * g_damage;
}

// ═══════════════════════════════════════════════════════════
//  9. GẮN HOOK
// ═══════════════════════════════════════════════════════════
static void ResolveAll() {
    LOGI("=== Resolve START ===");

    // [0] Speed
    T_HOOK("UnityEngine", "Time", "get_timeScale", 0, getTS);
    T_HOOK("UnityEngine", "Time", "set_timeScale", 1, setTS);
    if (o_getTS) g_gameScale = o_getTS(nullptr);

    // [1] Free Shop — StoreModel.BuyProduct(string, Action<bool>) instance 2 params
    T_HOOK("WesternBreaker", "StoreModel", "BuyProduct", 2, BuyProduct);

    // [2] Remove Ads — GamestotyAds.DontShowAdsByPlacement(string) static 1 param
    T_HOOK("GamestotySpace", "GamestotyAds", "DontShowAdsByPlacement", 1, DontShowAds);

    // [3] Damage — hook cả 2 class chứa get_GlobalDamageMultiplier
    {
        void* kGTM = FindClass(OBFUSCATE("WesternBreaker"), OBFUSCATE("GatheringToolModel"));
        if (kGTM) {
            void* m = FindMethod(kGTM, OBFUSCATE("get_GlobalDamageMultiplier"), 0);
            if (m) DoHook(MethodFnPtr(m), (void*)h_GTM_getDmg, (void**)&o_GTM_getDmg,
                          "GatheringToolModel.get_GlobalDamageMultiplier");
            else   LOGE("GatheringToolModel.get_GlobalDamageMultiplier(0) NOT FOUND");
        } else {
            LOGE("GatheringToolModel class NOT FOUND");
        }

        void* kGun = FindClass(OBFUSCATE("WesternBreaker"), OBFUSCATE("Gun"));
        if (kGun) {
            void* m = FindMethod(kGun, OBFUSCATE("get_GlobalDamageMultiplier"), 0);
            if (m) DoHook(MethodFnPtr(m), (void*)h_Gun_getDmg, (void**)&o_Gun_getDmg,
                          "Gun.get_GlobalDamageMultiplier");
            else   LOGE("Gun.get_GlobalDamageMultiplier(0) NOT FOUND");
        } else {
            LOGE("Gun class NOT FOUND");
        }
    }

    TraceReport();
    LOGI("=== Resolve DONE ===");
}

// ═══════════════════════════════════════════════════════════
//  10. MENU
// ═══════════════════════════════════════════════════════════
jobjectArray GetFeatureList(JNIEnv* env, jobject) {
    const char* features[] = {
        OBFUSCATE("Category_" GAME_NAME),
        OBFUSCATE("0_SeekBar_⚡ Tốc Độ Game_1_10"),
        OBFUSCATE("1_Toggle_🛒 Miễn Phí Cửa Hàng"),
        OBFUSCATE("2_Toggle_🚫 Bỏ Quảng Cáo"),
        OBFUSCATE("3_SeekBar_⚔️ Nhân Sát Thương_1_20"),
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
        case 2:
            g_noads = boolean;
            LOGI("NoAds: %s", g_noads ? "BAT" : "TAT");
            break;
        case 3:
            g_damage = (value < 1) ? 1.0f : (float)value;
            LOGI("Damage x%.0f", g_damage);
            break;
    }
}

// ═══════════════════════════════════════════════════════════
//  11. KHỞI TẠO — KHÔNG SỬA
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
