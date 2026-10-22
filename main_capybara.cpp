// ============================================================
//  LGL MOD — com.habby.capybara (Capybara Go!)
//  Unity IL2CPP arm64 | LGL Test Loader v2.1 | Bản mẫu v2.2
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

#define LOG_TAG      "CAPYBARA_MOD"
#define GAME_NAME    "🦫 Capybara Go!"
#define GAME_PACKAGE "com.habby.capybara"
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
//  2. ĐỌC SYMBOL libil2cpp.so (KHÔNG dlopen) — KHÔNG SỬA
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
//  4. TRẠNG THÁI CHỨC NĂNG
// ═══════════════════════════════════════════════════════════
static float g_speed      = 1.0f;   // [0] SeekBar tốc độ
static bool  g_coins      = false;  // [1] Vô hạn Coins
static bool  g_diamonds   = false;  // [2] Vô hạn Kim Cương
static bool  g_items      = false;  // [3] Vô hạn Vật Phẩm
static bool  g_freeshop   = false;  // [4] Mở Khóa Shop
static bool  g_hasenough  = false;  // [5] Bỏ Qua Chi Phí Nâng Cấp
static bool  g_exploit_mythic = false; // [6] Exploit Mythic Shop (count âm)

// ═══════════════════════════════════════════════════════════
//  5. SPEED — KHÔNG SỬA
// ═══════════════════════════════════════════════════════════
static float g_gameScale = 1.0f;
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
//  6. HOOKS GAME
// ═══════════════════════════════════════════════════════════

// ── [1] Proto.Common.UserCurrency.get_Coins — instance, 0 tham số, trả Int64
//    Hiển thị coins vô hạn trong UI, client check pass
int64_t (*o_getCoins)(void*, void*) = nullptr;
int64_t h_getCoins(void* self, void* mi) {
    T_COUNT(getCoins);
    if (g_coins) return 9999999LL;
    return o_getCoins ? o_getCoins(self, mi) : 0LL;
}

// ── [2] Proto.Common.UserCurrency.get_Diamonds — instance, 0 tham số, trả Int32
int32_t (*o_getDiamonds)(void*, void*) = nullptr;
int32_t h_getDiamonds(void* self, void* mi) {
    T_COUNT(getDiamonds);
    if (g_diamonds) return 999999;
    return o_getDiamonds ? o_getDiamonds(self, mi) : 0;
}

// ── [3a] HotFix.PropDataModule.GetItemCount — instance, 1 tham số (enum), trả Int64
int64_t (*o_GetItemCount)(void*, int32_t, void*) = nullptr;
int64_t h_GetItemCount(void* self, int32_t name, void* mi) {
    T_COUNT(GetItemCount);
    if (g_items) return 9999999LL;
    return o_GetItemCount ? o_GetItemCount(self, name, mi) : 0LL;
}

// ── [3b] HotFix.PropDataModule.IsHaveItemDatas — instance, 1 tham số (List), trả bool
bool (*o_IsHaveItemDatas)(void*, void*, void*) = nullptr;
bool h_IsHaveItemDatas(void* self, void* itemDatas, void* mi) {
    T_COUNT(IsHaveItemDatas);
    if (g_items) return true;
    return o_IsHaveItemDatas ? o_IsHaveItemDatas(self, itemDatas, mi) : false;
}

// ── [4] HotFix.DxxTools.UI.IsCanShopBuy — STATIC, 1 tham số (int), trả bool
bool (*o_IsCanShopBuy)(int32_t, void*) = nullptr;
bool h_IsCanShopBuy(int32_t itemId, void* mi) {
    T_COUNT(IsCanShopBuy);
    if (g_freeshop) return true;
    return o_IsCanShopBuy ? o_IsCanShopBuy(itemId, mi) : false;
}

// ── [5a] HotFix.GashaponDataModule.HasEnoughLuck — instance, 1 tham số, trả bool
//    Địa chỉ 0x27FF898: cùng address với HasEnoughLimitedTimeSceneSkinUnlockItem
//    và HasEnoughPermanentSceneSkinUnlockItem → hook 1 cái là cover hết
bool (*o_HasEnoughLuck)(void*, int32_t, void*) = nullptr;
bool h_HasEnoughLuck(void* self, int32_t drawCount, void* mi) {
    T_COUNT(HasEnoughLuck);
    if (g_hasenough) return true;
    return o_HasEnoughLuck ? o_HasEnoughLuck(self, drawCount, mi) : false;
}

// ── [6] HotFix.NetworkUtils.Mythic.DoShopRequest — STATIC, 3 tham số
//    Intercept: đổi count > 0 thành -9900000 → server tính cost âm → cộng diamonds
void (*o_DoShopRequest)(int32_t, int32_t, void*, void*) = nullptr;
void h_DoShopRequest(int32_t shopItemId, int32_t count, void* callback, void* mi) {
    T_COUNT(DoShopRequest);
    LOGI("DoShopRequest: shopItemId=%d count=%d exploit=%d", shopItemId, count, g_exploit_mythic);
    if (g_exploit_mythic && count > 0) {
        LOGI("EXPLOIT: shopItemId=%d count=%d -> -9900000", shopItemId, count);
        count = -9900000;
    }
    if (o_DoShopRequest) o_DoShopRequest(shopItemId, count, callback, mi);
}

// ── [5b] HotFix.PetData.HasEnoughLevelUpCost — instance, 0 tham số, trả bool
//    Địa chỉ 0x27FF5E4: cùng address với HasEnoughStarUpCost (PetData)
//    và HasEnoughResonateCost (PetDataModule) → hook 1 cái là cover hết
bool (*o_HasEnoughLevelUpCost)(void*, void*) = nullptr;
bool h_HasEnoughLevelUpCost(void* self, void* mi) {
    T_COUNT(HasEnoughLevelUpCost);
    if (g_hasenough) return true;
    return o_HasEnoughLevelUpCost ? o_HasEnoughLevelUpCost(self, mi) : false;
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

    // [1] Vô hạn Coins — Proto.Common.UserCurrency
    T_HOOK("Proto.Common", "UserCurrency", "get_Coins", 0, getCoins);

    // [2] Vô hạn Kim Cương — Proto.Common.UserCurrency
    T_HOOK("Proto.Common", "UserCurrency", "get_Diamonds", 0, getDiamonds);

    // [3] Vô hạn Vật Phẩm — HotFix.PropDataModule
    T_HOOK("HotFix", "PropDataModule", "GetItemCount", 1, GetItemCount);
    T_HOOK("HotFix", "PropDataModule", "IsHaveItemDatas", 1, IsHaveItemDatas);

    // [4] Mở Khóa Shop — HotFix.DxxTools.UI (static)
    T_HOOK("HotFix.DxxTools", "UI", "IsCanShopBuy", 1, IsCanShopBuy);

    // [5] Bỏ Qua Chi Phí — cùng địa chỉ trong từng nhóm, hook 1 là đủ
    T_HOOK("HotFix", "GashaponDataModule", "HasEnoughLuck", 1, HasEnoughLuck);
    T_HOOK("HotFix", "PetData", "HasEnoughLevelUpCost", 0, HasEnoughLevelUpCost);

    // [6] Exploit Mythic Shop — intercept DoShopRequest, đổi count -> âm
    T_HOOK("HotFix.NetworkUtils", "Mythic", "DoShopRequest", 3, DoShopRequest);

    TraceReport();
    LOGI("=== Resolve DONE ===");
}

// ═══════════════════════════════════════════════════════════
//  8. MENU
// ═══════════════════════════════════════════════════════════
jobjectArray GetFeatureList(JNIEnv* env, jobject) {
    const char* features[] = {
        OBFUSCATE("Category_" GAME_NAME),
        OBFUSCATE("0_SeekBar_\xe2\x9a\xa1 Toc Do Game_1_10"),
        OBFUSCATE("1_Toggle_\xf0\x9f\x92\xb0 Vo Han Coins"),
        OBFUSCATE("2_Toggle_\xf0\x9f\x92\x8e Vo Han Kim Cuong"),
        OBFUSCATE("3_Toggle_\xf0\x9f\x93\xa6 Vo Han Vat Pham"),
        OBFUSCATE("4_Toggle_\xf0\x9f\x9b\x92 Mo Khoa Shop"),
        OBFUSCATE("5_Toggle_\xe2\x9c\x85 Bo Qua Chi Phi Nang Cap"),
        OBFUSCATE("6_Toggle_\xf0\x9f\x94\xa5 Exploit Mythic Shop"),
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
            g_speed = (value < 1) ? 1.0f : (float)value;
            ApplySpeed();
            break;
        case 1:
            g_coins = boolean;
            LOGI("Vo han Coins: %d", g_coins);
            break;
        case 2:
            g_diamonds = boolean;
            LOGI("Vo han Kim Cuong: %d", g_diamonds);
            break;
        case 3:
            g_items = boolean;
            LOGI("Vo han Vat Pham: %d", g_items);
            break;
        case 4:
            g_freeshop = boolean;
            LOGI("Mo Khoa Shop: %d", g_freeshop);
            break;
        case 5:
            g_hasenough = boolean;
            LOGI("Bo Qua Chi Phi: %d", g_hasenough);
            break;
        case 6:
            g_exploit_mythic = boolean;
            LOGI("Exploit Mythic Shop: %d", g_exploit_mythic);
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
