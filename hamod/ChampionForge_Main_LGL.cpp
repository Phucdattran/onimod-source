// ============================================================
//  LGL MOD — com.mobibrain.championforge (v1.0.12)
//  Test qua LGL Test Loader (máy root), sau đó ghép APK.
//
//  Tính năng:
//    [0]  Free Shop  — mua đồ không tốn tiền thật
//
//  HOOKS (PatchMethodPointer, Android 15 MTE-safe + T_HOOK UNIQUE):
//    - GManager_IAP_Hw::BuyProduct(string)   → bỏ qua Google Pay,
//        gọi GPay.AddGoodsId(goodsId,1) qua il2cpp_runtime_invoke
//    - GPay::CountById(string)               → luôn trả 999 (UI owned)
//    - Kunpo_IAP::FallCondition(BuyProduct)  → false (an toàn)
//    - Kunpo_IAP::CancelCondition(BuyProduct)→ false (an toàn)
//    - Kunpo_IAP::SendAward()  [T_HOOK]      → log để debug
//
//  Cách dùng Free Shop:
//    1. Chờ Toast "LGL Mod: lib XX:XX:XX" hiện lên
//    2. Bật toggle "Free Shop" trong menu LGL
//    3. Bấm mua item trong shop
//    4. KHÔNG hiện Google Pay dialog — item được ghi thẳng vào client
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
#include <errno.h>
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
void*       (*il2cpp_runtime_invoke)(void*, void*, void**, void**)         = nullptr;
void*       (*il2cpp_string_new)(const char*)                              = nullptr;

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
    LOAD(il2cpp_runtime_invoke);   LOAD(il2cpp_string_new);
    #undef LOAD
    bool ok = il2cpp_domain_get && il2cpp_domain_get_assemblies &&
              il2cpp_assembly_get_image && il2cpp_class_from_name &&
              il2cpp_class_get_methods && il2cpp_method_get_name &&
              il2cpp_runtime_invoke;
    LOGI("InitIL2CppAPI: %s (invoke=%p)", ok ? "OK" : "FAIL", il2cpp_runtime_invoke);
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
template <typename T> static inline T& Field(void* obj, int off) {
    return *(T*)((uintptr_t)obj + off);
}
static bool DoHook(void* addr, void* hook, void** orig, const char* name) {
    if (!addr) { LOGE("Hook SKIP (NULL): %s", name); return false; }
    int r = DobbyHook(addr, hook, orig);
    if (r == 0) { LOGI("Hook OK: %s @ %p", name, addr); return true; }
    LOGE("Hook FAIL(%d): %s @ %p", r, name, addr); return false;
}

// PatchMethodPointer: ghi đè trực tiếp methodPointer trong MethodInfo struct.
// Không dùng Dobby trampoline → tránh MTE crash trên Android 15.
// Chỉ patch đúng MethodInfo đó, không ảnh hưởng các method dùng chung địa chỉ.
static bool PatchMethodPointer(void* method_info, void* new_fn, void** orig_out, const char* tag) {
    if (!method_info || !new_fn || !orig_out) { LOGE("PatchMethodPointer [%s]: NULL arg", tag); return false; }
    void** fn_field = (void**)method_info;
    *orig_out = *fn_field;
    if (!*orig_out) { LOGE("PatchMethodPointer [%s]: methodPointer = NULL", tag); return false; }
    if (*orig_out == new_fn) { LOGI("PatchMethodPointer [%s]: da patch roi", tag); return true; }
    uintptr_t page = (uintptr_t)fn_field & ~(uintptr_t)0xFFF;
    if (mprotect((void*)page, 0x1000, PROT_READ | PROT_WRITE) != 0) {
        LOGE("PatchMethodPointer [%s]: mprotect FAIL errno=%d", tag, errno); return false;
    }
    __atomic_store_n(fn_field, new_fn, __ATOMIC_SEQ_CST);
    mprotect((void*)page, 0x1000, PROT_READ);
    LOGI("PatchMethodPointer [%s] OK: orig=%p hook=%p", tag, *orig_out, new_fn);
    return true;
}

// ═══════════════════════════════════════════════════════════
//  4. TRẠNG THÁI CHỨC NĂNG
// ═══════════════════════════════════════════════════════════
static bool g_freeShop = false;  // [0] Free Shop

// MethodInfo của GPay.AddGoodsId — cache để gọi qua il2cpp_runtime_invoke
static void* g_mi_AddGoodsId = nullptr;

// ═══════════════════════════════════════════════════════════
//  5. HOOKS — Free Shop (Kunpo IAP Framework)
//
//  Chiến lược:
//    A. GManager_IAP_Hw::BuyProduct(string)  [SHARED STUB → PatchMethodPointer]
//       → Bỏ qua Google Pay, gọi GPay.AddGoodsId(goodsId,1)
//         qua il2cpp_runtime_invoke để cộng vào _shoppingList.
//
//    B. GPay::CountById(string)  [SHARED → PatchMethodPointer]
//       → Trả 999 để UI shop hiện "đã sở hữu".
//
//    C. Kunpo_IAP::FallCondition / CancelCondition  [UNIQUE → PatchMethodPointer]
//       → false để chặn cancel path nếu flow gốc bị trigger.
//
//    D. Kunpo_IAP::SendAward()  [UNIQUE 0x31CD9E8 → T_HOOK + log]
//       → Gọi bản gốc, chỉ log để xem khi nào được gọi.
// ═══════════════════════════════════════════════════════════

// --- A. GManager_IAP_Hw::BuyProduct(string goodsId) ---
// private System.Void BuyProduct(System.String goodsId); → SHARED STUB 0x11C74E4
// Chữ ký instance: (void* self, void* goodsIdStr, void* mi)
void (*o_IAPBuyProduct)(void*, void*, void*) = nullptr;
void h_IAPBuyProduct(void* self, void* goodsIdStr, void* mi) {
    T_COUNT(IAPBuyProduct);
    if (g_freeShop) {
        LOGI("FreeShop: BuyProduct intercepted goodsId=%p", goodsIdStr);
        if (g_mi_AddGoodsId && il2cpp_runtime_invoke && goodsIdStr) {
            void* exc = nullptr;
            int32_t count = 1;
            void* args[] = { goodsIdStr, (void*)&count };
            il2cpp_runtime_invoke(g_mi_AddGoodsId, nullptr, args, &exc);
            if (exc) LOGE("FreeShop: AddGoodsId threw exception");
            else LOGI("FreeShop: AddGoodsId OK");
        } else {
            LOGE("FreeShop: AddGoodsId invoke skip (mi=%p invoke=%p goodsId=%p)",
                 g_mi_AddGoodsId, il2cpp_runtime_invoke, goodsIdStr);
        }
        return;  // không gọi original → không hiện Google Pay
    }
    if (o_IAPBuyProduct) o_IAPBuyProduct(self, goodsIdStr, mi);
}

// --- B. GPay::CountById(string id) → trả về 999 khi Free Shop ---
// public static System.Int32 CountById(System.String id); → SHARED → PatchMethodPointer
// Chữ ký static: (void* idStr, void* mi)
int32_t (*o_CountById)(void*, void*) = nullptr;
int32_t h_CountById(void* idStr, void* mi) {
    T_COUNT(CountById);
    if (g_freeShop) return 999;
    return o_CountById ? o_CountById(idStr, mi) : 0;
}

// --- C. Kunpo_IAP::FallCondition(BuyProduct) → false ---
// UNIQUE address 0x31CDA2C → PatchMethodPointer an toàn
bool (*o_FallCondition)(void*, void*, void*) = nullptr;
bool h_FallCondition(void* self, void* buyProduct, void* mi) {
    T_COUNT(FallCondition);
    if (g_freeShop) { LOGI("FreeShop: FallCondition blocked"); return false; }
    return o_FallCondition ? o_FallCondition(self, buyProduct, mi) : false;
}

// --- C. Kunpo_IAP::CancelCondition(BuyProduct) → false ---
// UNIQUE address 0x31CDA34 → PatchMethodPointer an toàn
bool (*o_CancelCondition)(void*, void*, void*) = nullptr;
bool h_CancelCondition(void* self, void* buyProduct, void* mi) {
    T_COUNT(CancelCondition);
    if (g_freeShop) return false;
    return o_CancelCondition ? o_CancelCondition(self, buyProduct, mi) : false;
}

// --- D. Kunpo_IAP::SendAward() — log only, gọi bản gốc ---
// UNIQUE address 0x31CD9E8 → T_HOOK an toàn
void (*o_SendAward)(void*, void*) = nullptr;
void h_SendAward(void* self, void* mi) {
    T_COUNT(SendAward);
    LOGI("SendAward CALLED! self=%p (freeShop=%d)", self, (int)g_freeShop);
    if (o_SendAward) o_SendAward(self, mi);
}

// ═══════════════════════════════════════════════════════════
//  6. ResolveAll — patch tất cả hooks
// ═══════════════════════════════════════════════════════════
static void ResolveAll() {
    LOGI("=== ResolveAll START ===");

    // --- A. GManager_IAP_Hw::BuyProduct(1) → PatchMethodPointer ---
    {
        void* klass = FindClassAnywhere("GManager_IAP_Hw");
        if (klass) {
            void* mi = FindMethod(klass, "BuyProduct", 1);
            if (mi) PatchMethodPointer(mi, (void*)h_IAPBuyProduct, (void**)&o_IAPBuyProduct,
                                       "GManager_IAP_Hw::BuyProduct");
            else LOGE("GManager_IAP_Hw::BuyProduct(1) NOT FOUND");
        } else LOGE("GManager_IAP_Hw class NOT FOUND");
    }

    // --- GPay.AddGoodsId MethodInfo (để gọi qua il2cpp_runtime_invoke) ---
    {
        void* klass = FindClassAnywhere("GPay");
        if (klass) {
            g_mi_AddGoodsId = FindMethod(klass, "AddGoodsId", 2);
            if (g_mi_AddGoodsId)
                LOGI("GPay.AddGoodsId MethodInfo=%p fnPtr=%p", g_mi_AddGoodsId, *(void**)g_mi_AddGoodsId);
            else LOGE("GPay.AddGoodsId(2) NOT FOUND");
        } else LOGE("GPay class NOT FOUND");
    }

    // --- B. GPay::CountById(1) → PatchMethodPointer ---
    {
        void* klass = FindClassAnywhere("GPay");
        if (klass) {
            void* mi = FindMethod(klass, "CountById", 1);
            if (mi) PatchMethodPointer(mi, (void*)h_CountById, (void**)&o_CountById,
                                       "GPay::CountById");
            else LOGE("GPay::CountById(1) NOT FOUND");
        }
    }

    // --- C. Kunpo_IAP::FallCondition + CancelCondition → PatchMethodPointer ---
    {
        void* klass = GetClass("Kar98k.IAP.Kunpo", "Kunpo_IAP");
        if (klass) {
            void* m_fall = FindMethod(klass, "FallCondition", 1);
            if (m_fall) PatchMethodPointer(m_fall, (void*)h_FallCondition, (void**)&o_FallCondition,
                                           "Kunpo_IAP::FallCondition");
            else LOGE("Kunpo_IAP::FallCondition(1) NOT FOUND");

            void* m_cancel = FindMethod(klass, "CancelCondition", 1);
            if (m_cancel) PatchMethodPointer(m_cancel, (void*)h_CancelCondition, (void**)&o_CancelCondition,
                                             "Kunpo_IAP::CancelCondition");
            else LOGE("Kunpo_IAP::CancelCondition(1) NOT FOUND");
        } else LOGE("Kunpo_IAP class NOT FOUND");
    }

    // --- D. Kunpo_IAP::SendAward() → T_HOOK (UNIQUE address) ---
    T_HOOK("Kar98k.IAP.Kunpo", "Kunpo_IAP", "SendAward", 0, SendAward);

    TraceReport();
    LOGI("=== ResolveAll DONE ===");
}

// ═══════════════════════════════════════════════════════════
//  7. MENU — GetFeatureList + Changes
// ═══════════════════════════════════════════════════════════
jobjectArray GetFeatureList(JNIEnv* env, jobject) {
    const char* features[] = {
        OBFUSCATE("Category_" GAME_NAME),
        OBFUSCATE("0_Toggle_Free Shop"),
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
    T_STEP("bam tinh nang so %d", featNum);
    switch (featNum) {
        case 0:
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
    LOGI(".so injected (FreeShop: BuyProduct+AddGoodsId+CountById)");
    pthread_t ptid;
    pthread_create(&ptid, nullptr, h_t, nullptr);
}

#include "Includes/JNI_Boilerplate.h"
