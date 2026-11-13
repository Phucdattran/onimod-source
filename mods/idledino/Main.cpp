// ═══════════════════════════════════════════════════════════
// Idle Dino — LGL Mod v13
// Free Shop (12 entitlements) + Thanh kéo tài nguyên
// ═══════════════════════════════════════════════════════════
#define LOG_TAG      "IDLEDINO_MOD"
#define GAME_NAME    "Idle Dino"
#define GAME_PACKAGE "com.minhnguyen.idledino"
#include "Includes/LGLTrace.h"
#include "Includes/obfuscate.h"

#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <ctype.h>
#include <link.h>
#include <elf.h>
#include <jni.h>
#include <android/log.h>
#include <Dobby/dobby.h>
#include <atomic>
#include <cmath>

// ─── 1. Trạng thái mod ────────────────────────────────────────────────────
static bool              isGameReady   = false;
static std::atomic<bool> g_godot_found {false};
static uintptr_t         g_godot_base  = 0;

// ─── 2. Offset ────────────────────────────────────────────────────────────
#define GODOT_OFFSET_EMIT_SIGNAL  0x1235570
#define GODOT_OFFSET_REG_SINGLE   0x1234678

// ─── 3. FIX LGL 3.2 native methods ──────────────────────────────────────
extern "C" JNIEXPORT jstring JNICALL
Java_com_android_support_Menu_getMenuTitle(JNIEnv* env, jobject) {
    return env->NewStringUTF(GAME_NAME);
}
extern "C" JNIEXPORT jstring JNICALL
Java_com_android_support_Menu_getMenuSubtitle(JNIEnv* env, jobject) {
    return env->NewStringUTF("Free Shop + Tai Nguyen v13");
}
extern "C" JNIEXPORT jstring JNICALL
Java_com_android_support_Menu_getZaloText(JNIEnv* env, jobject) { return env->NewStringUTF(""); }
extern "C" JNIEXPORT jstring JNICALL
Java_com_android_support_Menu_getTeleText(JNIEnv* env, jobject) { return env->NewStringUTF(""); }
extern "C" JNIEXPORT jstring JNICALL
Java_com_android_support_Menu_getWebText(JNIEnv* env, jobject)  { return env->NewStringUTF(""); }
extern "C" JNIEXPORT jstring JNICALL
Java_com_android_support_Menu_getZaloLink(JNIEnv* env, jobject) { return env->NewStringUTF(""); }
extern "C" JNIEXPORT jstring JNICALL
Java_com_android_support_Menu_getTeleLink(JNIEnv* env, jobject) { return env->NewStringUTF(""); }
extern "C" JNIEXPORT jstring JNICALL
Java_com_android_support_Menu_getWebLink(JNIEnv* env, jobject)  { return env->NewStringUTF(""); }
extern "C" JNIEXPORT jboolean JNICALL
Java_com_android_support_Menu_VerifyKeyNative(JNIEnv*, jobject, jstring) { return JNI_TRUE; }

// ─── 4. Cache JNI classes ─────────────────────────────────────────────────
static jclass    g_boolClass  = nullptr;
static jmethodID g_boolCtor   = nullptr;
static jmethodID g_boolValueOf= nullptr;
static jclass    g_objClass   = nullptr;

static void CacheClasses(JNIEnv* env) {
    if (g_boolClass) return;
    jclass bc = env->FindClass("java/lang/Boolean");
    jclass oc = env->FindClass("java/lang/Object");
    if (!bc || !oc) return;
    g_boolClass  = (jclass)env->NewGlobalRef(bc);
    g_objClass   = (jclass)env->NewGlobalRef(oc);
    g_boolCtor   = env->GetMethodID(bc, "<init>", "(Z)V");
    g_boolValueOf= env->GetStaticMethodID(bc, "valueOf", "(Z)Ljava/lang/Boolean;");
    env->DeleteLocalRef(bc);
    env->DeleteLocalRef(oc);
}

// ─── 5. Fake purchase_result ──────────────────────────────────────────────
static void FakePurchaseSuccess(JNIEnv* env, jobjectArray params) {
    if (!params || env->GetArrayLength(params) < 1) return;
    jobject dict = env->GetObjectArrayElement(params, 0);
    if (!dict) return;
    jclass dictClass = env->GetObjectClass(dict);
    jmethodID getM = env->GetMethodID(dictClass, "get",
        "(Ljava/lang/Object;)Ljava/lang/Object;");
    bool isCancelled = false;
    if (getM) {
        jstring kCanc = env->NewStringUTF("cancelled");
        jobject vCanc = (jobject)env->CallObjectMethod(dict, getM, kCanc);
        env->DeleteLocalRef(kCanc);
        if (vCanc) {
            jclass vc = env->GetObjectClass(vCanc);
            jmethodID bv = env->GetMethodID(vc, "booleanValue", "()Z");
            if (bv) isCancelled = (bool)env->CallBooleanMethod(vCanc, bv);
            env->DeleteLocalRef(vc);
            env->DeleteLocalRef(vCanc);
        }
    }
    if (!isCancelled) { env->DeleteLocalRef(dictClass); env->DeleteLocalRef(dict); return; }
    jmethodID putM = env->GetMethodID(dictClass, "put",
        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    env->DeleteLocalRef(dictClass);
    if (!putM || !g_boolClass || !g_boolValueOf) { env->DeleteLocalRef(dict); return; }

    jstring k1 = env->NewStringUTF("cancelled");
    jobject vF = env->CallStaticObjectMethod(g_boolClass, g_boolValueOf, (jboolean)JNI_FALSE);
    jobject o1 = (jobject)env->CallObjectMethod(dict, putM, k1, vF);
    if (o1) env->DeleteLocalRef(o1);
    env->DeleteLocalRef(k1); env->DeleteLocalRef(vF);

    jstring k2 = env->NewStringUTF("transaction_id");
    jstring v2 = env->NewStringUTF("GPA.1234-5678-9012-34567");
    jobject o2 = (jobject)env->CallObjectMethod(dict, putM, k2, v2);
    if (o2) env->DeleteLocalRef(o2);
    env->DeleteLocalRef(k2); env->DeleteLocalRef(v2);

    jstring k3 = env->NewStringUTF("error");
    jstring v3 = env->NewStringUTF("");
    jobject o3 = (jobject)env->CallObjectMethod(dict, putM, k3, v3);
    if (o3) env->DeleteLocalRef(o3);
    env->DeleteLocalRef(k3); env->DeleteLocalRef(v3);

    LOGI("[FreeShop] purchase_result FAKED to SUCCESS");
    env->DeleteLocalRef(dict);
}

// ─── 6. Danh sách 12 entitlement ─────────────────────────────────────────
static const char* k_ents[] = {
    "remove_banner_ads", "remove_all_ads", "auto_upgrade", "more_dna",
    "unlimited_storage", "field_season_pass", "premium_set_q",
    "premium_set_qq", "premium_set_qqq", "starter_pack",
    "explorer_pack", "legend_pack", nullptr
};

// ─── 7. Tìm base libgodot_android.so ──────────────────────────────────────
static bool FindGodotBase() {
    bool found = false;
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* data) -> int {
        if (!info->dlpi_name) return 0;
        const char* leaf = strrchr(info->dlpi_name, '/');
        leaf = leaf ? leaf + 1 : info->dlpi_name;
        if (strcmp(leaf, "libgodot_android.so") != 0) return 0;
        g_godot_base = (uintptr_t)info->dlpi_addr;
        *static_cast<bool*>(data) = true;
        return 1;
    }, &found);
    return found;
}

// ─── 8. Hook: nativeEmitSignal ────────────────────────────────────────────
typedef void (*t_EmitSignal)(JNIEnv*, jclass, jlong, jstring, jobjectArray);
static t_EmitSignal o_EmitSignal = nullptr;
static jlong g_plugin_obj_id = 0;

static void ReInjectAll(JNIEnv* env, jclass clazz) {
    if (!o_EmitSignal || !g_boolClass || !g_boolCtor || !g_objClass) return;
    if (!g_plugin_obj_id) return;
    jstring sig = env->NewStringUTF("entitlement");
    for (int i = 0; k_ents[i]; i++) {
        jobjectArray params = env->NewObjectArray(2, g_objClass, nullptr);
        jstring name = env->NewStringUTF(k_ents[i]);
        jobject tObj = env->NewObject(g_boolClass, g_boolCtor, (jboolean)JNI_TRUE);
        env->SetObjectArrayElement(params, 0, name);
        env->SetObjectArrayElement(params, 1, tObj);
        o_EmitSignal(env, clazz, g_plugin_obj_id, sig, params);
        env->DeleteLocalRef(name);
        env->DeleteLocalRef(tObj);
        env->DeleteLocalRef(params);
    }
    env->DeleteLocalRef(sig);
}

static void h_EmitSignal(JNIEnv* env, jclass clazz,
                         jlong objId, jstring signalName, jobjectArray params) {
    T_COUNT(EmitSignal);
    if (!env || !signalName) {
        if (o_EmitSignal) o_EmitSignal(env, clazz, objId, signalName, params);
        return;
    }
    const char* sig = env->GetStringUTFChars(signalName, nullptr);
    if (!sig) {
        if (o_EmitSignal) o_EmitSignal(env, clazz, objId, signalName, params);
        return;
    }
    g_plugin_obj_id = objId;
    CacheClasses(env);

    if (strcmp(sig, "entitlement") == 0 && params && env->GetArrayLength(params) >= 2) {
        if (g_boolClass && g_boolCtor) {
            jobject trueObj = env->NewObject(g_boolClass, g_boolCtor, (jboolean)JNI_TRUE);
            if (trueObj) { env->SetObjectArrayElement(params, 1, trueObj); env->DeleteLocalRef(trueObj); }
        }
    }
    if (strcmp(sig, "purchase_result") == 0)
        FakePurchaseSuccess(env, params);

    bool doReInject = strcmp(sig, "customer_info") == 0 ||
                      strcmp(sig, "purchases_error") == 0 ||
                      strcmp(sig, "purchase_error") == 0;

    env->ReleaseStringUTFChars(signalName, sig);
    if (o_EmitSignal) o_EmitSignal(env, clazz, objId, signalName, params);
    if (doReInject) ReInjectAll(env, clazz);
}

// ─── 9. Hook: nativeRegisterSingleton ────────────────────────────────────
typedef void (*t_RegSingle)(JNIEnv*, jclass, jlong, jstring);
static t_RegSingle o_RegSingle = nullptr;

static void h_RegSingle(JNIEnv* env, jclass clazz, jlong objId, jstring pluginName) {
    T_COUNT(RegSingle);
    const char* name = nullptr;
    if (pluginName && env) name = env->GetStringUTFChars(pluginName, nullptr);
    LOGI("[RegSingleton] id=%" PRId64 " plugin='%s'", (int64_t)objId, name ? name : "(null)");
    if (name && pluginName) env->ReleaseStringUTFChars(pluginName, name);
    if (o_RegSingle) o_RegSingle(env, clazz, objId, pluginName);
}

// ─── 10. Tài nguyên — Toggle preset + áp dụng ───────────────────────────
#define SAVE_PATH "/data/data/com.minhnguyen.idledino/files/idle_dino_farm.save"
#define BAK_PATH  "/data/data/com.minhnguyen.idledino/files/idle_dino_farm.save.bak"

#include <time.h>

static long g_set_dna      = 0;
static long g_set_amber    = 0;
static long g_set_meat     = 0;
static long long g_last_apply = 0;

// Thay "key":SỐ bằng newval trong buffer JSON
static bool json_set_num(char* buf, long cap, const char* key, const char* newval) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    char* p = strstr(buf, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ') p++;
    char* end = p;
    if (*end == '-') end++;
    while (*end && (isdigit((unsigned char)*end) || *end == '.' ||
                    *end == 'e' || *end == 'E')) end++;

    long new_len = (long)strlen(newval);
    long rest    = (long)strlen(end);
    if ((p - buf) + new_len + rest + 1 > cap) return false;

    memmove(p + new_len, end, rest + 1);
    memcpy(p, newval, new_len);
    return true;
}

static void ApplyResources() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long now = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
    if (now - g_last_apply < 1000) { LOGI("[TaiNguyen] debounce skip"); return; }
    g_last_apply = now;

    if (g_set_dna == 0 && g_set_amber == 0 && g_set_meat == 0) {
        LOGE("[TaiNguyen] Tat ca = 0 — bat 1 toggle truoc roi bam Ap dung");
        return;
    }

    FILE* f = fopen(SAVE_PATH, "rb");
    if (!f) { LOGE("[TaiNguyen] Can't open save file"); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    long cap = sz + 256;
    char* buf = (char*)malloc(cap);
    if (!buf) { fclose(f); LOGE("[TaiNguyen] malloc fail"); return; }
    sz = (long)fread(buf, 1, sz, f);
    buf[sz] = 0;
    fclose(f);

    char dna_str[32], amber_str[32], meat_str[64];
    bool ok1 = false, ok2 = false, ok3 = false;
    if (g_set_dna > 0) {
        snprintf(dna_str, sizeof(dna_str), "%ld", g_set_dna);
        ok1 = json_set_num(buf, cap, "dna", dna_str);
    }
    if (g_set_amber > 0) {
        snprintf(amber_str, sizeof(amber_str), "%ld", g_set_amber);
        ok2 = json_set_num(buf, cap, "amber", amber_str);
    }
    if (g_set_meat > 0) {
        snprintf(meat_str, sizeof(meat_str), "%ld.0", g_set_meat);
        ok3 = json_set_num(buf, cap, "meat", meat_str);
    }
    LOGI("[TaiNguyen] da set: dna=%ld ok=%d | amber=%ld ok=%d | meat=%ld ok=%d",
         g_set_dna, ok1, g_set_amber, ok2, g_set_meat, ok3);

    if (!ok1 && !ok2 && !ok3) {
        LOGE("[TaiNguyen] Ko tim thay key — format save thay doi?");
        free(buf); return;
    }

    long wlen = (long)strlen(buf);
    f = fopen(SAVE_PATH, "wb");
    if (!f) { free(buf); LOGE("[TaiNguyen] Can't write save"); return; }
    fwrite(buf, 1, wlen, f);
    fclose(f);

    f = fopen(BAK_PATH, "wb");
    if (f) { fwrite(buf, 1, wlen, f); fclose(f); }

    free(buf);
    LOGI("[TaiNguyen] XONG! Tat game va mo lai de ap dung.");
}

// ─── 11. ResolveAll ──────────────────────────────────────────────────────
static void ResolveAll() {
    T_STEP("ResolveAll");
    void* a_emit = (void*)(g_godot_base + GODOT_OFFSET_EMIT_SIGNAL);
    LOGI("Hook nativeEmitSignal @ %p", a_emit);
    if (DobbyHook(a_emit, (void*)h_EmitSignal, (void**)&o_EmitSignal) == 0)
        LOGI("nativeEmitSignal: OK");
    else
        LOGE("nativeEmitSignal: FAIL");

    void* a_reg = (void*)(g_godot_base + GODOT_OFFSET_REG_SINGLE);
    if (DobbyHook(a_reg, (void*)h_RegSingle, (void**)&o_RegSingle) == 0)
        LOGI("nativeRegisterSingleton: OK");
    else
        LOGE("nativeRegisterSingleton: FAIL");

    TraceReport();
}

// ─── 12. Menu ────────────────────────────────────────────────────────────
static jobjectArray GetFeatureList(JNIEnv* env, jclass) {
    // Toggle chon gia tri -> bat la ghi, cai sau de len cai truoc
    const char* items[] = {
        "Category_Free Shop",
        "0_Toggle_Free Shop (Luon bat)",
        "Category_DNA (bat 1 gia tri)",
        "1_Toggle_DNA 1 trieu",
        "2_Toggle_DNA 5 trieu",
        "3_Toggle_DNA 9 trieu",
        "Category_Amber (bat 1 gia tri)",
        "4_Toggle_Amber 5000",
        "5_Toggle_Amber 50000",
        "6_Toggle_Amber 999999",
        "Category_Thit (bat 1 gia tri)",
        "7_Toggle_Thit 1 trieu",
        "8_Toggle_Thit 5 trieu",
        "9_Toggle_Thit 9 trieu",
        "10_Button_Ap dung Save (tat mo lai)"
    };
    const int N = 15;
    jclass strClass = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray(N, strClass, nullptr);
    for (int i = 0; i < N; i++) {
        jstring s = env->NewStringUTF(items[i]);
        env->SetObjectArrayElement(arr, i, s);
        env->DeleteLocalRef(s);
    }
    env->DeleteLocalRef(strClass);
    return arr;
}

static void Changes(JNIEnv*, jclass, jobject, jint featNum, jstring, jfloat, jboolean enabled, jstring) {
    switch (featNum) {
        // DNA — bat toggle nao thi ghi gia tri do
        case 1: if (enabled) { g_set_dna = 1000000L; LOGI("[TaiNguyen] DNA -> 1 trieu"); } break;
        case 2: if (enabled) { g_set_dna = 5000000L; LOGI("[TaiNguyen] DNA -> 5 trieu"); } break;
        case 3: if (enabled) { g_set_dna = 9000000L; LOGI("[TaiNguyen] DNA -> 9 trieu"); } break;
        // Amber
        case 4: if (enabled) { g_set_amber =   5000L; LOGI("[TaiNguyen] Amber -> 5000"); } break;
        case 5: if (enabled) { g_set_amber =  50000L; LOGI("[TaiNguyen] Amber -> 50000"); } break;
        case 6: if (enabled) { g_set_amber = 999999L; LOGI("[TaiNguyen] Amber -> 999999"); } break;
        // Thit
        case 7: if (enabled) { g_set_meat = 1000000L; LOGI("[TaiNguyen] Thit -> 1 trieu"); } break;
        case 8: if (enabled) { g_set_meat = 5000000L; LOGI("[TaiNguyen] Thit -> 5 trieu"); } break;
        case 9: if (enabled) { g_set_meat = 9000000L; LOGI("[TaiNguyen] Thit -> 9 trieu"); } break;
        // Ap dung
        case 10:
            LOGI("[TaiNguyen] Ap dung: dna=%ld amber=%ld meat=%ld",
                 g_set_dna, g_set_amber, g_set_meat);
            ApplyResources();
            break;
    }
}

static void     CheckOverlayPermission(JNIEnv*, jclass, jobject) {}
static void     Init(JNIEnv*, jclass)                            {}
static jboolean my_isGameLibLoaded(JNIEnv*, jclass) {
    return isGameReady ? JNI_TRUE : JNI_FALSE;
}

// ─── 13. Khởi tạo ────────────────────────────────────────────────────────
static void* mod_thread(void*) {
    T_STEP("wait libgodot_android.so");
    LOGI("=== IDLE DINO MOD v13 START ===");
    for (int i = 0; i < 300; i++) {
        if (FindGodotBase()) { g_godot_found = true; break; }
        usleep(100000);
    }
    if (!g_godot_found) { LOGE("libgodot_android.so NOT FOUND!"); return nullptr; }
    LOGI("libgodot_android.so base = 0x%" PRIxPTR, g_godot_base);
    T_STEP("wait godot init");
    TraceSleep(3);
    ResolveAll();
    isGameReady = true;
    LOGI("=== MOD READY ===");
    TraceStartCallReports();
    return nullptr;
}

__attribute__((constructor))
static void lib_main() {
    LOGI("lib_main: Idle Dino Mod v13 loaded");
    pthread_t t;
    pthread_create(&t, nullptr, mod_thread, nullptr);
    pthread_detach(t);
}

#include "Menu/Menu.h"
#include "Includes/JNI_Boilerplate.h"
