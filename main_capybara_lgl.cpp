// ============================================================
//  LGL MOD — com.yibu.CapybaraSurvivorGOGOGO v10.0.1
//  Chuc nang: Speed (1-10x) + Dame xN + Free Shop
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

#define LOG_TAG   "CAPYBARA_MOD"
#define GAME_NAME "Capybara Survivor"
#define GAME_PACKAGE "com.yibu.CapybaraSurvivorGOGOGO"

#include "Includes/LGLTrace.h"

// 1. IL2CPP API
uintptr_t il2cpp_base = 0;
void*       (*il2cpp_domain_get)()                                          = nullptr;
void*       (*il2cpp_thread_attach)(void*)                                  = nullptr;
void**      (*il2cpp_domain_get_assemblies)(void*, size_t*)                 = nullptr;
void*       (*il2cpp_assembly_get_image)(void*)                             = nullptr;
size_t      (*il2cpp_image_get_class_count)(void*)                          = nullptr;
void*       (*il2cpp_image_get_class)(void*, size_t)                        = nullptr;
void*       (*il2cpp_class_from_name)(void*, const char*, const char*)      = nullptr;
const char* (*il2cpp_class_get_name)(void*)                                 = nullptr;
void*       (*il2cpp_class_get_methods)(void*, void**)                      = nullptr;
const char* (*il2cpp_method_get_name)(void*)                                = nullptr;
uint32_t    (*il2cpp_method_get_param_count)(void*)                         = nullptr;
void*       (*il2cpp_object_get_class)(void*)                               = nullptr;
void*       (*il2cpp_runtime_invoke)(void*, void*, void**, void**)           = nullptr;

// 2. ELF SYMBOL + IL2CPP LOADER
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
    LOAD(il2cpp_domain_get); LOAD(il2cpp_thread_attach);
    LOAD(il2cpp_domain_get_assemblies); LOAD(il2cpp_assembly_get_image);
    LOAD(il2cpp_image_get_class_count); LOAD(il2cpp_image_get_class);
    LOAD(il2cpp_class_from_name); LOAD(il2cpp_class_get_name);
    LOAD(il2cpp_class_get_methods); LOAD(il2cpp_method_get_name);
    LOAD(il2cpp_method_get_param_count);
    LOAD(il2cpp_object_get_class); LOAD(il2cpp_runtime_invoke);
    #undef LOAD
    bool ok = il2cpp_domain_get && il2cpp_domain_get_assemblies && il2cpp_assembly_get_image &&
              il2cpp_class_from_name && il2cpp_class_get_methods && il2cpp_method_get_name;
    LOGI("InitIL2CppAPI: %s", ok ? "OK" : "FAIL");
    return ok;
}

int dl_cb(struct dl_phdr_info* info, size_t, void* data) {
    if (info->dlpi_name && strstr(info->dlpi_name, "libil2cpp.so")) {
        il2cpp_base = info->dlpi_addr; g_il2cpp_phdr = info->dlpi_phdr;
        g_il2cpp_phnum = info->dlpi_phnum; *(bool*)data = true; return 1;
    }
    return 0;
}
bool IsIL2CppLoaded() { bool f=false; dl_iterate_phdr(dl_cb,&f); return f; }

// 3. HELPERS
static inline void* MethodFnPtr(void* mi) { return mi ? *(void**)mi : nullptr; }

void* FindClass(const char* ns, const char* name) {
    size_t n=0; void** list=il2cpp_domain_get_assemblies(il2cpp_domain_get(),&n);
    for (size_t i=0;i<n;i++){void* img=il2cpp_assembly_get_image(list[i]);if(!img)continue;if(void* k=il2cpp_class_from_name(img,ns,name))return k;}
    return nullptr;
}

void* FindClassAnywhere(const char* name) {
    size_t n=0; void** list=il2cpp_domain_get_assemblies(il2cpp_domain_get(),&n);
    for (size_t i=0;i<n;i++){
        void* img=il2cpp_assembly_get_image(list[i]); if(!img)continue;
        if(il2cpp_image_get_class_count&&il2cpp_image_get_class&&il2cpp_class_get_name){
            size_t nc=il2cpp_image_get_class_count(img);
            for(size_t j=0;j<nc;j++){void* k=il2cpp_image_get_class(img,j);const char* kn=k?il2cpp_class_get_name(k):nullptr;if(kn&&strcmp(kn,name)==0)return k;}
        } else if(void* k=il2cpp_class_from_name(img,"",name))return k;
    }
    return nullptr;
}

void* FindMethod(void* klass, const char* name, int nParams) {
    if(!klass)return nullptr; void* iter=nullptr;
    while(void* m=il2cpp_class_get_methods(klass,&iter)){
        const char* n=il2cpp_method_get_name(m); if(!n||strcmp(n,name)!=0)continue;
        if(nParams>=0&&il2cpp_method_get_param_count&&(int)il2cpp_method_get_param_count(m)!=nParams)continue;
        return m;
    }
    return nullptr;
}

static void* GetClass(const char* ns, const char* cls) {
    return (ns&&*ns)?FindClass(ns,cls):FindClassAnywhere(cls);
}

void* MethodAddr(const char* ns, const char* cls, const char* method, int nParams) {
    void* k=GetClass(ns,cls); if(!k){LOGE("NOT FOUND class: %s.%s",ns,cls);return nullptr;}
    void* m=FindMethod(k,method,nParams); if(!m){LOGE("NOT FOUND: %s.%s(%d)",cls,method,nParams);return nullptr;}
    return MethodFnPtr(m);
}

static bool DoHook(void* addr, void* hook, void** orig, const char* name) {
    if(!addr){LOGE("Hook SKIP(NULL): %s",name);return false;}
    int r=DobbyHook(addr,hook,orig);
    if(r==0){LOGI("Hook OK: %s @ %p",name,addr);return true;}
    LOGE("Hook FAIL(%d): %s @ %p",r,name,addr); return false;
}

template <typename T> static inline T& Field(void* obj, int off) {
    return *(T*)((uintptr_t)obj+off);
}

// 4. TRANG THAI
static float g_speed     = 1.0f;
static int   g_dmg_mult  = 1;    // 1=off, 5/10/20=nhan sat thuong player
static bool  g_free_shop = false;

// 5. SPEED — UnityEngine.Time
// get_timeScale @ 0x4827FDC | set_timeScale @ 0x4828004
void  (*o_setTS)(float,void*) = nullptr;
float (*o_getTS)(void*)       = nullptr;
static float g_gameScale = 1.0f;

void h_setTS(float v,void* mi){T_COUNT(setTS);g_gameScale=v;if(o_setTS)o_setTS(v*g_speed,mi);}
float h_getTS(void* mi){T_COUNT(getTS);if(g_speed!=1.0f)return g_gameScale;return o_getTS?o_getTS(mi):g_gameScale;}
static void ApplySpeed(){
    if(!o_setTS){LOGE("Speed: chua hook Time.set_timeScale");return;}
    o_setTS(g_gameScale*g_speed,nullptr);
    LOGI("Speed x%.0f applied",g_speed);
}

// 6. DAME — Fomula class (attack-side, nhan sat thuong player+dong-minh)
// AtkPack: damage @ 0x10 (Int64), isCritical @ 0x18 (bool)
// Fomula static class (ns="") cac public function tra ve AtkPack:
//   GetPlayerAtk(attackRate)              @ 0x240DE88  — normal attack player
//   GetPlayerSkill(skillRate)             @ 0x240EC74  — skill player
//   GetPlayerSword()                      @ 0x240EDD8  — sword player
//   GetPlayerLightning()                  @ 0x240EF24  — lightning player
//   GetFriendAtk(id,atkTrans,bp,rate)    @ 0x240E2D0  — normal dong-minh
//   GetFriendSkill(rate,id,at,bp,crit)   @ 0x240E5BC  — skill dong-minh

typedef void* (*t_GetAtk1)(int32_t, void*);
typedef void* (*t_GetAtk0)(void*);
typedef void* (*t_GetFriendAtk4)(int32_t, int32_t, int32_t, int32_t, void*);
typedef void* (*t_GetFriendSkill5)(int32_t, int32_t, int32_t, int32_t, bool, void*);

static t_GetAtk1       o_GetPlayerAtk      = nullptr;
static t_GetAtk1       o_GetPlayerSkill    = nullptr;
static t_GetAtk0       o_GetPlayerSword    = nullptr;
static t_GetAtk0       o_GetPlayerLightning = nullptr;
static t_GetFriendAtk4  o_GetFriendAtk     = nullptr;
static t_GetFriendSkill5 o_GetFriendSkill  = nullptr;

static inline void MultDmg(void* pack) {
    if (!pack || g_dmg_mult <= 1) return;
    Field<int64_t>(pack, 0x10) *= g_dmg_mult;
}

void* h_GetPlayerAtk(int32_t r, void* mi)      { T_COUNT(GetPlayerAtk);      void* p=o_GetPlayerAtk(r,mi);            MultDmg(p); return p; }
void* h_GetPlayerSkill(int32_t r, void* mi)    { T_COUNT(GetPlayerSkill);    void* p=o_GetPlayerSkill(r,mi);           MultDmg(p); return p; }
void* h_GetPlayerSword(void* mi)               { T_COUNT(GetPlayerSword);     void* p=o_GetPlayerSword(mi);             MultDmg(p); return p; }
void* h_GetPlayerLightning(void* mi)           { T_COUNT(GetPlayerLightning); void* p=o_GetPlayerLightning(mi);         MultDmg(p); return p; }
void* h_GetFriendAtk(int32_t id, int32_t at, int32_t bp, int32_t ar, void* mi) {
    T_COUNT(GetFriendAtk);  void* p=o_GetFriendAtk(id,at,bp,ar,mi); MultDmg(p); return p;
}
void* h_GetFriendSkill(int32_t sr, int32_t id, int32_t at, int32_t bp, bool crit, void* mi) {
    T_COUNT(GetFriendSkill); void* p=o_GetFriendSkill(sr,id,at,bp,crit,mi); MultDmg(p); return p;
}

// 7. FREE SHOP — PayManager.PayAct
// PayManager.PayAct(productId, priceId, yesAction) [3 params] mo Google Play dialog
// PayManager.GrantProductRewardById(productId, out payAmount) [2 params, private] cap do
// Hook PayAct: khi FreeShop ON, bo qua Google Play, goi GrantProductRewardById truc tiep
typedef void (*t_PayAct)(void* self, void* productId, int32_t priceId, void* yesAction, void* mi);
typedef bool (*t_GrantRewardById)(void* self, void* productId, float* payAmount, void* mi);

static t_PayAct          o_PayAct          = nullptr;
static t_GrantRewardById g_GrantRewardById  = nullptr;

void h_PayAct(void* self, void* productId, int32_t priceId, void* yesAction, void* mi) {
    T_COUNT(PayAct);
    if (!g_free_shop || !self) {
        if (o_PayAct) o_PayAct(self, productId, priceId, yesAction, mi);
        return;
    }
    float payAmount = 0.0f;
    bool ok = g_GrantRewardById ? g_GrantRewardById(self, productId, &payAmount, nullptr) : false;
    LOGI("FreeShop: GrantRewardById -> %s amount=%.2f", ok?"OK":"FAIL", payAmount);
    // Invoke yesAction (UI refresh callback)
    if (yesAction && il2cpp_object_get_class && il2cpp_runtime_invoke) {
        void* cls = il2cpp_object_get_class(yesAction);
        void* invM = cls ? FindMethod(cls, "Invoke", 0) : nullptr;
        if (invM) {
            void* exc = nullptr;
            il2cpp_runtime_invoke(invM, yesAction, nullptr, &exc);
        }
    }
}

// 9. GAN HOOK
static void ResolveAll(){
    LOGI("=== Resolve START ===");
    T_HOOK("UnityEngine","Time","get_timeScale",0,getTS);
    T_HOOK("UnityEngine","Time","set_timeScale",1,setTS);
    if(o_getTS) g_gameScale=o_getTS(nullptr);
    // Dame — Fomula attack-side multiplier
    T_HOOK("","Fomula","GetPlayerAtk",1,GetPlayerAtk);
    T_HOOK("","Fomula","GetPlayerSkill",1,GetPlayerSkill);
    T_HOOK("","Fomula","GetPlayerSword",0,GetPlayerSword);
    T_HOOK("","Fomula","GetPlayerLightning",0,GetPlayerLightning);
    T_HOOK("","Fomula","GetFriendAtk",4,GetFriendAtk);
    T_HOOK("","Fomula","GetFriendSkill",5,GetFriendSkill);
    // Free Shop: hook PayManager.PayAct (nguon goc tat ca IAP)
    {void* k=FindClassAnywhere("PayManager");
     if(k){void* m=FindMethod(k,"GrantProductRewardById",2);if(m)g_GrantRewardById=(t_GrantRewardById)MethodFnPtr(m);LOGI("GrantRewardById: %p",(void*)g_GrantRewardById);}
     T_HOOK("","PayManager","PayAct",3,PayAct);}
    TraceReport();
    LOGI("=== Resolve DONE ===");
}

// 10. MENU
jobjectArray GetFeatureList(JNIEnv* env,jobject){
    const char* features[]={
        OBFUSCATE("Category_" GAME_NAME),
        OBFUSCATE("0_SeekBar_\xe2\x9a\xa1 Toc Do Game_1_10"),
        OBFUSCATE("1_Spinner_\xe2\x9a\x94 Dame_Off,x5,x10,x20"),
        OBFUSCATE("2_Toggle_\xf0\x9f\x9b\x92 Free Shop"),
    };
    int total=sizeof(features)/sizeof(features[0]);
    jobjectArray ret=env->NewObjectArray(total,env->FindClass("java/lang/String"),env->NewStringUTF(""));
    for(int i=0;i<total;i++){jstring s=env->NewStringUTF(features[i]);env->SetObjectArrayElement(ret,i,s);env->DeleteLocalRef(s);}
    return ret;
}

void Changes(JNIEnv* env,jclass,jobject,jint featNum,jstring featName,jint value,jboolean boolean,jstring str){
    T_STEP("bam tinh nang %d (val=%d bat=%d)",featNum,value,boolean);
    switch(featNum){
        case 0: g_speed=(value<1)?1.0f:(float)value; ApplySpeed(); break;
        case 1: { static const int mults[]={1,5,10,20}; g_dmg_mult=(value>=0&&value<4)?mults[value]:1; LOGI("Dame: x%d",g_dmg_mult); break; }
        case 2: g_free_shop=boolean; LOGI("FreeShop: %s",g_free_shop?"BAT":"TAT"); break;
    }
}

// 11. KHOI TAO
static volatile bool g_il2cpp_ready=false;
static int (*o_il2cpp_init)(const char*)=nullptr;
static int h_il2cpp_init(const char* domain_name){
    int r=o_il2cpp_init?o_il2cpp_init(domain_name):0; g_il2cpp_ready=true; return r;
}

void* h_t(void*){
    T_STEP("cho libil2cpp.so");
    while(!IsIL2CppLoaded())usleep(100000);
    LOGI("libil2cpp.so base: 0x%" PRIxPTR,il2cpp_base);
    if(!InitIL2CppAPI()){LOGE("InitIL2CppAPI FAIL - dung");return nullptr;}
    T_STEP("hook il2cpp_init");
    void* initAddr=ElfSym("il2cpp_init");
    bool hooked=initAddr&&DobbyHook(initAddr,(void*)h_il2cpp_init,(void**)&o_il2cpp_init)==0;
    LOGI("Hook il2cpp_init: %s",hooked?"OK":"FAIL");
    T_STEP("cho il2cpp_init");
    for(int i=0;i<300&&!g_il2cpp_ready;i++)usleep(100000);
    if(g_il2cpp_ready)LOGI("il2cpp_init xong"); else LOGE("Khong thay il2cpp_init -> cho them 5s");
    for(int i=0;i<20;i++)usleep(250000);
    T_STEP("attach + cho assemblies");
    void* domain=il2cpp_domain_get();
    if(domain&&il2cpp_thread_attach)il2cpp_thread_attach(domain);
    {size_t last=0;int stable=0;
     while(stable<5){size_t n=0;il2cpp_domain_get_assemblies(domain,&n);
       if(n>0&&n==last)stable++;else{last=n;stable=0;}usleep(100000);}
     LOGI("Assemblies on dinh: %zu",last);}
    T_STEP("ResolveAll");
    ResolveAll();
    isGameReady=true;
    LOGI("MOD READY!");
    TraceStartCallReports();
    return nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_com_android_support_Main_CheckOverlayPermission(JNIEnv*,jclass,jobject){}

__attribute__((constructor))
void l_m(){LOGI(".so injected");pthread_t ptid;pthread_create(&ptid,nullptr,h_t,nullptr);}

#include "Includes/JNI_Boilerplate.h"
