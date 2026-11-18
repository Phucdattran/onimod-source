// Champion Forge 1.0.16 — ARM64 Auto A1 (metadata compatibility checks).
// Build with the supplied LGL 3.2 project. All gameplay operations run on Unity's thread.
// This file does not alter Billing, purchase receipts, or server purchase requests.
#include <pthread.h>
#include <jni.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include <link.h>
#include <elf.h>
#include <atomic>
#include <sys/mman.h>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>
#include <mutex>
#include <deque>
#include <type_traits>
#include <cstring>
#include <map>
#include "Includes/obfuscate.h"
#include "Includes/Logger.h"
#include "Includes/Utils.h"
#include <Dobby/dobby.h>
#define IconWebViewData LglTemplateIconWebViewData
#include "Menu/Setup.h"
#undef IconWebViewData
#include "MenuStrings.h"
#define LOG_TAG "CHAMPIONFORGE_MOD"
#define GAME_NAME "Champion Forge - Auto A1"
#define GAME_PACKAGE "com.mobibrain.championforge"
#define TraceStartCallReports LglTemplateTraceStartCallReports
#include "Includes/LGLTrace.h"
#undef TraceStartCallReports
#if !defined(__aarch64__)
#error This Main.cpp targets arm64-v8a only.
#endif
static_assert(sizeof(void*) == 8, "ARM64 required");
jstring IconWebViewData(JNIEnv* env, jobject thiz) {
    return Java_com_android_support_Menu_Icon(env, thiz);
}
std::atomic<bool> isGameReady{false};
jboolean my_isGameLibLoaded(JNIEnv*, jobject) { return isGameReady.load() ? JNI_TRUE : JNI_FALSE; }
static void SafeTraceReinstall() {
    for (int signal : {SIGSEGV, SIGABRT, SIGILL, SIGBUS}) {
        struct sigaction current{};
        if (sigaction(signal, nullptr, &current) != 0) continue;
        if ((current.sa_flags & SA_SIGINFO) && current.sa_sigaction == trace_crash_handler) continue;
        struct sigaction handler{};
        handler.sa_sigaction = trace_crash_handler;
        handler.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&handler.sa_mask);
        sigaction(signal, &handler, &g_old_sa[signal & 31]);
    }
}
static void* SafeTraceReports(void*) {
    TraceSleep(5);
    SafeTraceReinstall();
    std::vector<long> previous;
    for (;;) {
        std::vector<HookCounter*> counters;
        pthread_mutex_lock(&g_cnt_mx);
        counters = g_counters;
        pthread_mutex_unlock(&g_cnt_mx);
        previous.resize(counters.size(), -1);
        for (size_t i=0;i<counters.size();++i) {
            auto* counter=counters[i];
            if (!__atomic_load_n(&counter->hooked,__ATOMIC_ACQUIRE)) continue;
            const long count=__atomic_load_n(&counter->calls,__ATOMIC_RELAXED);
            if(count!=previous[i]){LOGI("Hook %s: calls=%ld",counter->name,count);previous[i]=count;}
        }
        TraceSleep(20);
    }
    return nullptr;
}
static void TraceStartCallReports() {
    pthread_t thread{};
    if (pthread_create(&thread, nullptr, SafeTraceReports, nullptr) == 0) pthread_detach(thread);
}


uintptr_t il2cpp_base = 0;
void* (*il2cpp_domain_get)() = nullptr;
void* (*il2cpp_thread_attach)(void*) = nullptr;
void (*il2cpp_thread_detach)(void*) = nullptr;
void** (*il2cpp_domain_get_assemblies)(void*,size_t*) = nullptr;
void* (*il2cpp_assembly_get_image)(void*) = nullptr;
const char* (*il2cpp_image_get_name)(void*) = nullptr;
size_t (*il2cpp_image_get_class_count)(void*) = nullptr;
void* (*il2cpp_image_get_class)(void*,size_t) = nullptr;
void* (*il2cpp_class_from_name)(void*,const char*,const char*) = nullptr;
const char* (*il2cpp_class_get_name)(void*) = nullptr;
void* (*il2cpp_class_get_methods)(void*,void**) = nullptr;
const char* (*il2cpp_method_get_name)(void*) = nullptr;
uint32_t (*il2cpp_method_get_param_count)(void*) = nullptr;
void* (*il2cpp_method_get_param)(void*,uint32_t) = nullptr;
void* (*il2cpp_method_get_return_type)(void*) = nullptr;
uint32_t (*il2cpp_method_get_flags)(void*,uint32_t*) = nullptr;
char* (*il2cpp_type_get_name)(void*) = nullptr;
void (*il2cpp_free)(void*) = nullptr;
void* (*il2cpp_class_get_field_from_name)(void*,const char*) = nullptr;
void* (*il2cpp_field_get_type)(void*) = nullptr;
void (*il2cpp_field_static_get_value)(void*,void*) = nullptr;
void (*il2cpp_field_static_set_value)(void*,void*) = nullptr;
void (*il2cpp_field_get_value)(void*,void*,void*) = nullptr;
void (*il2cpp_runtime_class_init)(void*) = nullptr;
void* (*il2cpp_runtime_invoke)(void*,void*,void**,void**) = nullptr;
void* (*il2cpp_object_unbox)(void*) = nullptr;
void* (*il2cpp_object_get_virtual_method)(void*,void*) = nullptr;
void* (*il2cpp_class_from_type)(void*) = nullptr;
bool (*il2cpp_class_is_valuetype)(void*) = nullptr;

uint32_t (*il2cpp_field_get_flags)(void*) = nullptr;
void* (*il2cpp_method_get_class)(void*) = nullptr;
const char* (*il2cpp_class_get_namespace)(void*) = nullptr;
void* (*il2cpp_object_get_class)(void*) = nullptr;
bool (*il2cpp_class_is_assignable_from)(void*,void*) = nullptr;
bool (*il2cpp_method_is_generic)(void*) = nullptr;

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


static bool InitIL2CppAPI() {
#define LOAD(n) n = reinterpret_cast<decltype(n)>(ElfSym(#n)); if (!n) { LOGE("Missing export: %s", #n); return false; }
    LOAD(il2cpp_domain_get); LOAD(il2cpp_thread_attach); LOAD(il2cpp_thread_detach);
    LOAD(il2cpp_domain_get_assemblies); LOAD(il2cpp_assembly_get_image); LOAD(il2cpp_image_get_name);
    LOAD(il2cpp_image_get_class_count); LOAD(il2cpp_image_get_class); LOAD(il2cpp_class_from_name);
    LOAD(il2cpp_class_get_name); LOAD(il2cpp_class_get_methods); LOAD(il2cpp_method_get_name);
    LOAD(il2cpp_method_get_param_count); LOAD(il2cpp_method_get_param); LOAD(il2cpp_method_get_return_type);
    LOAD(il2cpp_method_get_flags); LOAD(il2cpp_type_get_name); LOAD(il2cpp_free);
    LOAD(il2cpp_class_get_field_from_name); LOAD(il2cpp_field_get_type);
    LOAD(il2cpp_field_static_get_value); LOAD(il2cpp_field_static_set_value); LOAD(il2cpp_field_get_value);
    LOAD(il2cpp_runtime_class_init); LOAD(il2cpp_runtime_invoke); LOAD(il2cpp_object_unbox);
    LOAD(il2cpp_object_get_virtual_method); LOAD(il2cpp_class_from_type); LOAD(il2cpp_class_is_valuetype);
    LOAD(il2cpp_field_get_flags); LOAD(il2cpp_method_get_class); LOAD(il2cpp_class_get_namespace);
    LOAD(il2cpp_object_get_class); LOAD(il2cpp_class_is_assignable_from); LOAD(il2cpp_method_is_generic);
#undef LOAD
    return true;
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



// Pure compatibility rules used by the live binder and mutation tests.
static std::string CFAutoLeaf(const std::string& name) {
    const size_t p=name.find_last_of('.');return p==std::string::npos?name:name.substr(p+1);
}
static bool CFAutoTypeNameMatches(const std::string& actual,const std::string& expected) {
    if(actual==expected)return true;
    if(expected.rfind("System.",0)==0 || expected.find_first_of("[]&*")!=std::string::npos)return false;
    return expected.find('.')==std::string::npos && CFAutoLeaf(actual)==expected;
}
static bool CFAutoShape(const std::string& actualReturn,uint32_t flags,bool generic,
                        const std::vector<std::string>& actualParams,const char* expectedReturn,
                        bool expectedStatic,std::initializer_list<const char*> expectedParams) {
    if(generic || ((flags&0x10u)!=0)!=expectedStatic ||
       !CFAutoTypeNameMatches(actualReturn,expectedReturn) || actualParams.size()!=expectedParams.size())return false;
    size_t i=0;for(const char* type:expectedParams)if(actualParams[i++]!=type)return false;
    return true;
}
static bool CFAutoFieldStorage(uint32_t flags,bool expectedStatic) {
    return ((flags&0x10u)!=0)==expectedStatic;
}


static void* CFImage(const char* wanted) {
    size_t count=0;
    void** assemblies=il2cpp_domain_get_assemblies(il2cpp_domain_get(),&count);
    for (size_t i=0; assemblies && i<count; ++i) {
        void* image=il2cpp_assembly_get_image(assemblies[i]);
        const char* name=image?il2cpp_image_get_name(image):nullptr;
        if (name && !strcmp(name,wanted)) return image;
    }
    return nullptr;
}

static void* CFAutoClass(const char* ns,const char* name) {
    size_t count=0;void** source=il2cpp_domain_get_assemblies(il2cpp_domain_get(),&count);
    std::vector<void*> images;
    uintptr_t fingerprint=uintptr_t(count)+1;
    for(size_t i=0;source && i<count;++i){
        void* image=il2cpp_assembly_get_image(source[i]);if(!image)continue;
        images.push_back(image);fingerprint=(fingerprint*uintptr_t(1099511628211ULL))^uintptr_t(image)^il2cpp_image_get_class_count(image);
    }
    void* found=nullptr;
    for(void* image:images)if(void* k=il2cpp_class_from_name(image,ns,name)){
        if(found && found!=k){LOGE("Auto reject: duplicate full class %s.%s",ns,name);return nullptr;}
        found=k;
    }
    if(found)return found;
    static std::mutex missMutex;static std::map<std::string,uintptr_t> misses;
    const std::string key=std::string(ns)+"::"+name;
    {std::lock_guard<std::mutex> lock(missMutex);auto it=misses.find(key);if(it!=misses.end() && it->second==fingerprint)return nullptr;}
    for(void* image:images){
        const size_t classes=il2cpp_image_get_class_count(image);
        for(size_t i=0;i<classes;++i){
            void* k=il2cpp_image_get_class(image,i);const char* simple=k?il2cpp_class_get_name(k):nullptr;
            if(!simple || strcmp(simple,name))continue;
            if(found && found!=k){LOGE("Auto reject: ambiguous moved class %s",name);return nullptr;}
            found=k;
        }
    }
    if(found)LOGI("Auto rebound class %s.%s -> %s.%s",ns,name,il2cpp_class_get_namespace(found),il2cpp_class_get_name(found));
    else {std::lock_guard<std::mutex> lock(missMutex);misses[key]=fingerprint;}
    return found;
}
void* FindClass(const char* ns,const char* name){return CFAutoClass(ns?ns:"",name);}
void* FindClassAnywhere(const char* name){return CFAutoClass("",name);}
static void* CFClass(const char* ns,const char* name){return CFAutoClass(ns?ns:"",name);}
static std::string CFTypeName(void* type) {
    char* name=type?il2cpp_type_get_name(type):nullptr;
    std::string result=name?name:"";
    if (name) il2cpp_free(name);
    return result;
}
static void* CFMethod(void* klass,const char* name,std::initializer_list<const char*> params) {
    if (!klass) return nullptr;
    void* iter=nullptr; void* found=nullptr;
    while (void* method=il2cpp_class_get_methods(klass,&iter)) {
        const char* n=il2cpp_method_get_name(method);
        if (!n || strcmp(n,name) || il2cpp_method_is_generic(method) || il2cpp_method_get_param_count(method)!=params.size()) continue;
        uint32_t i=0; bool match=true;
        for (auto* expected:params) {
            if (CFTypeName(il2cpp_method_get_param(method,i++))!=expected) {match=false;break;}
        }
        if (!match) continue;
        if (found) {LOGE("Ambiguous exact method: %s",name);return nullptr;}
        found=method;
    }
    if (found) LOGI("Bound %s.%s(%zu) MI=%p entry=%p",il2cpp_class_get_name(klass),name,params.size(),found,*reinterpret_cast<void**>(found));
    else LOGE("Missing exact method: %s.%s(%zu)",il2cpp_class_get_name(klass),name,params.size());
    return found;
}

static void* CFBindMethod(void* klass,const char* name,std::initializer_list<const char*> params,const char* result,bool isStatic){
    void* method=CFMethod(klass,name,params);if(!method)return nullptr;
    uint32_t impl=0;const uint32_t flags=il2cpp_method_get_flags(method,&impl);
    std::vector<std::string> types;
    for(uint32_t i=0;i<il2cpp_method_get_param_count(method);++i)types.push_back(CFTypeName(il2cpp_method_get_param(method,i)));
    if(!CFAutoShape(CFTypeName(il2cpp_method_get_return_type(method)),flags,il2cpp_method_is_generic(method),types,result,isStatic,params)){
        LOGE("Auto reject: changed return/static/signature for %s.%s",il2cpp_class_get_name(klass),name);return nullptr;
    }
    if(std::string(result).rfind("System.",0)!=0){
        void* returnClass=il2cpp_class_from_type(il2cpp_method_get_return_type(method));
        if(!returnClass || il2cpp_class_is_valuetype(returnClass)){LOGE("Auto reject: expected reference return %s",name);return nullptr;}
    }
    return method;
}
static bool CFObjectCompatible(void* self,void* klass){
    void* actual=self?il2cpp_object_get_class(self):nullptr;
    return actual && klass && il2cpp_class_is_assignable_from(klass,actual);
}
static bool CFFieldSchema(void* klass,const char* name,const char* expectedType,bool isStatic){
    void* f=klass?il2cpp_class_get_field_from_name(klass,name):nullptr;
    return f && CFAutoFieldStorage(il2cpp_field_get_flags(f),isStatic) && CFAutoTypeNameMatches(CFTypeName(il2cpp_field_get_type(f)),expectedType);
}
static bool CFReferenceFieldSchema(void* klass,const char* name,void* base,bool isStatic){
    void* f=klass?il2cpp_class_get_field_from_name(klass,name):nullptr;
    if(!f || !CFAutoFieldStorage(il2cpp_field_get_flags(f),isStatic))return false;
    void* type=il2cpp_class_from_type(il2cpp_field_get_type(f));
    return type && !il2cpp_class_is_valuetype(type) && base && il2cpp_class_is_assignable_from(base,type);
}
void* FindMethod(void* klass,const char* name,int nParams) {
    if (!klass) return nullptr;
    void* it=nullptr;void* found=nullptr;
    while (void* m=il2cpp_class_get_methods(klass,&it)) {
        const char* n=il2cpp_method_get_name(m);
        if (n && !strcmp(n,name) && static_cast<int>(il2cpp_method_get_param_count(m))==nParams) {
            if (found) return nullptr;
            found=m;
        }
    }
    return found;
}
void* MethodAddr(const char* ns,const char* cls,const char* name,int count) {
    void* method=FindMethod(FindClass(ns,cls),name,count);
    return method?*reinterpret_cast<void**>(method):nullptr;
}
static void* CFInvokeObject(void* method,void* self,void** args,bool* ok=nullptr) {
    if (ok) *ok=false;
    if (!method) return nullptr;
    uint32_t impl=0;const uint32_t flags=il2cpp_method_get_flags(method,&impl);
    const bool isStatic=(flags&0x10u)!=0;
    if ((!isStatic && (!self || !CFObjectCompatible(self,il2cpp_method_get_class(method)))) || (isStatic && self)) {LOGE("Invoke invalid self: %s",il2cpp_method_get_name(method));return nullptr;}
    if (self && (flags&0x40u)) {
        method=il2cpp_object_get_virtual_method(self,method);
        if (!method) {LOGE("Virtual method unresolved");return nullptr;}
    }
    void* exception=nullptr;
    void* result=il2cpp_runtime_invoke(method,self,args,&exception);
    if (exception) {LOGE("Managed exception: %s exception=%p",il2cpp_method_get_name(method),exception);return nullptr;}
    if (ok) *ok=true;
    return result;
}
static bool CFInvoke(void* method,void* self,void** args) {
    bool ok=false;CFInvokeObject(method,self,args,&ok);return ok;
}
static bool CFInvokeInt64(void* method,void* self,void** args,int64_t& out) {
    if (!method || CFTypeName(il2cpp_method_get_return_type(method))!="System.Int64") return false;
    bool ok=false;void* box=CFInvokeObject(method,self,args,&ok);
    if (!ok || !box) return false;
    void* data=il2cpp_object_unbox(box);if (!data) return false;
    memcpy(&out,data,sizeof(out));return true;
}
static bool CFInvokeInt32(void* method,void* self,void** args,int32_t& out) {
    if (!method || CFTypeName(il2cpp_method_get_return_type(method))!="System.Int32") return false;
    bool ok=false;void* box=CFInvokeObject(method,self,args,&ok);
    if (!ok || !box) return false;
    void* data=il2cpp_object_unbox(box);if (!data) return false;
    memcpy(&out,data,sizeof(out));return true;
}
template<class T> static bool CFFieldTypeMatches(void* field) {
    void* type=field?il2cpp_field_get_type(field):nullptr;
    if (!type) return false;
    if constexpr (std::is_same<T,void*>::value) {
        void* klass=il2cpp_class_from_type(type);
        return klass && !il2cpp_class_is_valuetype(klass);
    }
    const std::string name=CFTypeName(type);
    if constexpr (std::is_same<T,bool>::value) return name=="System.Boolean";
    if constexpr (std::is_same<T,float>::value) return name=="System.Single";
    if constexpr (std::is_same<T,int32_t>::value) return name=="System.Int32";
    if constexpr (std::is_same<T,int64_t>::value) return name=="System.Int64";
    return false;
}
template<class T> static bool CFStaticGet(void* klass,const char* name,T& out) {
    if (!klass) return false;
    void* field=il2cpp_class_get_field_from_name(klass,name);
    if (!field || !CFAutoFieldStorage(il2cpp_field_get_flags(field),true) || !CFFieldTypeMatches<T>(field)) {LOGE("Static read type/storage mismatch: %s",name);return false;}
    il2cpp_runtime_class_init(klass);
    il2cpp_field_static_get_value(field,&out);return true;
}
template<class T> static bool CFStaticSet(void* klass,const char* name,const T& value) {
    if (!klass) return false;
    void* field=il2cpp_class_get_field_from_name(klass,name);
    if (!field || !CFAutoFieldStorage(il2cpp_field_get_flags(field),true) || (il2cpp_field_get_flags(field)&0x60u) || !CFFieldTypeMatches<T>(field)) {LOGE("Static write type/storage mismatch: %s",name);return false;}
    il2cpp_runtime_class_init(klass);
    T copy=value;il2cpp_field_static_set_value(field,&copy);return true;
}
template<class T> static bool CFFieldGet(void* self,void* klass,const char* name,T& out) {
    if (!self || !klass || !CFObjectCompatible(self,klass)) return false;
    void* field=il2cpp_class_get_field_from_name(klass,name);
    if (!field || !CFAutoFieldStorage(il2cpp_field_get_flags(field),false) || !CFFieldTypeMatches<T>(field)) {LOGE("Field read type/storage mismatch: %s",name);return false;}
    il2cpp_field_get_value(self,field,&out);return true;
}


// Native targets must keep their full ABI and an independent executable entry.
static bool CFAutoExecutable(void* address){
    uintptr_t p=reinterpret_cast<uintptr_t>(address);
    if(!p || (p&3u) || !g_il2cpp_phdr)return false;
    for(int i=0;i<g_il2cpp_phnum;++i){
        const auto& h=g_il2cpp_phdr[i];
        if(h.p_type==PT_LOAD && (h.p_flags&PF_X) && p>=il2cpp_base+h.p_vaddr && p<il2cpp_base+h.p_vaddr+h.p_memsz)return true;
    }
    return false;
}
static bool CFAutoNativeIndependent(void* klass,void* method){
    void* entry=method?*reinterpret_cast<void**>(method):nullptr;
    if(!CFAutoExecutable(entry))return false;
    void* it=nullptr;
    while(void* other=il2cpp_class_get_methods(klass,&it)){
        if(other!=method && *reinterpret_cast<void**>(other)==entry){
            LOGE("Auto reject: shared native bridge in %s",il2cpp_class_get_name(klass));return false;
        }
    }
    return true;
}
static void* CFAutoNativeMethod(void* klass,const char* cls,const char* name){
    if(!strcmp(cls,"Time") && !strcmp(name,"get_timeScale"))return CFBindMethod(klass,name,{},"System.Single",true);
    if(!strcmp(cls,"Time") && !strcmp(name,"set_timeScale"))return CFBindMethod(klass,name,{"System.Single"},"System.Void",true);
    if(!strcmp(cls,"UnitySynchronizationContext") && !strcmp(name,"ExecuteTasks"))return CFBindMethod(klass,name,{},"System.Void",true);
    if(!strcmp(cls,"HwAdsManager") && !strcmp(name,"ShowRewardedVideo"))return CFBindMethod(klass,name,{"System.String"},"System.Void",false);
    if(!strcmp(cls,"HwAdsManager") && !strcmp(name,"get_IsRewardAdLoaded"))return CFBindMethod(klass,name,{},"System.Boolean",false);
    return nullptr;
}
struct CFAutoNativeHook {void* entry;void* replacement;void* original;};
static std::mutex g_autoHooksMutex;
static std::vector<CFAutoNativeHook> g_autoHooks;
static bool CFAutoInstallHook(const char* ns,const char* cls,const char* name,int count,void* replacement,void** original,const char* counterName){
    void* klass=FindClass(ns,cls);
    void* method=klass?CFAutoNativeMethod(klass,cls,name):nullptr;
    if(!method || int(il2cpp_method_get_param_count(method))!=count || !CFAutoNativeIndependent(klass,method)){
        LOGE("Auto disabled incompatible hook %s.%s",cls,name);++g_hook_fail;return false;
    }
    void* entry=*reinterpret_cast<void**>(method);
    std::lock_guard<std::mutex> lock(g_autoHooksMutex);
    for(const auto& hook:g_autoHooks)if(hook.entry==entry){
        if(hook.replacement!=replacement){LOGE("Auto reject: hook address collision %s",counterName);return false;}
        *original=hook.original;return true;
    }
    if(DobbyHook(entry,replacement,original)!=0){++g_hook_fail;LOGE("Auto hook failed: %s.%s",cls,name);return false;}
    g_autoHooks.push_back({entry,replacement,*original});++g_hook_ok;
    HookCounter* counter=trace_counter(counterName);__atomic_store_n(&counter->hooked,true,__ATOMIC_RELEASE);
    LOGI("Auto hook OK: %s.%s @ %p",cls,name,entry);return true;
}
#undef T_HOOK
#define T_HOOK(ns,cls,method,nParams,fn) CFAutoInstallHook((ns),(cls),(method),(nParams),(void*)h_##fn,(void**)&o_##fn,#fn)

// DirectHook: bypass CFAutoNativeMethod whitelist + shared-entry check.
// Dùng cho hook bảo vệ (ban block, freeze coin) mà CFAutoInstallHook từ chối.
static bool DirectHook(const char* ns,const char* cls,const char* methodName,int nParams,
                       void* replacement,void** original,const char* counterName) {
    void* klass=FindClass(ns,cls);
    if (!klass){LOGE("DH: no class %s",cls);++g_hook_fail;return false;}
    void* method=FindMethod(klass,methodName,nParams);
    if (!method){LOGE("DH: no method %s.%s(%d)",cls,methodName,nParams);++g_hook_fail;return false;}
    void* entry=*reinterpret_cast<void**>(method);
    if (!entry){LOGE("DH: null entry %s.%s",cls,methodName);++g_hook_fail;return false;}
    {
        std::lock_guard<std::mutex> lock(g_autoHooksMutex);
        for (const auto& h:g_autoHooks)
            if (h.entry==entry && h.replacement==replacement){*original=h.original;++g_hook_ok;LOGI("DH reuse: %s.%s",cls,methodName);return true;}
    }
    if (DobbyHook(entry,replacement,original)!=0){++g_hook_fail;LOGE("DH: Dobby failed %s.%s",cls,methodName);return false;}
    {std::lock_guard<std::mutex> lock(g_autoHooksMutex);g_autoHooks.push_back({entry,replacement,*original});}
    ++g_hook_ok;
    HookCounter* counter=trace_counter(counterName);__atomic_store_n(&counter->hooked,true,__ATOMIC_RELEASE);
    LOGI("DH OK: %s.%s @ %p",cls,methodName,entry);return true;
}
#define D_HOOK(ns,cls,method,nParams,fn) DirectHook((ns),(cls),(method),(nParams),(void*)h_##fn,(void**)&o_##fn,#fn)

#include <cmath>
#include <cstdint>

namespace cf_combat_detail {
constexpr std::int32_t kSource = 999999;
enum Error : std::uint32_t {
    kResolve = 1, kRead = 2, kDamage = 4, kGod = 8,
    kAttackSpeed = 16, kMoveSpeed = 32, kOneHit = 64, kInvalidRate = 128
};
struct Metadata {
    void* gm = nullptr;
    void* manager = nullptr;
    void* liver = nullptr;
    void* damage = nullptr;
    void* revoke = nullptr;
    void* resist = nullptr;
    void* unresist = nullptr;
    void* heal = nullptr;
    void* attackSpeed = nullptr;
    void* moveSpeed = nullptr;
    bool ready = false;
};
static Metadata m;
static std::uint32_t errors = 0;
static bool Valid(float v) { return std::isfinite(v) && v > 0.0f; }
static bool Same(float a, float b) {
    return std::fabs(a-b) <= 0.000001f *
        std::fmax(1.0f, std::fmax(std::fabs(a), std::fabs(b)));
}
static bool Source(void* method, void* role) {
    std::int32_t id = kSource;
    void* args[] = {&id};
    return CFInvoke(method, role, args);
}
static bool LiveDamage(void* role, float rate) {
    if (!Source(m.revoke, role)) return false;
    if (Same(rate, 1.0f)) return true;
    std::int32_t id = kSource;
    void* args[] = {&id, &rate};
    return CFInvoke(m.damage, role, args);
}
static bool Damage(void* role, float desired) {
    float old = 1.0f;
    if (!CFStaticGet<float>(m.gm, "GMRoleAttackRate", old)) return false;
    if (Same(old, desired)) return true;
    if (role && !LiveDamage(role, desired)) {
        if (Valid(old)) LiveDamage(role, old);
        return false;
    }
    if (CFStaticSet<float>(m.gm, "GMRoleAttackRate", desired)) return true;
    if (role && Valid(old)) LiveDamage(role, old);
    return false;
}
static bool God(void* role, bool desired) {
    bool old = false;
    if (!CFStaticGet<bool>(m.gm, "GMRoleInvincible", old)) return false;
    if (old == desired) return true;
    if (!CFStaticSet<bool>(m.gm, "GMRoleInvincible", desired)) return false;
    if (!role) return true;
    bool ok = desired ? Source(m.resist, role) : Source(m.unresist, role);
    if (ok && desired) ok = CFInvoke(m.heal, role, nullptr);
    if (ok) return true;
    if (old) Source(m.resist, role); else Source(m.unresist, role);
    CFStaticSet<bool>(m.gm, "GMRoleInvincible", old);
    return false;
}
static bool Speed(void* role, float desired, const char* field, void* method) {
    float old = 1.0f;
    if (!CFStaticGet<float>(m.gm, field, old)) return false;
    if (Same(old, desired)) return true;
    if (role && !Valid(old)) return false;
    float ratio = role ? desired / old : 1.0f;
    if (!Valid(ratio)) return false;
    if (!CFStaticSet<float>(m.gm, field, desired)) return false;
    if (!role) return true;
    void* args[] = {&ratio};
    if (CFInvoke(method, role, args)) return true;
    CFStaticSet<float>(m.gm, field, old);
    return false;
}
} // namespace cf_combat_detail

static bool CFCombatResolve() {
    using namespace cf_combat_detail;
    if (m.ready) return true;
    m.gm = CFClass("", "UI_GMPanel");
    m.manager = CFClass("Adventure", "AdventureManager");
    m.liver = CFClass("Adventure", "Liver");
    if (!m.gm || !m.manager || !m.liver) { errors |= kResolve; return false; }
    m.damage = CFBindMethod(m.liver, "ChangeAttackDamageRate", {"System.Int32", "System.Single"}, "System.Void", false);
    m.revoke = CFBindMethod(m.liver, "RevokeChangeAttackDamageRate", {"System.Int32"}, "System.Void", false);
    m.resist = CFBindMethod(m.liver, "AddDamageResist", {"System.Int32"}, "System.Void", false);
    m.unresist = CFBindMethod(m.liver, "RemoveDamageResist", {"System.Int32"}, "System.Void", false);
    m.heal = CFBindMethod(m.liver, "RestoreFullHealth", {}, "System.Void", false);
    m.attackSpeed = CFBindMethod(m.liver, "ChangeAttackSpeedScale", {"System.Single"}, "System.Void", false);
    m.moveSpeed = CFBindMethod(m.liver, "ChangeMoveSpeedScale", {"System.Single"}, "System.Void", false);
    if (!m.damage || !m.revoke || !m.resist || !m.unresist || !m.heal ||
        !m.attackSpeed || !m.moveSpeed) { errors |= kResolve; return false; }
    if(!CFReferenceFieldSchema(m.manager,"PlayerRole",m.liver,true)){errors|=kResolve;return false;}
    float rate = 1.0f;
    bool flag = false;
    void* role = nullptr;
    m.ready = CFStaticGet<float>(m.gm, "GMRoleAttackRate", rate) &&
        CFStaticGet<float>(m.gm, "GMRoleAttackSpeedApplied", rate) &&
        CFStaticGet<float>(m.gm, "GMRoleMoveSpeedApplied", rate) &&
        CFStaticGet<bool>(m.gm, "GMRoleInvincible", flag) &&
        CFStaticGet<bool>(m.gm, "GMOneHitKill", flag) &&
        CFStaticGet<void*>(m.manager, "PlayerRole", role);
    if (!m.ready) errors |= kResolve;
    return m.ready;
}

static void CFCombatApply(float damage, bool god, float attackSpeed,
                          float moveSpeed, bool oneHit) {
    using namespace cf_combat_detail;
    errors = 0;
    if (!CFCombatResolve()) return;
    if (!Valid(damage) || !Valid(attackSpeed) || !Valid(moveSpeed)) {
        errors |= kInvalidRate; return;
    }
    void* role = nullptr;
    if (!CFStaticGet<void*>(m.manager, "PlayerRole", role)) { errors |= kRead; return; }
    if (!Damage(role, damage)) errors |= kDamage;
    if (!God(role, god)) errors |= kGod;
    if (!Speed(role, attackSpeed, "GMRoleAttackSpeedApplied", m.attackSpeed)) errors |= kAttackSpeed;
    if (!Speed(role, moveSpeed, "GMRoleMoveSpeedApplied", m.moveSpeed)) errors |= kMoveSpeed;
    bool oldOneHit = false;
    if (!CFStaticGet<bool>(m.gm, "GMOneHitKill", oldOneHit) ||
        (oldOneHit != oneHit && !CFStaticSet<bool>(m.gm, "GMOneHitKill", oneHit))) errors |= kOneHit;
}

static std::uint32_t CFCombatLastErrorMask() { return cf_combat_detail::errors; }
static void CFCombatInvalidate(){cf_combat_detail::m={};cf_combat_detail::errors=cf_combat_detail::kResolve;}


namespace cf_resources_detail {
struct Methods {void* data=nullptr;void* service=nullptr;void* getData=nullptr;void* count=nullptr;void* sync=nullptr;void* getValue=nullptr;void* setValue=nullptr;void* getRender=nullptr;void* setRender=nullptr;void* setDiamondRender=nullptr;void* save=nullptr;bool ready=false;};
static Methods m;static const char* message="Waiting for Coin metadata";static int granted=0;static bool attempted=false;
struct Snapshot{int64_t inventory=-1,archive=-1,render=-1;bool inventoryValid=false;};
static bool Number(void*& number){
    bool ok=false;void* data=CFInvokeObject(m.getData,nullptr,nullptr,&ok);
    return ok && data && CFFieldGet<void*>(data,m.data,"Coin",number) && number;
}
static bool Read(Snapshot& s){
    void* number=nullptr;if(!Number(number) || !CFInvokeInt64(m.getValue,number,nullptr,s.archive) || !CFInvokeInt64(m.getRender,nullptr,nullptr,s.render))return false;
    void* item=nullptr;int32_t id=2;void* args[]={&id};
    if(CFStaticGet<void*>(m.service,"Item",item) && item)s.inventoryValid=CFInvokeInt64(m.count,item,args,s.inventory) && s.inventory>=0;
    return true;
}
}
static bool CFResourcesResolve(){
    using namespace cf_resources_detail;if(m.ready)return true;
    Methods next;next.data=CFClass("","UserData");next.service=CFClass("","GService");
    void* item=CFClass("","ItemUnity");void* number=CFClass("Cavy.Serializable.XorPtr","LongXorPtr");
    void* render=CFClass("","UserDataRender");void* archive=CFClass("","GArchive");
    if(!next.data || !next.service || !item || !number || !render || !archive)return false;
    if(!CFReferenceFieldSchema(next.data,"Coin",number,false))return false;
    next.getData=CFBindMethod(next.data,"get_I",{},"UserData",true);
    next.count=CFBindMethod(item,"GetCount",{"System.Int32"},"System.Int64",false);
    next.sync=CFBindMethod(item,"SyncCount",{"System.Int32","System.Int64"},"System.Void",false);
    next.getValue=CFBindMethod(number,"get_Value",{},"System.Int64",false);
    next.setValue=CFBindMethod(number,"set_Value",{"System.Int64"},"System.Void",false);
    next.getRender=CFBindMethod(render,"get_Coin",{},"System.Int64",true);
    next.setRender=CFBindMethod(render,"set_Coin",{"System.Int64"},"System.Void",true);
    next.setDiamondRender=CFBindMethod(render,"set_Diamond",{"System.Int64"},"System.Void",true);
    next.save=CFBindMethod(archive,"SaveToLocal",{},"System.Void",true);
    next.ready=next.getData && next.count && next.sync && next.getValue && next.setValue && next.getRender && next.setRender && next.save;
    if(!next.ready)return false;
    m=next;message="Coin metadata compatible";return true;
}
static const char* CFResourcesLastMessage(){return cf_resources_detail::message;}
static int CFResourcesLastGranted(){return cf_resources_detail::granted;}
static bool CFResourcesGrant(int id,int amount){
    using namespace cf_resources_detail;granted=0;attempted=true;
    if(id!=2 || amount<1 || amount>900000000){message="Choose Coin amount 1..900000000";return false;}
    if(!CFResourcesResolve())return false;
    Snapshot before;if(!Read(before) || before.archive<0){message="Coin data not ready";return false;}
    if(before.archive>INT64_MAX-amount || (before.inventoryValid && before.inventory>INT64_MAX-amount)){message="Coin amount too large";return false;}
    void* number=nullptr;if(!Number(number))return false;
    int64_t target=before.archive+amount;void* valueArgs[]={&target};
    if(!CFInvoke(m.setValue,number,valueArgs)){message="Coin write failed";return false;}
    bool cacheOK=true;
    if(before.inventoryValid){
        void* item=nullptr;int32_t coin=2;int64_t count=before.inventory+amount;void* args[]={&coin,&count};
        cacheOK=CFStaticGet<void*>(m.service,"ItemSync",item) && item && CFInvoke(m.sync,item,args);
    }
    const bool renderOK=CFInvoke(m.setRender,nullptr,valueArgs);
    Snapshot after;
    if(!Read(after) || !renderOK || !cacheOK || after.archive!=target || after.render!=target ||
       (before.inventoryValid && (!after.inventoryValid || after.inventory!=before.inventory+amount))){message="Coin changed partially; check current amounts";return false;}
    granted=amount;
    LOGI("Auto Coin local: archive=%lld render=%lld item=%lld",(long long)after.archive,(long long)after.render,(long long)after.inventory);
    if(!CFInvoke(m.save,nullptr,nullptr)){message="Coin changed; local save failed";return false;}
    message="Local Coin/display changed; server persistence unverified";return true;
}
static bool CFResourcesAudit(){
    using namespace cf_resources_detail;if(!attempted || !CFResourcesResolve())return false;
    Snapshot s;if(!Read(s))return false;
    LOGI("Auto Coin audit: archive=%lld render=%lld item=%lld",(long long)s.archive,(long long)s.render,(long long)s.inventory);return true;
}
static void CFResourcesInvalidate(){cf_resources_detail::m={};}
static bool CFDiamondGrant(int amount) {
    using namespace cf_resources_detail;
    if (amount<1 || amount>900000000) {message="Choose Diamond amount 1..900000000";return false;}
    if (!CFResourcesResolve()) return false;
    bool ok=false;
    void* data=CFInvokeObject(m.getData,nullptr,nullptr,&ok);
    if (!ok || !data) {message="Diamond: UserData not ready";return false;}
    void* number=nullptr;
    if (!CFFieldGet<void*>(data,m.data,"Diamond",number) || !number) {message="Diamond field not found";return false;}
    int64_t current=0;
    if (!CFInvokeInt64(m.getValue,number,nullptr,current)) {message="Diamond read failed";return false;}
    if (current>INT64_MAX-amount) {message="Diamond amount too large";return false;}
    const int64_t target=current+amount;
    void* valueArgs[]={const_cast<int64_t*>(&target)};
    if (!CFInvoke(m.setValue,number,valueArgs)) {message="Diamond write failed";return false;}
    if (m.setDiamondRender) CFInvoke(m.setDiamondRender,nullptr,valueArgs);
    if (!CFInvoke(m.save,nullptr,nullptr)) {message="Diamond changed; save failed";return false;}
    LOGI("Diamond grant: amount=%d current=%lld target=%lld",amount,(long long)current,(long long)target);
    message="Diamond changed; server persistence unverified";return true;
}

static void CFAttackRangeApply(float multiplier) {
    using namespace cf_combat_detail;
    if (!m.ready) return;
    float baseRange=0.0f;
    if (!CFStaticGet<float>(m.liver,"BaseAttackRangeRadius",baseRange)||baseRange<=0.0f) return;
    void* role=nullptr;
    if (!CFStaticGet<void*>(m.manager,"PlayerRole",role)||!role) return;
    *reinterpret_cast<float*>(reinterpret_cast<char*>(role)+0x1E8)=baseRange*multiplier;
}

#include <atomic>
#include <stdint.h>
#include <time.h>

namespace cf_ads_detail {
static std::atomic<bool> hooksReady{false}, enabled{false}, adapterReady{false};
static std::atomic<bool> pending{false};
static void* managerClass=nullptr;
static void* adapterClass=nullptr;
static void* readyMI=nullptr;
static void* setReadyMI=nullptr;
static void* completeMI=nullptr;
static bool logicBound=false;
static bool forcedReady=false;
static uint64_t nextBind=0, nextReadiness=0;
static const char* lastMessage="Rewarded-video test not initialized";
static uint64_t Milliseconds() {
    timespec ts{};clock_gettime(CLOCK_MONOTONIC,&ts);
    return uint64_t(ts.tv_sec)*1000u+uint64_t(ts.tv_nsec/1000000);
}
static bool SetReady(void* adapter,bool value) {
    void* args[]={&value};
    return CFInvoke(setReadyMI,adapter,args);
}
static bool BindLogic() {
    if (logicBound) return true;
    adapterClass=CFClass("","HwAdapterBridge");
    if (!adapterClass) return false;
    setReadyMI=CFBindMethod(adapterClass,"SetRewardAdReady",{"System.Boolean"},"System.Void",false);
    completeMI=CFBindMethod(adapterClass,"CompleteRewardRequest",{"System.Boolean"},"System.Void",false);
    logicBound=setReadyMI && completeMI &&
        CFReferenceFieldSchema(adapterClass,"Instance",adapterClass,true) &&
        CFFieldSchema(adapterClass,"_initialized","System.Boolean",false) &&
        CFFieldSchema(adapterClass,"_requestCompleted","System.Boolean",false) &&
        CFFieldSchema(adapterClass,"_rewardAdReady","System.Boolean",false) &&
        CFFieldSchema(adapterClass,"_onAdPlaySuccess","System.Action",false);
    if (logicBound) LOGI("ADS READY: game callback adapter resolved");
    return logicBound;
}
}
static void (*o_AdsShow)(void*,void*,void*)=nullptr;
static bool (*o_AdsReady)(void*,void*)=nullptr;

static void h_AdsShow(void* self,void* point,void* mi) {
    T_COUNT(AdsShow);
    using namespace cf_ads_detail;
    if (!self || !hooksReady.load() || !enabled.load() || !adapterReady.load()) {
        if (o_AdsShow) o_AdsShow(self,point,mi);
        return;
    }
    if (!pending.exchange(true)) LOGI("No Ads: queued local rewarded-video completion");
}
static bool h_AdsReady(void* self,void* mi) {
    T_COUNT(AdsReady);
    using namespace cf_ads_detail;
    if (self && hooksReady.load() && enabled.load() && adapterReady.load()) return true;
    return o_AdsReady?o_AdsReady(self,mi):false;
}


static bool CFAdsInstallHooks() {
    using namespace cf_ads_detail;
    managerClass=FindClass("Game.HwSDK","HwAdsManager");
    if (!managerClass || !CFReferenceFieldSchema(managerClass,"_instance",managerClass,true)) return false;
    void* show=CFBindMethod(managerClass,"ShowRewardedVideo",{"System.String"},"System.Void",false);
    readyMI=CFBindMethod(managerClass,"get_IsRewardAdLoaded",{},"System.Boolean",false);
    if (!show || !readyMI ||
        CFTypeName(il2cpp_method_get_return_type(show))!="System.Void" ||
        CFTypeName(il2cpp_method_get_return_type(readyMI))!="System.Boolean") return false;
    uint32_t impl=0;
    if ((il2cpp_method_get_flags(show,&impl)&0x10u) ||
        (il2cpp_method_get_flags(readyMI,&impl)&0x10u)) return false;
    const bool showOK=T_HOOK("Game.HwSDK","HwAdsManager","ShowRewardedVideo",1,AdsShow);
    const bool readyOK=T_HOOK("Game.HwSDK","HwAdsManager","get_IsRewardAdLoaded",0,AdsReady);
    const bool ok=showOK && readyOK && o_AdsShow && o_AdsReady;
    hooksReady.store(ok);
    if (!ok) LOGE("No Ads unavailable: AOT SDK hooks failed");
    return ok;
}

static void CFAdsTick(bool requested) {
    using namespace cf_ads_detail;
    enabled.store(requested);
    if (!hooksReady.load()) return;
    const uint64_t now=Milliseconds();
    if (!logicBound && now>=nextBind) {nextBind=now+1000;BindLogic();}
    if (!logicBound) return;
    void* adapter=nullptr;
    bool initialized=false;
    if (!CFStaticGet<void*>(adapterClass,"Instance",adapter) || !adapter ||
        !CFFieldGet<bool>(adapter,adapterClass,"_initialized",initialized) || !initialized) {
        adapterReady.store(false);return;
    }
    adapterReady.store(true);

    if (pending.exchange(false)) {
        bool done=true;void* callback=nullptr;
        const bool valid=CFFieldGet<bool>(adapter,adapterClass,"_requestCompleted",done) &&
            CFFieldGet<void*>(adapter,adapterClass,"_onAdPlaySuccess",callback) && !done && callback;
        bool success=true;void* args[]={&success};
        if (valid && CFInvoke(completeMI,adapter,args)) {
            lastMessage="Reward request completed locally without showing SDK video";
            LOGI("No Ads: game reward callback completed; SDK video not requested");
        } else {
            lastMessage="Reward callback was absent or could not be completed";
            LOGE("No Ads: pending callback was not completed");
        }
    }
    if (requested && now>=nextReadiness) {
        nextReadiness=now+100;
        forcedReady=SetReady(adapter,true) || forcedReady;
    } else if (!requested && forcedReady) {
        void* manager=nullptr;
        bool actual=false;
        if (CFStaticGet<void*>(managerClass,"_instance",manager) && CFObjectCompatible(manager,managerClass) && o_AdsReady)
            actual=o_AdsReady(manager,readyMI);
        if (SetReady(adapter,actual)) forcedReady=false;
    }
}
static bool CFAdsReady() {return cf_ads_detail::hooksReady.load() && cf_ads_detail::adapterReady.load();}
static const char* CFAdsLastMessage() {return cf_ads_detail::lastMessage;}
static bool CFAdsHooksReady(){return cf_ads_detail::hooksReady.load();}

// --- No Miss (AddIsIgnoreDodgeState on player Role every 2s) ---
namespace cf_nomiss_detail {
struct Metadata {
    void* adMgrKlass=nullptr;
    void* addIgnoreDodgeMI=nullptr;
    bool ready=false;
};
static Metadata m;
}
static bool CFNoMissResolve() {
    using namespace cf_nomiss_detail;
    if (m.ready) return true;
    void* adMgr=CFClass("Adventure","AdventureManager");
    void* liver=CFClass("Adventure","Liver");
    if (!adMgr||!liver) return false;
    void* mi=CFMethod(liver,"AddIsIgnoreDodgeState",{"System.Int32"});
    if (!mi) return false;
    m.adMgrKlass=adMgr;m.addIgnoreDodgeMI=mi;
    m.ready=true;LOGI("NoMiss resolved");return true;
}
static void CFNoMissApply() {
    using namespace cf_nomiss_detail;
    if (!m.ready) return;
    void* playerRole=nullptr;
    if (!CFStaticGet<void*>(m.adMgrKlass,"PlayerRole",playerRole)||!playerRole) return;
    int32_t id=9999;void* args[1]={&id};
    CFInvoke(m.addIgnoreDodgeMI,playerRole,args);
}
static void CFNoMissInvalidate() { cf_nomiss_detail::m={}; }

// --- Đóng băng vàng (Coin) ---
static void (*o_SetCoin)(void*,int64_t,void*) = nullptr;
static void h_SetCoin(void* self,int64_t value,void* mi) {
    T_COUNT(SetCoin);
    if (o_SetCoin) o_SetCoin(self,value,mi);
}
static int64_t (*o_GetCoin)(void*,void*) = nullptr;
static int64_t h_GetCoin(void* self,void* mi) {
    T_COUNT(GetCoin);
    return o_GetCoin ? o_GetCoin(self,mi) : 0;
}

// --- GCheat spoof + ACTk block ---
// MI_Install: thay methodPointer trong MethodInfo, KHONG patch bytes function
// Game quet function thay bytes nguyen ven -> khong phat hien
static void* MI_Install(const char* ns, const char* cls, const char* name, int argc, void* hookFn) {
    void* klass = ns && ns[0] ? FindClass(ns,cls) : FindClassAnywhere(cls);
    if (!klass) { LOGE("MI: class %s not found",cls); return nullptr; }
    void* mi = FindMethod(klass,name,argc);
    if (!mi) { LOGE("MI: %s.%s(%d) not found",cls,name,argc); return nullptr; }
    uintptr_t* ptr = (uintptr_t*)mi;
    uintptr_t page = (uintptr_t)ptr & ~(uintptr_t)(getpagesize()-1);
    mprotect((void*)page, getpagesize()*2, PROT_READ|PROT_WRITE);
    void* orig = (void*)*ptr;
    *ptr = (uintptr_t)hookFn;
    LOGI("MI_HOOK: %s.%s @ %p",cls,name,hookFn);
    return orig;
}

static bool (*o_GCheatBan)(void*,int,void*,void*) = nullptr;
static void (*o_GCheatTriggerBan)(void*,void*,void*) = nullptr;
static void (*o_ACTkSpeedHack)(void*,void*) = nullptr;
static void (*o_ACTkObscured)(void*,void*) = nullptr;
static void (*o_ACTkInjection)(void*,void*) = nullptr;
static std::atomic<bool> g_blockACTk{true};

// Spoof: tra true (ban ok) nhung khong goi original -> khong ban thuc su
static bool h_GCheatBan(void* self,int reason,void* msg,void* mi) {
    T_COUNT(GCheatBan);
    LOGI("BanSpoof: Ban(reason=%d) -> true",reason);
    return true;
}
// Spoof: goi original voi reason=nullptr -> gui server bao khong co vi pham
static void h_GCheatTriggerBan(void* self,void* reason,void* mi) {
    T_COUNT(GCheatTriggerBan);
    if (o_GCheatTriggerBan) o_GCheatTriggerBan(self,nullptr,mi);
}
static void h_ACTkSpeedHack(void* self,void* mi) {
    T_COUNT(ACTkSpeedHack);
    if (!g_blockACTk.load()&&o_ACTkSpeedHack) o_ACTkSpeedHack(self,mi);
}
static void h_ACTkObscured(void* self,void* mi) {
    T_COUNT(ACTkObscured);
    if (!g_blockACTk.load()&&o_ACTkObscured) o_ACTkObscured(self,mi);
}
static void h_ACTkInjection(void* self,void* mi) {
    T_COUNT(ACTkInjection);
    if (!g_blockACTk.load()&&o_ACTkInjection) o_ACTkInjection(self,mi);
}

static bool CFBanBlockInstall() {
    // MI_Install = thay MethodInfo pointer, khong dung Dobby
    o_GCheatBan=(decltype(o_GCheatBan))MI_Install("","GCheat","Ban",2,(void*)h_GCheatBan);
    o_GCheatTriggerBan=(decltype(o_GCheatTriggerBan))MI_Install("","GCheat","TriggerBan",1,(void*)h_GCheatTriggerBan);
    bool a1=D_HOOK("CodeStage.AntiCheat.Detectors","SpeedHackDetector","StartDetectionAutomatically",0,ACTkSpeedHack);
    bool a2=D_HOOK("CodeStage.AntiCheat.Detectors","ObscuredCheatingDetector","StartDetectionAutomatically",0,ACTkObscured);
    bool a3=D_HOOK("CodeStage.AntiCheat.Detectors","InjectionDetector","StartDetectionAutomatically",0,ACTkInjection);
    LOGI("BanBlock: MIBan=%d MITrigger=%d | ACTk Speed=%d Obscured=%d Inject=%d",
         o_GCheatBan!=nullptr,o_GCheatTriggerBan!=nullptr,a1,a2,a3);
    return true;
}

static std::atomic<bool> g_published{false}, g_speedReady{false}, g_speedDirty{true};
static std::atomic<float> g_speed{1.0f}, g_gameScale{1.0f};
static std::atomic<bool> g_scaleSeeded{false}, g_god{false}, g_oneHit{false}, g_noAds{false};
static std::atomic<int> g_damage{1}, g_attackSpeed{1}, g_moveSpeed{1}, g_amount{100};
static std::atomic<bool> g_combatReady{false},g_resourcesReady{false};
static std::atomic<int> g_attackRange{1};
static std::atomic<bool> g_noMiss{false};
struct CFResourceJob { int id; int count; };
static std::mutex g_jobsMutex;
static std::deque<CFResourceJob> g_jobs;
static uint64_t g_auditStart=0;
static unsigned g_auditStage=3;
static void* g_getTSMI=nullptr;
static void* g_setTSMI=nullptr;
static float (*o_getTS)(void*)=nullptr;
static void (*o_setTS)(float,void*)=nullptr;
static void (*o_ExecuteTasks)(void*)=nullptr;
static uint64_t CFMilliseconds() {
    timespec ts{};clock_gettime(CLOCK_MONOTONIC,&ts);
    return uint64_t(ts.tv_sec)*1000u+uint64_t(ts.tv_nsec/1000000);
}
static void h_setTS(float value,void* mi) {
    T_COUNT(setTS);
    if (!o_setTS) return;
    if (!g_published.load() || !g_speedReady.load()) {o_setTS(value,mi);return;}
    if (std::isfinite(value) && value>=0) {
        g_gameScale.store(value);g_scaleSeeded.store(true);
        o_setTS(value*g_speed.load(),mi);
    } else o_setTS(value,mi);
}
static float h_getTS(void* mi) {
    T_COUNT(getTS);
    if (!o_getTS) return 1;
    if (!g_published.load() || !g_speedReady.load()) return o_getTS(mi);
    if (!g_scaleSeeded.load()) {
        float actual=o_getTS(mi);
        if (std::isfinite(actual) && actual>=0) {g_gameScale.store(actual);g_scaleSeeded.store(true);}
        return actual;
    }
    return g_gameScale.load();
}
static void CFApplyGameSpeed() {
    if (!g_scaleSeeded.load()) {
        float actual=o_getTS(g_getTSMI);
        if (!std::isfinite(actual) || actual<0) return;
        g_gameScale.store(actual);g_scaleSeeded.store(true);
    }
    const float base=g_gameScale.load(),multiplier=g_speed.load();
    o_setTS(base*multiplier,g_setTSMI);
    LOGI("Speed: multiplier=%.0f base=%.2f actual=%.2f",multiplier,base,base*multiplier);
}
static void CFGameplayTick() {
    static uint64_t nextBind=0,nextApply=0;
    static uint32_t previousError=UINT32_MAX;
    static bool waitingMessage=false;
    static uint64_t nextAdsBind=0;
    const uint64_t now=CFMilliseconds();
    if(!CFAdsHooksReady() && now>=nextAdsBind){nextAdsBind=now+5000;CFAdsInstallHooks();}
    CFAdsTick(g_noAds.load());
    const uint64_t auditOffsets[]={200,1000,3000};
    if (g_auditStage<3 && now>=g_auditStart+auditOffsets[g_auditStage]) {
        CFResourcesAudit();++g_auditStage;
    }
    if ((!g_combatReady.load() || !g_resourcesReady.load()) && now>=nextBind) {
        nextBind=now+2000;
        if (CFClass("", "UI_GMPanel") || CFClass("", "GService")) {
            if (!g_combatReady.load() && CFCombatResolve()) {
                g_combatReady.store(true);LOGI("COMBAT READY: damage/god/attack/move/onehit");
            }
            if (!g_resourcesReady.load() && CFResourcesResolve()) {
                g_resourcesReady.store(true);LOGI("RESOURCES READY: Coin local action");
            }
        } else if (!waitingMessage) {waitingMessage=true;LOGI("Auto waiting for compatible game classes in loaded assemblies");}
    }
    if (g_combatReady.load() && now>=nextApply) {
        nextApply=now+250;
        CFCombatApply(float(g_damage.load()),g_god.load(),float(g_attackSpeed.load()),float(g_moveSpeed.load()),g_oneHit.load());
        CFAttackRangeApply(float(g_attackRange.load()));
        const uint32_t error=CFCombatLastErrorMask();
        if (error!=previousError) {previousError=error;LOGI("Combat apply errorMask=%u",error);}
        if(error){CFCombatInvalidate();g_combatReady.store(false);nextBind=now+5000;LOGE("Auto combat suspended; will resolve compatible metadata again");}
    }
    if (g_resourcesReady.load()) {
        CFResourceJob job{};bool present=false;
        {
            std::lock_guard<std::mutex> lock(g_jobsMutex);
            if (!g_jobs.empty()) {job=g_jobs.front();g_jobs.pop_front();present=true;}
        }
        if (present) {
            T_STEP("resource id=%d count=%d",job.id,job.count);
            bool ok=CFResourcesGrant(job.id,job.count);
            g_auditStart=now;g_auditStage=0;
            LOGI("Resource id=%d requested=%d granted=%d ok=%d: %s",job.id,job.count,CFResourcesLastGranted(),ok,CFResourcesLastMessage());
            if(!ok){CFResourcesInvalidate();g_resourcesReady.store(false);nextBind=now+5000;}
        }
    }
    // No Miss: add IgnoreDodge state to player every 2s
    {
        static uint64_t nextNoMiss=0;
        if (g_noMiss.load() && now>=nextNoMiss) {
            nextNoMiss=now+2000;
            T_STEP("no miss apply");
            if (CFNoMissResolve()) CFNoMissApply();
        }
    }
}
static void h_ExecuteTasks(void* mi) {
    T_COUNT(ExecuteTasks);
    if (o_ExecuteTasks) o_ExecuteTasks(mi);
    if (!g_published.load()) return;
    try {
        if (g_speedReady.load() && g_speedDirty.exchange(false)) CFApplyGameSpeed();
        CFGameplayTick();
    } catch (...) {
        LOGE("Native exception during main-thread action");
    }
}
static bool CFUnityStatic(void* klass,const char* name,std::initializer_list<const char*> args,const char* result,void*& method) {
    method=CFBindMethod(klass,name,args,result,true);
    if (!method || CFTypeName(il2cpp_method_get_return_type(method))!=result) return false;
    uint32_t impl=0;return (il2cpp_method_get_flags(method,&impl)&0x10u)!=0;
}
static void ResolveAll() {
    void* time=FindClass("UnityEngine","Time");
    void* sync=FindClass("UnityEngine","UnitySynchronizationContext");
    void* execute=nullptr;
    bool speed=CFUnityStatic(time,"get_timeScale",{},"System.Single",g_getTSMI) &&
        CFUnityStatic(time,"set_timeScale",{"System.Single"},"System.Void",g_setTSMI);
    if (!CFUnityStatic(sync,"ExecuteTasks",{},"System.Void",execute)) {LOGE("No Unity main-thread hook");TraceReport();return;}
    const bool mainHook=T_HOOK("UnityEngine","UnitySynchronizationContext","ExecuteTasks",0,ExecuteTasks);
    if (!mainHook || !o_ExecuteTasks) {TraceReport();return;}
    if (speed) {
        const bool getOK=T_HOOK("UnityEngine","Time","get_timeScale",0,getTS);
        const bool setOK=T_HOOK("UnityEngine","Time","set_timeScale",1,setTS);
        g_speedReady.store(getOK && setOK && o_getTS && o_setTS);
    }
    D_HOOK("","UserInfoUnity","set_Coin",1,SetCoin);
    D_HOOK("","UserDataRender","get_Coin",0,GetCoin);
    CFAdsInstallHooks();
    CFBanBlockInstall();
    TraceReport();
    g_published.store(true);
}
jobjectArray GetFeatureList(JNIEnv* env,jobject) {
    const char* features[]={
        OBFUSCATE("Category_Champion Forge - Chien Dau"),
        OBFUSCATE("102_Toggle_Bat Tu"),
        OBFUSCATE("100_SeekBar_Toc Do Game_1_50"),
        OBFUSCATE("103_SeekBar_Toc Do Danh_1_10"),
        OBFUSCATE("104_SeekBar_Toc Do Chay_1_10"),
        OBFUSCATE("101_InputValue_Dame Vat Ly - Mac Dinh 1"),
        OBFUSCATE("105_Toggle_One Hit - Quai"),
        OBFUSCATE("113_SeekBar_Danh Xa_1_50"),
        OBFUSCATE("117_Toggle_Danh Luon Trung (No Miss)"),
        OBFUSCATE("Category_Tai Nguyen"),
        OBFUSCATE("106_InputValue_So Luong - Mac Dinh 900000"),
        OBFUSCATE("107_Button_Cong Coin"),
        OBFUSCATE("RichTextView_Coin thay doi tren may. May chu co the dong bo lai."),
        OBFUSCATE("Category_Quang Cao"),
        OBFUSCATE("110_Toggle_Nhan Thuong Khong Xem Video"),
    };
    const jsize count=sizeof(features)/sizeof(features[0]);
    jclass stringClass=env->FindClass("java/lang/String");if (!stringClass) return nullptr;
    jobjectArray result=env->NewObjectArray(count,stringClass,nullptr);env->DeleteLocalRef(stringClass);
    if (!result) return nullptr;
    for (jsize i=0;i<count;++i) {
        jstring text=env->NewStringUTF(features[i]);if (!text) return result;
        env->SetObjectArrayElement(result,i,text);env->DeleteLocalRef(text);
    }
    return result;
}
void Changes(JNIEnv*,jclass,jobject,jint number,jstring,jint value,jboolean enabled,jstring) {
    T_STEP("menu id=%d value=%d enabled=%d",number,value,enabled);
    switch (number) {
    case 100:g_speed.store(float(std::clamp<int>(value,1,50)));g_speedDirty.store(true);break;
    case 101:g_damage.store(std::clamp<int>(value,1,999999999));break;
    case 102:g_god.store(enabled==JNI_TRUE);break;
    case 103:g_attackSpeed.store(std::clamp<int>(value,1,10));break;
    case 104:g_moveSpeed.store(std::clamp<int>(value,1,10));break;
    case 105:g_oneHit.store(enabled==JNI_TRUE);break;
    case 110:g_noAds.store(enabled==JNI_TRUE);break;
    case 106:g_amount.store(std::clamp<int>(value,1,900000));break;
    case 107:{
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        if (g_jobs.size()<8) g_jobs.push_back({2,g_amount.load()});
        else LOGE("Resource queue is full");
        break;
    }
    case 113:g_attackRange.store(std::clamp<int>(value,1,50));break;
    case 117:g_noMiss.store(enabled==JNI_TRUE);break;
    default:return;
    }
    LOGI("Menu id=%d value=%d enabled=%d",number,value,enabled);
}
static std::atomic<bool> g_il2cpp_ready{false};
static int (*o_il2cpp_init)(const char*)=nullptr;
static int (*o_il2cpp_init_utf16)(const uint16_t*)=nullptr;
static int h_il2cpp_init(const char* name) {
    T_COUNT(Il2CppInit);
    int r=o_il2cpp_init?o_il2cpp_init(name):0;
    if (o_il2cpp_init && r) g_il2cpp_ready.store(true);
    return r;
}
static int h_il2cpp_init_utf16(const uint16_t* name) {
    T_COUNT(Il2CppInitUtf16);
    int r=o_il2cpp_init_utf16?o_il2cpp_init_utf16(name):0;
    if (o_il2cpp_init_utf16 && r) g_il2cpp_ready.store(true);
    return r;
}
static void* h_t(void*) {
    T_STEP("wait libil2cpp");
    while (!IsIL2CppLoaded()) usleep(100000);
    LOGI("Champion Forge Auto A1 built %s %s base=0x%" PRIxPTR,__DATE__,__TIME__,il2cpp_base);
    if (!InitIL2CppAPI()) return nullptr;
    TRACE_COUNTER(Il2CppInit);TRACE_COUNTER(Il2CppInitUtf16);
    void* init=ElfSym("il2cpp_init");void* init16=ElfSym("il2cpp_init_utf16");
    bool hooked=init && !DobbyHook(init,(void*)h_il2cpp_init,(void**)&o_il2cpp_init);
    bool hooked16=init16 && !DobbyHook(init16,(void*)h_il2cpp_init_utf16,(void**)&o_il2cpp_init_utf16);
    LOGI("Hook il2cpp_init: %s; init_utf16: %s",hooked?"OK":"FAIL",hooked16?"OK":"FAIL");
    if (!hooked && !hooked16) return nullptr;
    while (!g_il2cpp_ready.load()) usleep(100000);
    LOGI("il2cpp_init xong");TraceSleep(5);
    void* domain=il2cpp_domain_get();
    void* thread=domain?il2cpp_thread_attach(domain):nullptr;
    if (!thread) {LOGE("No initialized domain/thread");return nullptr;}
    size_t last=0;int stable=0;
    for (int i=0;i<600 && stable<5;++i) {
        size_t n=0;void** list=il2cpp_domain_get_assemblies(domain,&n);
        if (list && n && n==last) ++stable;else {stable=0;last=n;}
        usleep(100000);
    }
    if (stable>=5) {
        LOGI("Assemblies on dinh: %zu",last);
        ResolveAll();isGameReady.store(g_published.load());
        LOGI("MOD READY: mainThread=%d speed=%d",g_published.load(),g_speedReady.load());
        TraceStartCallReports();
    } else LOGE("Assemblies did not stabilize");
    il2cpp_thread_detach(thread);return nullptr;
}
extern "C" JNIEXPORT void JNICALL
Java_com_android_support_Main_CheckOverlayPermission(JNIEnv*,jclass,jobject) {}
__attribute__((constructor)) void l_m() {
    LOGI("Champion Forge Auto A1 injected");
    pthread_t thread{};
    if (!pthread_create(&thread,nullptr,h_t,nullptr)) pthread_detach(thread);
}
#include "Includes/JNI_Boilerplate.h"
