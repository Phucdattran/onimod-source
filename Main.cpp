#include <pthread.h>
#include <jni.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#include <link.h> 
#include <fcntl.h>
#include <android/log.h> 

#include "Includes/obfuscate.h"
#include "Includes/Logger.h"
#include "Includes/Utils.h"
#include "Menu/Setup.h"
#include <Dobby/dobby.h>
#include "MenuStrings.h"
extern "C" void KhoiTao_Auto_IL2CPP();
#include "Includes/EngineVIP.h"

bool enable_free_shop = false;
float locked_speed = 1.0f;
bool trigger_speed_update = false;

uintptr_t g_il2cpp_base = 0;

// ==========================================
// OFFSET BẢN UPDATE MỚI NHẤT (v1.13.4448)
// ==========================================
#define OFF_BUY_PRODUCT         0x2959C60

#define OFF_GET_INSTANCE        0x2557B90
#define OFF_SEND                0x255A3BC
#define OFF_SET_PROGRESS        0x2558220
#define OFF_HANDLE_FAIL         0x25586EC

#define OFF_REPORT_GRANT        0x255D234
#define OFF_REFUSE_UNLOCK       0x255D538

#define OFF_ANTI_TRIGGER        0x2477160
#define OFF_ANTI_AUTOBAN        0x2479FF4
#define OFF_ANTI_HASANY         0x247A6D8

int dl_iterate_callback_base(struct dl_phdr_info *info, size_t size, void *data) {
    if (info->dlpi_name && strstr(info->dlpi_name, OBFUSCATE("libil2cpp.so"))) {
        g_il2cpp_base = info->dlpi_addr;
        if (data != nullptr) *(bool*)data = true;
        return 1;
    }
    return 0;
}

bool isIl2cppLoaded_Safe() {
    bool isLoaded = false;
    dl_iterate_phdr(dl_iterate_callback_base, &isLoaded);
    return isLoaded;
}

// Hàm vá mã Hex (Dùng để khóa mồm các hàm báo cáo của Game)
void oni_patch(uintptr_t offset, uint32_t ins) {
    if (g_il2cpp_base == 0) return;
    void* addr = (void*)(g_il2cpp_base + offset);
    uintptr_t page_start = (uintptr_t)addr & ~0xFFF;
    mprotect((void*)page_start, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC);
    *(uint32_t*)addr = ins;
    __builtin___clear_cache((char*)addr, (char*)addr + 4);
}

// ==========================================
// LỚP BẢO VỆ 1: MUA TRỰC TIẾP (DIRECT BUY)
// ==========================================
int (*o_BuyProductByID)(void*, void*, void*);
int h_BuyProductByID(void* _this, void* productId, void* methodInfo) {
    if (enable_free_shop && productId != nullptr && g_il2cpp_base != 0) {
        void* pm_inst = ((void* (*)())(g_il2cpp_base + OFF_GET_INSTANCE))();
        if (pm_inst != nullptr) {
            // Truyền 1 vào GrantSource (Hợp lệ), thay vì 0 như bản trước
            ((bool (*)(void*, void*, int, void*))(g_il2cpp_base + OFF_SEND))(pm_inst, productId, 1, nullptr);
            ((void (*)(void*, bool, void*))(g_il2cpp_base + OFF_SET_PROGRESS))(pm_inst, false, nullptr);
        }
        return 4; 
    }
    return o_BuyProductByID ? o_BuyProductByID(_this, productId, methodInfo) : 0;
}

// ==========================================
// LỚP BẢO VỆ 2: HỦY THANH TOÁN -> NHẬN ĐỒ (CANCEL TO BUY)
// ==========================================
void (*o_HandlePurchaseFailure)(void*, void*, int, void*);
void h_HandlePurchaseFailure(void* _this, void* id, int reason, void* methodInfo) {
    // Nếu bấm mua bị hiện bảng Google Play, cứ bấm nút Back (Hủy) là nó nhảy vào đây. Ta ép nó thành nhận đồ luôn!
    if (enable_free_shop && id != nullptr && g_il2cpp_base != 0) {
        ((bool (*)(void*, void*, int, void*))(g_il2cpp_base + OFF_SEND))(_this, id, 1, nullptr);
        ((void (*)(void*, bool, void*))(g_il2cpp_base + OFF_SET_PROGRESS))(_this, false, nullptr);
        return; // Chặn không cho game biết là mua thất bại
    }
    if (o_HandlePurchaseFailure) o_HandlePurchaseFailure(_this, id, reason, methodInfo);
}

// ==========================================
// BỊT MẮT HÀM KIỂM TRA HACK (LUÔN BÁO FALSE)
// ==========================================
bool (*o_HasAnyCheat)(void*, void*);
bool h_HasAnyCheat(void* _this, void* methodInfo) {
    return false; 
}

// ==========================================
// CÁC HÀM IL2CPP API & SPEED HACK
// ==========================================
void* (*il2cpp_domain_get)() = nullptr;
void* (*il2cpp_thread_attach)(void*) = nullptr;
void** (*il2cpp_domain_get_assemblies)(void*, size_t*) = nullptr;
void* (*il2cpp_assembly_get_image)(void*) = nullptr;
void* (*il2cpp_class_from_name)(void*, const char*, const char*) = nullptr;
void* (*il2cpp_class_get_methods)(void*, void**) = nullptr;
const char* (*il2cpp_method_get_name)(void*) = nullptr;
uint32_t (*il2cpp_method_get_param_count)(void*) = nullptr;

bool InitIl2cpp() {
    void* handle = dlopen(OBFUSCATE("libil2cpp.so"), RTLD_LAZY);
    if (!handle) handle = dlopen(OBFUSCATE("libil2cpp.so"), RTLD_NOLOAD);
    if (!handle) return false;
    
    il2cpp_domain_get = (void* (*)())dlsym(handle, OBFUSCATE("il2cpp_domain_get"));
    il2cpp_thread_attach = (void* (*)(void*))dlsym(handle, OBFUSCATE("il2cpp_thread_attach"));
    il2cpp_domain_get_assemblies = (void** (*)(void*, size_t*))dlsym(handle, OBFUSCATE("il2cpp_domain_get_assemblies"));
    il2cpp_assembly_get_image = (void* (*)(void*))dlsym(handle, OBFUSCATE("il2cpp_assembly_get_image"));
    il2cpp_class_from_name = (void* (*)(void*, const char*, const char*))dlsym(handle, OBFUSCATE("il2cpp_class_from_name"));
    il2cpp_class_get_methods = (void* (*)(void*, void**))dlsym(handle, OBFUSCATE("il2cpp_class_get_methods"));
    il2cpp_method_get_name = (const char* (*)(void*))dlsym(handle, OBFUSCATE("il2cpp_method_get_name"));
    il2cpp_method_get_param_count = (uint32_t (*)(void*))dlsym(handle, OBFUSCATE("il2cpp_method_get_param_count"));
    
    return (il2cpp_domain_get && il2cpp_method_get_param_count);
}

void* GetClass(const char* nameSpace, const char* className) {
    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies) return nullptr;
    void* domain = il2cpp_domain_get();
    if (!domain) return nullptr;
    size_t size = 0;
    void** assemblies = il2cpp_domain_get_assemblies(domain, &size);
    if (!assemblies || size == 0) return nullptr;
    for (size_t i = 0; i < size; ++i) {
        void* image = il2cpp_assembly_get_image(assemblies[i]);
        if (!image) continue;
        void* klass = il2cpp_class_from_name(image, nameSpace, className);
        if (klass) return klass;
    }
    return nullptr;
}

void (*o_set_timeScale)(float, void*);
void h_set_timeScale(float value, void* methodInfo) {
    if (locked_speed > 1.0f) {
        if (o_set_timeScale) o_set_timeScale(locked_speed, methodInfo);
        return;
    }
    if (o_set_timeScale) o_set_timeScale(value, methodInfo);
}

float (*o_get_deltaTime)(void*);
float h_get_deltaTime(void* methodInfo) {
    if (trigger_speed_update) {
        trigger_speed_update = false;
        if (o_set_timeScale) o_set_timeScale(locked_speed, nullptr);
    }
    return o_get_deltaTime ? o_get_deltaTime(methodInfo) : 0.016f;
}

void *h_t(void *) {
    Setup_Ngu_Dong_4s();
    
    while (!isIl2cppLoaded_Safe()) usleep(500000);

    // ==========================================
    // KHÓA MÕM CÁC HÀM ANTI-CHEAT VÀ MÁCH LẺO CỦA DEV
    // Dùng lệnh RET (0xD65F03C0) để hàm chạy xong lập tức quay về, không làm gì cả.
    // ==========================================
    oni_patch(OFF_ANTI_TRIGGER, 0xD65F03C0);  
    oni_patch(OFF_ANTI_AUTOBAN, 0xD65F03C0);  
    oni_patch(OFF_REPORT_GRANT, 0xD65F03C0); // Chặn cảnh báo nhận đồ trái phép 
    oni_patch(OFF_REFUSE_UNLOCK, 0xD65F03C0); // Chặn khóa đồ khi hack

    // Tiến hành Hook hàm
    if (g_il2cpp_base != 0) {
        DobbyHook((void*)(g_il2cpp_base + OFF_BUY_PRODUCT), (void*)h_BuyProductByID, (void**)&o_BuyProductByID);
        DobbyHook((void*)(g_il2cpp_base + OFF_HANDLE_FAIL), (void*)h_HandlePurchaseFailure, (void**)&o_HandlePurchaseFailure);
        DobbyHook((void*)(g_il2cpp_base + OFF_ANTI_HASANY), (void*)h_HasAnyCheat, (void**)&o_HasAnyCheat);
    }

    while (!InitIl2cpp()) usleep(500000);

    void* domain = nullptr;
    while (!domain) {
        domain = il2cpp_domain_get();
        usleep(100000);
    }
    il2cpp_thread_attach(domain);

    // Móc tốc độ bằng IL2CPP API
    void* klass_Time = GetClass(OBFUSCATE("UnityEngine"), OBFUSCATE("Time"));
    if (klass_Time) {
        void* iter = nullptr;
        while (void* method = il2cpp_class_get_methods(klass_Time, &iter)) {
            const char* name = il2cpp_method_get_name(method);
            if (strcmp(name, OBFUSCATE("set_timeScale")) == 0 && il2cpp_method_get_param_count(method) == 1) {
                DobbyHook(*(void**)method, (void*)h_set_timeScale, (void**)&o_set_timeScale);
            }
            if (strcmp(name, OBFUSCATE("get_deltaTime")) == 0 && il2cpp_method_get_param_count(method) == 0) {
                DobbyHook(*(void**)method, (void*)h_get_deltaTime, (void**)&o_get_deltaTime);
            }
        }
    }

    isGameReady = true;
    return NULL;
}

jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    const char *features[] = {
        OBFUSCATE("Category_🔥 Chiến Tuyến Tam Quốc"),
        OBFUSCATE("1_Toggle_🛒 BẬT FREE SHOP"),
        OBFUSCATE("2_SeekBar_⚡ SPEED _1_20")
    };
    int total = sizeof(features) / sizeof(features[0]);
    jobjectArray ret = env->NewObjectArray(total, env->FindClass("java/lang/String"), env->NewStringUTF(""));
    for (int i = 0; i < total; i++) {
        jstring str = env->NewStringUTF(features[i]);
        env->SetObjectArrayElement(ret, i, str);
        env->DeleteLocalRef(str);
    }
    return ret;
}

void Changes(JNIEnv *env, jclass clazz, jobject obj, jint featNum, jstring featName, jint value, jboolean boolean, jstring str) {
    if (featNum == 1) enable_free_shop = boolean;
    else if (featNum == 2) {
        locked_speed = (float)value;
        trigger_speed_update = true;
    }
}

__attribute__((constructor))
void l_m() {
    pthread_t ptid;
    pthread_create(&ptid, NULL, h_t, NULL);
}

#include "Includes/JNI_Boilerplate.h"
