// ============================================================
//  ColorFit — com.lumosplay.colorfit.gp v2.9.0
//  Chức năng: Free Shop + Speed
//  Framework: LGL Test Loader (máy root, inject vào CH Play APK)
// ============================================================

#define LOG_TAG      "COLORFIT_MOD"
#define GAME_NAME    "🎨 Color Fit"
#define GAME_PACKAGE "com.lumosplay.colorfit.gp"
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
void*       (*il2cpp_string_new)(const char*)                              = nullptr;

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

template <typename T> static inline T& Field(void* obj, int off) {
    return *(T*)((uintptr_t)obj + off);
}

// ═══════════════════════════════════════════════════════════
//  4. TRẠNG THÁI CHỨC NĂNG
// ═══════════════════════════════════════════════════════════
static float g_speed    = 1.0f;   // [0] Speed (SeekBar)
static bool  g_freeShop = false;  // [1] Free Shop

// IAPManager cached methods (tất cả đều private, cần cache thủ công)
// dump: private void server_verify_success(string productsId, uint goodId,
//           string order_id, int payEnv, string transactionID, string purchaseReceipt)
typedef void (*t_server_verify_success)(void* self,
    void* productsId, uint32_t goodId, void* order_id, int32_t payEnv,
    void* transactionID, void* purchaseReceipt, void* mi);
static t_server_verify_success g_fn_serverVerify = nullptr;

// dump: private void client_request_success(string product, uint goodId)
typedef void (*t_client_request_success)(void* self, void* productId, uint32_t goodId, void* mi);
static t_client_request_success g_fn_clientSuccess = nullptr;

// dump: public void ReplenishPurchase(string productId)
typedef void (*t_ReplenishPurchase)(void* self, void* productId, void* mi);
static t_ReplenishPurchase g_fn_replenish = nullptr;

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
    if (!o_setTS) {
        LOGE("Speed KHONG chay: chua hook duoc Time.set_timeScale");
        return;
    }
    o_setTS(g_gameScale * g_speed, nullptr);
    LOGI("Speed x%.0f: game dat %.2f -> thuc te %.2f", g_speed, g_gameScale, g_gameScale * g_speed);
}

// ═══════════════════════════════════════════════════════════
//  6. HOOKS
// ═══════════════════════════════════════════════════════════

// BuyProduct — khi Free Shop bật: skip Google Pay, gọi server_verify_success trực tiếp
// (server_verify_success là response đã xác nhận từ server → trigger toàn bộ luồng cấp đồ)
// dump: public void BuyProduct(string productId, uint goodId) — instance, 2 params
void (*o_BuyProduct)(void*, void*, uint32_t, void*) = nullptr;
void h_BuyProduct(void* self, void* productId, uint32_t goodId, void* mi) {
    T_COUNT(BuyProduct);
    LOGI("BuyProduct: self=%p pid=%p goodId=%u freeShop=%d",
         self, productId, goodId, (int)g_freeShop);
    if (g_freeShop) {
        LOGI("FreeShop: chan BuyProduct goodId=%u", goodId);

        // Bước 1: server_verify_success = giả lập server đã xác nhận mua hàng
        // Đây là entry point cho toàn bộ flow cấp đồ nội bộ
        if (g_fn_serverVerify && self && productId && il2cpp_string_new) {
            void* fakeOrder = il2cpp_string_new("free_order_001");
            void* fakeTx    = il2cpp_string_new("GPA.free_transaction");
            void* fakeRcpt  = il2cpp_string_new("{}");
            LOGI("FreeShop: goi server_verify_success...");
            g_fn_serverVerify(self, productId, goodId, fakeOrder, 1, fakeTx, fakeRcpt, nullptr);
            LOGI("FreeShop: server_verify_success xong");
        }
        // Bước 2: gọi thêm client_request_success (nếu không chạy tự động từ bước 1)
        else if (g_fn_clientSuccess && self && productId) {
            LOGI("FreeShop: fallback - goi client_request_success...");
            g_fn_clientSuccess(self, productId, goodId, nullptr);
            LOGI("FreeShop: client_request_success xong");
        }
        else {
            LOGE("FreeShop: KHONG CO CACHE! verify=%p success=%p self=%p pid=%p il2str=%p",
                 (void*)g_fn_serverVerify, (void*)g_fn_clientSuccess,
                 self, productId, (void*)il2cpp_string_new);
        }

        // Bước 3: ReplenishPurchase — đảm bảo đồ được cộng vào tài khoản
        if (g_fn_replenish && self && productId) {
            LOGI("FreeShop: goi ReplenishPurchase...");
            g_fn_replenish(self, productId, nullptr);
            LOGI("FreeShop: ReplenishPurchase xong");
        }

        return;  // không gọi gốc = không hiện dialog Google Pay
    }
    if (o_BuyProduct) o_BuyProduct(self, productId, goodId, mi);
}

// IsFreePackageClaimble — hiển thị nút "gói miễn phí có thể nhận"
// dump: public bool IsFreePackageClaimble() — instance, 0 params
bool (*o_IsFreePackageClaimble)(void*, void*) = nullptr;
bool h_IsFreePackageClaimble(void* self, void* mi) {
    T_COUNT(IsFreePackageClaimble);
    if (g_freeShop) return true;
    return o_IsFreePackageClaimble ? o_IsFreePackageClaimble(self, mi) : false;
}

// IsFreeMaxCount — giới hạn nhận gói miễn phí chưa đạt (false = vẫn nhận được)
// dump: public bool IsFreeMaxCount() — instance, 0 params
bool (*o_IsFreeMaxCount)(void*, void*) = nullptr;
bool h_IsFreeMaxCount(void* self, void* mi) {
    T_COUNT(IsFreeMaxCount);
    if (g_freeShop) return false;
    return o_IsFreeMaxCount ? o_IsFreeMaxCount(self, mi) : true;
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

    // [1] Free Shop — hook BuyProduct để chặn dialog Google Pay
    T_HOOK("", "IAPManager", "BuyProduct", 2, BuyProduct);

    // Cache các method private của IAPManager
    void* iapKlass = FindClassAnywhere("IAPManager");
    if (iapKlass) {
        // server_verify_success: 6 params (string,uint,string,int,string,string)
        void* m = FindMethod(iapKlass, "server_verify_success", 6);
        if (m) {
            g_fn_serverVerify = (t_server_verify_success) MethodFnPtr(m);
            LOGI("server_verify_success cached: %p", (void*)g_fn_serverVerify);
        } else {
            LOGE("server_verify_success NOT FOUND (6 params)");
        }

        // client_request_success: 2 params (string, uint) — fallback
        m = FindMethod(iapKlass, "client_request_success", 2);
        if (m) {
            g_fn_clientSuccess = (t_client_request_success) MethodFnPtr(m);
            LOGI("client_request_success cached: %p", (void*)g_fn_clientSuccess);
        } else {
            LOGE("client_request_success NOT FOUND (2 params)");
        }

        // ReplenishPurchase: 1 param (string) — cấp đồ sau verify
        m = FindMethod(iapKlass, "ReplenishPurchase", 1);
        if (m) {
            g_fn_replenish = (t_ReplenishPurchase) MethodFnPtr(m);
            LOGI("ReplenishPurchase cached: %p", (void*)g_fn_replenish);
        } else {
            LOGE("ReplenishPurchase NOT FOUND (1 param)");
        }
    } else {
        LOGE("IAPManager class NOT FOUND");
    }

    // ShopGoodsLogic — UI hiện nút gói free
    T_HOOK("", "ShopGoodsLogic", "IsFreePackageClaimble", 0, IsFreePackageClaimble);
    T_HOOK("", "ShopGoodsLogic", "IsFreeMaxCount",        0, IsFreeMaxCount);

    TraceReport();
    LOGI("=== Resolve DONE ===");
    LOGI("Cache: serverVerify=%p clientSuccess=%p replenish=%p il2str=%p",
         (void*)g_fn_serverVerify, (void*)g_fn_clientSuccess,
         (void*)g_fn_replenish, (void*)il2cpp_string_new);
}

// ═══════════════════════════════════════════════════════════
//  8. MENU
// ═══════════════════════════════════════════════════════════
jobjectArray GetFeatureList(JNIEnv* env, jobject) {
    const char* features[] = {
        OBFUSCATE("Category_" GAME_NAME),
        OBFUSCATE("0_SeekBar_⚡ Tốc Độ Game_1_10"),
        OBFUSCATE("1_Toggle_\U0001f6cd Free Shop"),
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
        case 1: // Free Shop
            g_freeShop = (boolean != 0);
            LOGI("FreeShop: %s (g_freeShop=%d)", g_freeShop ? "BAT" : "TAT", (int)g_freeShop);
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
