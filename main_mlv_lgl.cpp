// LGL May Root -- My Little Village (vn.corochti.mylittlevillage)
#define LOG_TAG      "MLV_MOD"
#define GAME_NAME    "My Little Village"
#define GAME_PACKAGE "vn.corochti.mylittlevillage"
#include "Includes/LGLTrace.h"

#include <jni.h>
bool isGameReady = false;
static jboolean my_isGameLibLoaded(JNIEnv*,jobject){return isGameReady?JNI_TRUE:JNI_FALSE;}

#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <link.h>
#include <elf.h>
#include <errno.h>
#include <cmath>
#include <atomic>
#include <android/log.h>
#include "Includes/obfuscate.h"
#include <Dobby/dobby.h>

// -- ElfImg --
struct ElfImg {
    uintptr_t         base  = 0;
    const ElfW(Phdr)* phdr  = nullptr;
    size_t            phnum = 0;
    const char*       path  = nullptr;
    bool has() const { return base && phdr && phnum; }
    bool contains(uintptr_t a, size_t sz, unsigned fl = PF_R) const {
        if (!a || !sz || !phdr) return false;
        for (size_t i = 0; i < phnum; ++i) {
            const ElfW(Phdr)& p = phdr[i];
            if (p.p_type != PT_LOAD || (p.p_flags & fl) != fl) continue;
            uintptr_t s = base + p.p_vaddr;
            if (a >= s && (a - s) <= p.p_memsz - sz) return true;
        }
        return false;
    }
    bool ru32(uintptr_t a, uint32_t* v) const {
        if (!contains(a, 4)) return false; memcpy(v,(const void*)a,4); return true;
    }
    uintptr_t da(ElfW(Addr) val, size_t sz) const {
        if (!val) return 0;
        if (contains((uintptr_t)val,sz)) return (uintptr_t)val;
        uintptr_t a = base+val; return contains(a,sz)?a:0;
    }
};

// -- ElfExports --
class ElfExports {
    const ElfImg& img_;
    uintptr_t sym_=0,str_=0,sysv_=0,gnu_=0; size_t strsz_=0;
    void* symAt(uint32_t idx,const char* name) const {
        uintptr_t a=sym_+idx*sizeof(ElfW(Sym));
        if(!img_.contains(a,sizeof(ElfW(Sym)))) return nullptr;
        ElfW(Sym) s; memcpy(&s,(const void*)a,sizeof(s));
        if(s.st_shndx==SHN_UNDEF||s.st_name>=strsz_) return nullptr;
        unsigned t=s.st_info&0xf,b=s.st_info>>4;
        if((t!=STT_FUNC&&t!=STT_NOTYPE)||(b!=STB_GLOBAL&&b!=STB_WEAK)) return nullptr;
        size_t nl=strlen(name)+1; if(nl>strsz_-s.st_name) return nullptr;
        if(memcmp((const void*)(str_+s.st_name),name,nl)) return nullptr;
        uintptr_t fn=(s.st_shndx==SHN_ABS)?s.st_value:img_.base+s.st_value;
        return img_.contains(fn,1,PF_X)?(void*)fn:nullptr;
    }
public:
    explicit ElfExports(const ElfImg& img):img_(img){}
    bool init(){
        uintptr_t dyn=0; size_t cnt=0;
        for(size_t i=0;i<img_.phnum;++i){
            const ElfW(Phdr)&p=img_.phdr[i];
            if(p.p_type==PT_DYNAMIC){dyn=img_.base+p.p_vaddr;cnt=p.p_memsz/sizeof(ElfW(Dyn));break;}
        }
        if(!cnt||cnt>65536||!img_.contains(dyn,cnt*sizeof(ElfW(Dyn)))) return false;
        const ElfW(Dyn)*d=(const ElfW(Dyn)*)dyn;
        ElfW(Addr) sym=0,str=0,sysv=0,gnu=0; size_t stride=sizeof(ElfW(Sym));
        for(size_t i=0;i<cnt;++i){
            if(d[i].d_tag==DT_NULL) break;
            switch(d[i].d_tag){
                case DT_SYMTAB:sym=d[i].d_un.d_ptr;break; case DT_STRTAB:str=d[i].d_un.d_ptr;break;
                case DT_STRSZ:strsz_=d[i].d_un.d_val;break; case DT_SYMENT:stride=d[i].d_un.d_val;break;
                case DT_HASH:sysv=d[i].d_un.d_ptr;break; case DT_GNU_HASH:gnu=d[i].d_un.d_ptr;break;
            }
        }
        if(stride!=sizeof(ElfW(Sym))||!strsz_) return false;
        sym_=img_.da(sym,sizeof(ElfW(Sym))); str_=img_.da(str,strsz_);
        sysv_=img_.da(sysv,8); gnu_=img_.da(gnu,16);
        return sym_&&str_&&(gnu_||sysv_);
    }
    void* find(const char* name) const {
        if(!name||!sym_||!str_) return nullptr;
        if(gnu_){
            uint32_t bc=0,fs=0,bl=0;
            img_.ru32(gnu_,&bc);img_.ru32(gnu_+4,&fs);img_.ru32(gnu_+8,&bl);
            uintptr_t buckets=gnu_+16+bl*sizeof(ElfW(Addr)),chains=buckets+bc*4;
            if(bc&&bl){
                uint32_t h=5381,idx=0;
                for(const unsigned char*p=(const unsigned char*)name;*p;++p)h=h*33+*p;
                if(img_.ru32(buckets+(h%bc)*4,&idx)&&idx>=fs){
                    for(size_t step=0;step<1048576;++step,++idx){
                        uint32_t c=0; if(!img_.ru32(chains+(idx-fs)*4,&c))break;
                        if((c|1u)==(h|1u)){void*fn=symAt(idx,name);if(fn)return fn;}
                        if(c&1u)break;
                    }
                }
            }
        }
        if(sysv_){
            uint32_t bc=0,sc=0;
            img_.ru32(sysv_,&bc);img_.ru32(sysv_+4,&sc);
            uintptr_t buckets=sysv_+8,chains=buckets+bc*4;
            if(bc&&sc){
                uint32_t h=0,idx=0;
                for(const unsigned char*p=(const unsigned char*)name;*p;++p){h=(h<<4)+*p;uint32_t hi=h&0xf0000000u;if(hi){h^=hi>>24;}h&=~hi;}
                if(img_.ru32(buckets+(h%bc)*4,&idx)){
                    for(size_t step=0;idx&&idx<sc&&step<1048576;++step){
                        void*fn=symAt(idx,name);if(fn)return fn;
                        if(!img_.ru32(chains+idx*4,&idx))break;
                    }
                }
            }
        }
        return nullptr;
    }
};

// -- FindLib --
static bool FindLib(ElfImg& img,const char* libname){
    struct Ctx{ElfImg*img;const char*name;bool found;};
    Ctx ctx{&img,libname,false};
    dl_iterate_phdr([](struct dl_phdr_info*info,size_t,void*data)->int{
        Ctx*c=(Ctx*)data; const char*path=info->dlpi_name;
        if(!path||!info->dlpi_phdr||!info->dlpi_phnum) return 0;
        const char*leaf=strrchr(path,'/');leaf=leaf?leaf+1:path;
        if(strcmp(leaf,c->name)!=0) return 0;
        c->img->base=info->dlpi_addr; c->img->phdr=info->dlpi_phdr;
        c->img->phnum=info->dlpi_phnum; c->img->path=path;
        c->found=true; return 1;
    },&ctx);
    return ctx.found;
}

// -- IL2CPP API (khong static - LGLTrace.h dung extern de tham chieu) --
void*   (*il2cpp_domain_get)()                                        = nullptr;
void*   (*il2cpp_thread_attach)(void*)                                = nullptr;
void**  (*il2cpp_domain_get_assemblies)(void*,size_t*)                = nullptr;
void*   (*il2cpp_assembly_get_image)(void*)                           = nullptr;
void*   (*il2cpp_class_from_name)(void*,const char*,const char*)      = nullptr;
void*   (*il2cpp_class_get_method_from_name)(void*,const char*,int)   = nullptr;
void*   (*il2cpp_class_get_methods)(void*,void**)                     = nullptr;
const char* (*il2cpp_method_get_name)(void*)                          = nullptr;
uint32_t (*il2cpp_method_get_param_count)(void*)                      = nullptr;
size_t  (*il2cpp_image_get_class_count)(void*)                        = nullptr;
void*   (*il2cpp_image_get_class)(void*,size_t)                       = nullptr;
const char* (*il2cpp_class_get_name)(void*)                           = nullptr;

static bool InitIL2CppAPI(const ElfImg& img){
    ElfExports ex(img); if(!ex.init()){LOGE("ElfExports init FAIL");return false;}
    void*h=img.path?dlopen(img.path,RTLD_NOW|RTLD_NOLOAD):nullptr;
    auto res=[&](const char*n)->void*{
        void*fn=h?dlsym(h,n):nullptr;
        if(fn&&img.contains((uintptr_t)fn,1,PF_X)) return fn;
        return ex.find(n);
    };
    il2cpp_domain_get             =(decltype(il2cpp_domain_get))            res("il2cpp_domain_get");
    il2cpp_thread_attach          =(decltype(il2cpp_thread_attach))         res("il2cpp_thread_attach");
    il2cpp_domain_get_assemblies  =(decltype(il2cpp_domain_get_assemblies)) res("il2cpp_domain_get_assemblies");
    il2cpp_assembly_get_image     =(decltype(il2cpp_assembly_get_image))    res("il2cpp_assembly_get_image");
    il2cpp_class_from_name        =(decltype(il2cpp_class_from_name))       res("il2cpp_class_from_name");
    il2cpp_class_get_method_from_name=(decltype(il2cpp_class_get_method_from_name))res("il2cpp_class_get_method_from_name");
    il2cpp_class_get_methods      =(decltype(il2cpp_class_get_methods))     res("il2cpp_class_get_methods");
    il2cpp_method_get_name        =(decltype(il2cpp_method_get_name))       res("il2cpp_method_get_name");
    il2cpp_method_get_param_count =(decltype(il2cpp_method_get_param_count))res("il2cpp_method_get_param_count");
    il2cpp_image_get_class_count  =(decltype(il2cpp_image_get_class_count)) res("il2cpp_image_get_class_count");
    il2cpp_image_get_class        =(decltype(il2cpp_image_get_class))       res("il2cpp_image_get_class");
    il2cpp_class_get_name         =(decltype(il2cpp_class_get_name))        res("il2cpp_class_get_name");
    bool ok=il2cpp_domain_get&&il2cpp_thread_attach&&il2cpp_domain_get_assemblies&&
            il2cpp_assembly_get_image&&il2cpp_class_from_name&&il2cpp_class_get_method_from_name;
    LOGI("IL2CPP API: %s",ok?"OK":"FAIL");
    return ok;
}

// Ham can cho T_HOOK trong LGLTrace.h (khong static - T_HOOK dung extern)
void* FindClass(const char* ns,const char* cls){
    if(!il2cpp_domain_get||!il2cpp_domain_get_assemblies||!il2cpp_assembly_get_image||!il2cpp_class_from_name) return nullptr;
    void*domain=il2cpp_domain_get(); if(!domain) return nullptr;
    size_t n=0; void**asms=il2cpp_domain_get_assemblies(domain,&n);
    for(size_t i=0;i<n;++i){
        void*img=il2cpp_assembly_get_image(asms[i]); if(!img) continue;
        void*k=il2cpp_class_from_name(img,ns,cls); if(k) return k;
    }
    return nullptr;
}

void* FindClassAnywhere(const char* cls){
    if(!il2cpp_domain_get||!il2cpp_domain_get_assemblies||!il2cpp_assembly_get_image) return nullptr;
    void*domain=il2cpp_domain_get(); if(!domain) return nullptr;
    size_t n=0; void**asms=il2cpp_domain_get_assemblies(domain,&n);
    for(size_t i=0;i<n;++i){
        void*img=il2cpp_assembly_get_image(asms[i]); if(!img) continue;
        void*k=il2cpp_class_from_name(img,"",cls); if(k) return k;
        if(il2cpp_image_get_class_count&&il2cpp_image_get_class&&il2cpp_class_get_name){
            size_t nc=il2cpp_image_get_class_count(img);
            for(size_t j=0;j<nc;++j){
                void*kls=il2cpp_image_get_class(img,j); if(!kls) continue;
                const char*kn=il2cpp_class_get_name(kls);
                if(kn&&strcmp(kn,cls)==0) return kls;
            }
        }
    }
    return nullptr;
}

void* MethodAddr(const char* ns,const char* cls,const char* method,int n){
    void*k=FindClass(ns,cls); if(!k) return nullptr;
    if(!il2cpp_class_get_method_from_name) return nullptr;
    void*m=il2cpp_class_get_method_from_name(k,method,n); if(!m) return nullptr;
    return *(void**)m;
}

// -- State --
static std::atomic<int>  g_spd{0};
static std::atomic<bool> g_shop{false};
static std::atomic<float> g_ts_game{1.0f};
static const float SPD_MUL[5]={1.0f,2.0f,3.0f,5.0f,10.0f};
static void (*p_GiveRewards)(void*,void*,void*,void*)=nullptr;

// -- Speed hooks --
typedef void  (*t_set_ts)(float,void*);
typedef float (*t_get_ts)(void*);
static t_set_ts o_set_ts=nullptr;
static t_get_ts o_get_ts=nullptr;

static void h_set_ts(float v,void*mi){
    T_COUNT(set_ts);
    g_ts_game.store(v,std::memory_order_relaxed);
    int s=g_spd.load(std::memory_order_relaxed);
    float mul=(s>=0&&s<5)?SPD_MUL[s]:1.0f;
    if(o_set_ts) o_set_ts(v*mul,mi);
}
static float h_get_ts(void*mi){
    T_COUNT(get_ts);
    return g_ts_game.load(std::memory_order_relaxed);
}

// -- Free Shop hooks --
typedef bool (*t_BuyIAP)(void*,void*,void*);
static t_BuyIAP o_BuyIAP=nullptr;
static bool h_BuyIAP(void*self,void*productId,void*mi){
    T_COUNT(BuyIAP);
    if(g_shop.load(std::memory_order_relaxed)){
        LOGI("FreeShop: BuyIAP -> GiveRewards");
        if(p_GiveRewards) p_GiveRewards(self,productId,nullptr,nullptr);
        return true;
    }
    return o_BuyIAP?o_BuyIAP(self,productId,mi):false;
}

typedef bool (*t_IsIAP)(void*,void*,int32_t*,void*);
static t_IsIAP o_IsIAP=nullptr;
static bool h_IsIAP(void*self,void*id,int32_t*amount,void*mi){
    T_COUNT(IsIAP);
    if(g_shop.load(std::memory_order_relaxed)){if(amount)*amount=1;return true;}
    return o_IsIAP?o_IsIAP(self,id,amount,mi):false;
}

typedef bool (*t_isBoughtAnyIAP)(void*,void*);
static t_isBoughtAnyIAP o_isBoughtAnyIAP=nullptr;
static bool h_isBoughtAnyIAP(void*self,void*mi){
    T_COUNT(isBoughtAnyIAP);
    if(g_shop.load(std::memory_order_relaxed)) return true;
    return o_isBoughtAnyIAP?o_isBoughtAnyIAP(self,mi):false;
}

typedef bool (*t_IsRmAds)(void*,void*);
static t_IsRmAds o_IsRmAds=nullptr;
static bool h_IsRmAds(void*self,void*mi){
    T_COUNT(IsRmAds);
    if(g_shop.load(std::memory_order_relaxed)) return true;
    return o_IsRmAds?o_IsRmAds(self,mi):false;
}

// -- ResolveAll --
static void ResolveAll(){
    T_STEP("ResolveAll");

    T_HOOK("UnityEngine","Time","get_timeScale",0,get_ts);
    T_HOOK("UnityEngine","Time","set_timeScale",1,set_ts);

    T_HOOK("TuoiTho","IAPController","BuyIAP",1,BuyIAP);

    // Cache GiveRewards de goi truc tiep (khong hook)
    {
        void*addr=MethodAddr("TuoiTho","IAPController","GiveRewards",2);
        if(addr){ p_GiveRewards=(decltype(p_GiveRewards))addr; LOGI("GiveRewards cached: %p",addr); }
        else LOGE("GiveRewards NOT FOUND");
    }

    T_HOOK("TuoiTho","IAPModel","IsIAP",2,IsIAP);
    T_HOOK("TuoiTho","IAPModel","get_isBoughtAnyIAP",0,isBoughtAnyIAP);
    T_HOOK("TuoiTho","IAPModel","get_IsRemoveInterstitialAds",0,IsRmAds);

    TraceReport();
}

// -- il2cpp_init hook (ban mau v2.2 - KHONG SUA) --
static std::atomic<bool> g_il2cpp_ready{false};
typedef void* (*t_il2cpp_init)(const char*,int,int);
static t_il2cpp_init o_il2cpp_init=nullptr;
static void* h_il2cpp_init(const char*domain,int n_threads,int flags){
    T_COUNT(il2cpp_init);
    void*result=o_il2cpp_init?o_il2cpp_init(domain,n_threads,flags):nullptr;
    g_il2cpp_ready.store(true,std::memory_order_release);
    LOGI("il2cpp_init xong");
    return result;
}

static void* mod_thread(void*){
    T_STEP("wait libil2cpp");
    ElfImg il2cpp;
    for(int retry=0;retry<120;++retry){
        if(FindLib(il2cpp,"libil2cpp.so")&&il2cpp.has()) break;
        usleep(100000);
    }
    if(!il2cpp.has()){LOGE("libil2cpp.so NOT FOUND");return nullptr;}
    LOGI("libil2cpp base=%p",(void*)il2cpp.base);

    T_STEP("hook il2cpp_init");
    {
        ElfExports ex(il2cpp);
        if(ex.init()){
            void*fn=ex.find("il2cpp_init");
            if(fn){
                if(DobbyHook(fn,(void*)h_il2cpp_init,(void**)&o_il2cpp_init)==0&&o_il2cpp_init)
                    LOGI("Hook il2cpp_init: OK");
                else LOGE("Hook il2cpp_init: FAIL");
            } else LOGE("il2cpp_init symbol NOT FOUND");
        }
    }

    T_STEP("wait il2cpp_ready");
    for(int retry=0;retry<300;++retry){
        if(g_il2cpp_ready.load(std::memory_order_acquire)) break;
        usleep(100000);
    }
    if(!g_il2cpp_ready.load(std::memory_order_acquire)){
        LOGE("il2cpp_init NEVER FIRED (timeout 30s)");
        return nullptr;
    }

    T_STEP("wait assemblies");
    TraceSleep(5);

    T_STEP("InitIL2CppAPI");
    if(!InitIL2CppAPI(il2cpp)){LOGE("InitIL2CppAPI FAIL");return nullptr;}

    T_STEP("attach thread");
    void*domain=il2cpp_domain_get();
    if(!domain){LOGE("domain NULL");return nullptr;}
    il2cpp_thread_attach(domain);

    T_STEP("ResolveAll");
    ResolveAll();
    isGameReady=true;
    TraceStartCallReports();
    LOGI("MOD READY");
    return nullptr;
}

__attribute__((constructor))
static void on_load(){
    T_STEP("on_load");
    pthread_t t; pthread_create(&t,nullptr,mod_thread,nullptr);
}

// -- JNI stubs --
static jstring IconWebViewData(JNIEnv*env,jclass){return env->NewStringUTF("");}
static jstring GetMenuTitle(JNIEnv*env,jclass){return env->NewStringUTF(GAME_NAME);}
static jstring GetMenuSubtitle(JNIEnv*env,jclass){return env->NewStringUTF("LGL Mod");}
static jstring GetTeleLink(JNIEnv*env,jclass){return env->NewStringUTF("");}
static jstring GetTeleText(JNIEnv*env,jclass){return env->NewStringUTF("");}
static jstring GetWebLink(JNIEnv*env,jclass){return env->NewStringUTF("");}
static jstring GetWebText(JNIEnv*env,jclass){return env->NewStringUTF("");}
static jstring GetZaloLink(JNIEnv*env,jclass){return env->NewStringUTF("");}
static jstring GetZaloText(JNIEnv*env,jclass){return env->NewStringUTF("");}
static jboolean VerifyKeyNative(JNIEnv*,jclass,jstring){return JNI_TRUE;}
static void Init(JNIEnv*,jclass,jobject,jobject,jobject){}
static jobjectArray SettingsList(JNIEnv*env,jclass){
    return env->NewObjectArray(0,env->FindClass("java/lang/String"),nullptr);
}

static jobjectArray GetFeatureList(JNIEnv*env,jclass){
    const char*items[]={"0_SeekBar_Toc do (x1/2/3/5/10)_0_4","1_Toggle_Free Shop"};
    int n=2; jclass sc=env->FindClass("java/lang/String");
    jobjectArray arr=env->NewObjectArray(n,sc,nullptr);
    for(int i=0;i<n;++i) env->SetObjectArrayElement(arr,i,env->NewStringUTF(items[i]));
    return arr;
}

static void Changes(JNIEnv*,jclass,jobject,
    jint featNum,jstring,jint value,jboolean enabled,jstring){
    switch(featNum){
        case 0:{
            int lv=(int)value; if(lv<0)lv=0; if(lv>4)lv=4;
            g_spd.store(lv,std::memory_order_relaxed);
            float cur=g_ts_game.load(std::memory_order_relaxed);
            if(o_set_ts) o_set_ts(cur*SPD_MUL[lv],nullptr);
            LOGI("Speed x%.0f (index=%d)",SPD_MUL[lv],lv);
            break;
        }
        case 1:
            g_shop.store(enabled==JNI_TRUE,std::memory_order_relaxed);
            LOGI("Free Shop: %s",(enabled==JNI_TRUE)?"ON":"OFF");
            break;
    }
}

static void CheckOverlayPermission(JNIEnv*,jclass,jobject){}

extern "C" JNIEXPORT jstring JNICALL Java_com_android_support_Menu_getMenuTitle(JNIEnv*env,jclass){
    return env->NewStringUTF(GAME_NAME);
}
extern "C" JNIEXPORT jstring JNICALL Java_com_android_support_Menu_getMenuSubtitle(JNIEnv*env,jclass){
    return env->NewStringUTF("LGL Mod");
}

#include "Includes/JNI_Boilerplate.h"
