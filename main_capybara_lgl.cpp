// ============================================================
//  LGL MOD — com.yibu.CapybaraSurvivorGOGOGO v10.0.1
//  Chuc nang: Speed (1-10x) + Bat Tu + Dame 1-Phat-Chet + Free Shop
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
static bool  g_god       = false;
static bool  g_dame      = false;
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

// 6. HEALTH.TAKEDAMAGE
// Health.TakeDamage(Int64 num, Boolean isCritical, Boolean markDamage) @ 0x22E7744
// Fields (du dump.cs):
//   isEnemyBase @ 0x81  — bool: true=ke thu, false=player/dong-minh
//   curHealth   @ 0x20  — Int64: mau hien tai
//   maxHealth   @ 0x28  — Int64: mau toi da
//   isDead      @ 0x8C  — bool
//   isInvincible @ 0xE0 — bool
// Logic:
//   g_god ON  + isEnemyBase==false => skip (player khong mat mau)
//   g_dame ON + isEnemyBase==true  => truyen num=maxHealth (chet 1 phat, giu du event)

typedef void (*t_TakeDamage)(void* self, int64_t num, bool isCritical, bool markDamage, void* mi);
static t_TakeDamage o_TakeDamage      = nullptr;
static t_TakeDamage o_CountTakeDamage = nullptr;

static void HandleTakeDamage(t_TakeDamage orig, void* self, int64_t num, bool isCritical, bool markDamage, void* mi) {
    if(!self)goto call_orig;
    // God mode: player/dong-minh khong mat mau
    if(g_god && !Field<bool>(self,0x81)){
        LOGI("God: chan %lld sat thuong cho self=%p",num,self);
        return;
    }
    // Dame: ke thu chet 1 phat
    if(g_dame && Field<bool>(self,0x81)){
        int64_t maxHP = Field<int64_t>(self,0x28);
        if(maxHP > 0){
            LOGI("Dame: 1-shot ke thu self=%p maxHP=%lld",self,maxHP);
            if(orig) orig(self,maxHP,isCritical,markDamage,mi);
            return;
        }
    }
call_orig:
    if(orig) orig(self,num,isCritical,markDamage,mi);
}

void h_TakeDamage(void* self, int64_t num, bool isCritical, bool markDamage, void* mi){
    T_COUNT(TakeDamage);
    HandleTakeDamage(o_TakeDamage,self,num,isCritical,markDamage,mi);
}
void h_CountTakeDamage(void* self, int64_t num, bool isCritical, bool markDamage, void* mi){
    T_COUNT(CountTakeDamage);
    HandleTakeDamage(o_CountTakeDamage,self,num,isCritical,markDamage,mi);
}

// 7. FREE SHOP
// 4 shop cell class, moi cai co Buy() kiem tra currency roi goi BuyItem().
// Hook Buy() / BuyBtn_OnClick() -> goi BuyItem() thang, bo qua kiem tra tien.
//   ActivShopCell.Buy()          @ 0x241A79C  -> BuyItem() @ 0x241AA08
//   AsprintShopCell.Buy()        @ 0x2424270  -> BuyItem() @ 0x24244A8
//   BlackMarketCell.Buy()        @ 0x24260F4  -> BuyItem() @ 0x24262EC
//   ExchShopCellCom.BuyBtn_OnClick() @ 0x238E944 -> BuyItem() @ 0x238EB28

typedef void (*t_ShopBuy)(void* self, void* mi);
static t_ShopBuy fn_ActivBuyItem   = nullptr;
static t_ShopBuy fn_AsprintBuyItem = nullptr;
static t_ShopBuy fn_BlackBuyItem   = nullptr;
static t_ShopBuy fn_ExchBuyItem    = nullptr;

static t_ShopBuy o_ActivBuy   = nullptr;
static t_ShopBuy o_AsprintBuy = nullptr;
static t_ShopBuy o_BlackBuy   = nullptr;
static t_ShopBuy o_ExchBuyBtn = nullptr;

// Hook Init() de zero cost field -> buy button luon enabled khi FreeShop ON
// ActivShopCell.Init(Int32 id, ActivShopUI shop)  cost@0x5C
// AsprintShopCell.Init(ShopGoodsData, AsprintShop) cost@0x54
// BlackMarketCell.Init(ShopGoodsData, Int32)       cost@0x74
// ExchShopCellCom.Init(ExchShopCom, ShopGoodsData) cost@0x68
typedef void (*t_ActivInit)  (void* self, int32_t id,    void* shopUI,       void* mi);
typedef void (*t_AsprintInit)(void* self, void* goodsData, void* shop,       void* mi);
typedef void (*t_BlackInit)  (void* self, void* goodsData, int32_t idxPD,   void* mi);
typedef void (*t_ExchInit)   (void* self, void* shop,    void* goodsData,   void* mi);
static t_ActivInit   o_ActivInit   = nullptr;
static t_AsprintInit o_AsprintInit = nullptr;
static t_BlackInit   o_BlackInit   = nullptr;
static t_ExchInit    o_ExchInit    = nullptr;

void h_ActivInit(void* self, int32_t id, void* shopUI, void* mi){
    T_COUNT(ActivInit);
    if(o_ActivInit) o_ActivInit(self,id,shopUI,mi);
    if(g_free_shop && self) Field<int32_t>(self,0x5C)=0;
}
void h_AsprintInit(void* self, void* goodsData, void* shop, void* mi){
    T_COUNT(AsprintInit);
    if(o_AsprintInit) o_AsprintInit(self,goodsData,shop,mi);
    if(g_free_shop && self) Field<int32_t>(self,0x54)=0;
}
void h_BlackInit(void* self, void* goodsData, int32_t idxPD, void* mi){
    T_COUNT(BlackInit);
    if(o_BlackInit) o_BlackInit(self,goodsData,idxPD,mi);
    if(g_free_shop && self) Field<int32_t>(self,0x74)=0;
}
void h_ExchInit(void* self, void* shop, void* goodsData, void* mi){
    T_COUNT(ExchInit);
    if(o_ExchInit) o_ExchInit(self,shop,goodsData,mi);
    if(g_free_shop && self) Field<int32_t>(self,0x68)=0;
}

void h_ActivBuy(void* self, void* mi){
    T_COUNT(ActivBuy);
    if(g_free_shop && fn_ActivBuyItem && self){ fn_ActivBuyItem(self,nullptr); return; }
    if(o_ActivBuy) o_ActivBuy(self,mi);
}
void h_AsprintBuy(void* self, void* mi){
    T_COUNT(AsprintBuy);
    if(g_free_shop && fn_AsprintBuyItem && self){ fn_AsprintBuyItem(self,nullptr); return; }
    if(o_AsprintBuy) o_AsprintBuy(self,mi);
}
void h_BlackBuy(void* self, void* mi){
    T_COUNT(BlackBuy);
    if(g_free_shop && fn_BlackBuyItem && self){ fn_BlackBuyItem(self,nullptr); return; }
    if(o_BlackBuy) o_BlackBuy(self,mi);
}
void h_ExchBuyBtn(void* self, void* mi){
    T_COUNT(ExchBuyBtn);
    if(g_free_shop && fn_ExchBuyItem && self){ fn_ExchBuyItem(self,nullptr); return; }
    if(o_ExchBuyBtn) o_ExchBuyBtn(self,mi);
}

// 8. GAN HOOK
static void ResolveAll(){
    LOGI("=== Resolve START ===");
    T_HOOK("UnityEngine","Time","get_timeScale",0,getTS);
    T_HOOK("UnityEngine","Time","set_timeScale",1,setTS);
    if(o_getTS) g_gameScale=o_getTS(nullptr);
    // Health.TakeDamage(3)
    T_HOOK("","Health","TakeDamage",3,TakeDamage);
    // CountHealth.TakeDamage(3) — subclass override, hook rieng
    T_HOOK("","CountHealth","TakeDamage",3,CountTakeDamage);
    // Free Shop: cache BuyItem() roi hook Buy() cua tung shop cell class
    {void* k=FindClassAnywhere("ActivShopCell");
     if(k){void* m=FindMethod(k,"BuyItem",0);if(m)fn_ActivBuyItem=(t_ShopBuy)MethodFnPtr(m);LOGI("ActivBuyItem: %p",fn_ActivBuyItem);}
     T_HOOK("","ActivShopCell","Buy",0,ActivBuy);}
    {void* k=FindClassAnywhere("AsprintShopCell");
     if(k){void* m=FindMethod(k,"BuyItem",0);if(m)fn_AsprintBuyItem=(t_ShopBuy)MethodFnPtr(m);LOGI("AsprintBuyItem: %p",fn_AsprintBuyItem);}
     T_HOOK("","AsprintShopCell","Buy",0,AsprintBuy);}
    {void* k=FindClassAnywhere("BlackMarketCell");
     if(k){void* m=FindMethod(k,"BuyItem",0);if(m)fn_BlackBuyItem=(t_ShopBuy)MethodFnPtr(m);LOGI("BlackBuyItem: %p",fn_BlackBuyItem);}
     T_HOOK("","BlackMarketCell","Buy",0,BlackBuy);}
    {void* k=FindClassAnywhere("ExchShopCellCom");
     if(k){void* m=FindMethod(k,"BuyItem",0);if(m)fn_ExchBuyItem=(t_ShopBuy)MethodFnPtr(m);LOGI("ExchBuyItem: %p",fn_ExchBuyItem);}
     T_HOOK("","ExchShopCellCom","BuyBtn_OnClick",0,ExchBuyBtn);}
    // Hook Init() de zero cost -> buy button luon enabled khi FreeShop ON
    T_HOOK("","ActivShopCell","Init",2,ActivInit);
    T_HOOK("","AsprintShopCell","Init",2,AsprintInit);
    T_HOOK("","BlackMarketCell","Init",2,BlackInit);
    T_HOOK("","ExchShopCellCom","Init",2,ExchInit);
    TraceReport();
    LOGI("=== Resolve DONE ===");
}

// 9. MENU
jobjectArray GetFeatureList(JNIEnv* env,jobject){
    const char* features[]={
        OBFUSCATE("Category_" GAME_NAME),
        OBFUSCATE("0_SeekBar_\xe2\x9a\xa1 Toc Do Game_1_10"),
        OBFUSCATE("1_Toggle_\xf0\x9f\x9b\xa1 Bat Tu"),
        OBFUSCATE("2_Toggle_\xe2\x9a\x94 Dame 1-Phat-Chet"),
        OBFUSCATE("3_Toggle_\xf0\x9f\x9b\x92 Free Shop"),
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
        case 1: g_god=boolean; LOGI("BatTu: %s",g_god?"BAT":"TAT"); break;
        case 2: g_dame=boolean; LOGI("Dame: %s",g_dame?"BAT":"TAT"); break;
        case 3: g_free_shop=boolean; LOGI("FreeShop: %s",g_free_shop?"BAT":"TAT"); break;
    }
}

// 10. KHOI TAO
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
