// HAMOD — Hải Tặc Truyền Kỳ (com.hhkb.rpg.gp)
// Unity IL2CPP + toLua hybrid
// Features:
//   index 0  SeekBar  Sát thương ×1-99999   → Lua hook GetHurtVal (team-aware)
//   index 1  SeekBar  Tốc độ x1/x3/x5/x10  → IL2CPP hook Time.set/get_timeScale
//   index 2  Toggle   Bỏ quảng cáo          → Lua hook Advertisement.sc_advideo_begin
//                                              patch KLLSDK.AD.ShowRewardAd
// ─────────────────────────────────────────────────────────────────────────────

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
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <atomic>
#include <cmath>
#include <map>
#include <string>
#include <vector>
#include <mutex>
#include <android/log.h>
#include "Includes/obfuscate.h"
#include <Dobby/dobby.h>

#define BUILD_ID "HTHK_v1"
#define LOG_TAG  "HTHK_MOD"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ── 1. State (std::atomic — 3 features theo index) ────────────────────────
static std::atomic<float> g_dmg{1.0f};          // index 0: damage ×1-99999
static std::atomic<float> g_spd{0.0f};          // index 1: speed index 0-3
static std::atomic<float> g_ads{0.0f};          // index 2: skip ads 0/1
static std::atomic<bool>  g_config_loaded_once{false};
static std::atomic<float> g_ts_game{1.0f};      // theo dõi timeScale game tự đặt
static const float        SPD_MUL[4] = {1.0f, 3.0f, 5.0f, 10.0f};

// ── 2. Lua API (từ libtolua.so) ────────────────────────────────────────────
typedef int (*lua_CFunction)(void*);
static void        (*l_pushcclosure)(void*, lua_CFunction, int) = nullptr;
static void        (*l_setglobal)(void*, const char*)           = nullptr;
static void        (*l_setfield)(void*, int, const char*)       = nullptr;
static void        (*l_pushnumber)(void*, double)               = nullptr;
static double      (*l_tonumber)(void*, int)                    = nullptr;
static const char* (*l_tolstring)(void*, int, size_t*)          = nullptr;
static void        (*l_settop)(void*, int)                      = nullptr;
static int         (*l_gettop)(void*)                           = nullptr;
#define L_GLOBALSINDEX (-10002)  // Lua 5.1

// ── 3. Lua module buffer store ─────────────────────────────────────────────
static std::map<std::string, std::vector<char>> g_orig_bufs;
static std::mutex g_buf_mtx;

// ── 4. Lua C helper functions ──────────────────────────────────────────────
static int c_hthk_log(void* L) {
    size_t n = 0;
    const char* msg = l_tolstring ? l_tolstring(L, 1, &n) : nullptr;
    if (msg) LOGI("[LUA] %s", msg);
    return 0;
}

static int c_hthk_num(void* L) {
    int idx = l_tonumber ? (int)l_tonumber(L, 1) : 0;
    double val = 0;
    switch (idx) {
        case 0: val = (double)g_dmg.load(std::memory_order_relaxed); break;
        case 1: val = (double)g_spd.load(std::memory_order_relaxed); break;
        case 2: val = (double)g_ads.load(std::memory_order_relaxed); break;
    }
    if (l_pushnumber) l_pushnumber(L, val);
    return 1;
}

typedef int (*t_lua_load2)(void*, const char*, size_t, const char*, const char*);
static t_lua_load2 o_lua_load_fwd = nullptr;

static int c_hthk_orig_load(void* L) {
    int top = l_gettop ? l_gettop(L) : -1;
    size_t nl = 0;
    const char* name = l_tolstring ? l_tolstring(L, 1, &nl) : nullptr;
    if (!name) return 0;
    std::unique_lock<std::mutex> lock(g_buf_mtx);
    auto it = g_orig_bufs.find(std::string(name));
    if (it == g_orig_bufs.end()) return 0;
    std::vector<char> copy = it->second;
    lock.unlock();
    if (!o_lua_load_fwd) return 0;
    int r = o_lua_load_fwd(L, copy.data(), copy.size(), name, "bt");
    if (r != 0) { if (l_settop && top >= 0) l_settop(L, top); return 0; }
    return 1;
}

static void lua_reg_helpers(void* L) {
    if (!l_pushcclosure) return;
    if (l_setglobal) {
        l_pushcclosure(L, c_hthk_num, 0);       l_setglobal(L, "__hthk_num");
        l_pushcclosure(L, c_hthk_orig_load, 0); l_setglobal(L, "__hthk_orig_load");
        l_pushcclosure(L, c_hthk_log, 0);       l_setglobal(L, "__hthk_log");
    } else if (l_setfield) {
        l_pushcclosure(L, c_hthk_num, 0);       l_setfield(L, L_GLOBALSINDEX, "__hthk_num");
        l_pushcclosure(L, c_hthk_orig_load, 0); l_setfield(L, L_GLOBALSINDEX, "__hthk_orig_load");
        l_pushcclosure(L, c_hthk_log, 0);       l_setfield(L, L_GLOBALSINDEX, "__hthk_log");
    }
}

// ── 5. Lua wrappers ────────────────────────────────────────────────────────
// DAMAGE: chỉ nhân khi người đánh = team 1, người nhận ≠ team 1
#define LUA_WRAP_DAMAGE \
"local NAME='@battle/skill/base/object/attackResultData.lua'\n" \
"local chunk=assert(__hthk_orig_load(NAME),'hthk:nil '..NAME)\n" \
"local ARD=chunk(...)\n" \
"if type(ARD)~='table' or type(ARD.GetHurtVal)~='function' then return ARD end\n" \
"local _g=ARD.GetHurtVal\n" \
"local function team_of(p)\n" \
"  if type(p)~='table' or type(p.GetTeamId)~='function' then return nil end\n" \
"  local ok,t=pcall(function() return p:GetTeamId() end)\n" \
"  if not ok then return nil end return t\n" \
"end\n" \
"local _said=0\n" \
"function ARD:GetHurtVal()\n" \
"  local v=_g(self)\n" \
"  local m=math.floor(__hthk_num(0))\n" \
"  if m<=1 or type(v)~='number' or v<=0 then return v end\n" \
"  local atk,def\n" \
"  pcall(function() atk=team_of(self:GetMaster()) end)\n" \
"  pcall(function() def=team_of(self:GetTarget()) end)\n" \
"  if atk~=1 or def==nil or def==1 then return v end\n" \
"  if _said<3 then _said=_said+1\n" \
"    __hthk_log('dmg '..tostring(v)..'->'..tostring(v*m)..' (atk='..tostring(atk)..',def='..tostring(def)..')')\n" \
"  end\n" \
"  return v*m\n" \
"end\n" \
"return ARD\n"

// ADS: patch KLLSDK.AD.ShowRewardAd để fire callbacks thay vì chiếu video
// Handshake begin→over vẫn đủ, ad_uid đúng, server không từ chối
#define LUA_WRAP_ADS \
"local NAME='@game/module/advertisement/Advertisement.lua'\n" \
"local chunk=assert(__hthk_orig_load(NAME),'hthk:nil '..NAME)\n" \
"local M=chunk(...)\n" \
"if type(M)~='table' then return M end\n" \
"local function patch_ad_sdk()\n" \
"  local AD=KLLSDK and KLLSDK.AD\n" \
"  if type(AD)~='table' then __hthk_log('ads:KLLSDK.AD=nil') return end\n" \
"  if AD._hthk then return end\n" \
"  AD._hthk=true\n" \
"  local _init=AD.IsInitRewardAd\n" \
"  AD.IsInitRewardAd=function(...) if __hthk_num(2)>=1 then return true end return _init and _init(...) end\n" \
"  local _avail=AD.IsRewardAdAvailable\n" \
"  AD.IsRewardAdAvailable=function(...) if __hthk_num(2)>=1 then return true end return _avail and _avail(...) end\n" \
"  local _param=AD.SetRewardAdParam\n" \
"  AD.SetRewardAdParam=function(...) if __hthk_num(2)>=1 then pcall(_param,...) return end return _param and _param(...) end\n" \
"  local _show=AD.ShowRewardAd\n" \
"  AD.ShowRewardAd=function(self,...)\n" \
"    if __hthk_num(2)>=1 then\n" \
"      if type(AD.OnRewardAdOpened)=='function' then pcall(function() AD:OnRewardAdOpened() end) end\n" \
"      if type(AD.OnAdRewardSuccessCallBack)=='function' then pcall(function() AD:OnAdRewardSuccessCallBack() end) end\n" \
"      if type(AD.OnRewardAdClosed)=='function' then pcall(function() AD:OnRewardAdClosed() end) end\n" \
"      __hthk_log('ads:ShowRewardAd->fire callbacks')\n" \
"      return true\n" \
"    end\n" \
"    return _show(self,...)\n" \
"  end\n" \
"  __hthk_log('ads:KLLSDK.AD patched OK')\n" \
"end\n" \
"local _sc_begin=M.sc_advideo_begin\n" \
"if type(_sc_begin)=='function' then\n" \
"  M.sc_advideo_begin=function(self,...)\n" \
"    if __hthk_num(2)>=1 then patch_ad_sdk() return _sc_begin(self,...) end\n" \
"    return _sc_begin(self,...)\n" \
"  end\n" \
"end\n" \
"return M\n"

// ── 6. Generic ELF image helper (dùng cho cả il2cpp và tolua) ─────────────
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
            uintptr_t s = base + p.p_vaddr, e = s + p.p_memsz;
            if (a >= s && a + sz <= e) return true;
        }
        return false;
    }
    bool ru32(uintptr_t a, uint32_t* v) const {
        if (!contains(a, 4)) return false;
        memcpy(v, (const void*)a, 4); return true;
    }
    uintptr_t da(ElfW(Addr) val, size_t sz) const {
        if (!val) return 0;
        if (contains((uintptr_t)val, sz)) return (uintptr_t)val;
        uintptr_t a = base + (uintptr_t)val;
        return contains(a, sz) ? a : 0;
    }
};

static ElfImg g_il2cpp_img;   // libil2cpp.so
static ElfImg g_tolua_img;    // libtolua.so

// HAMOD compat: ImageContains dùng g_il2cpp_img (cho DoHook validate)
static bool ImageContains(uintptr_t a, size_t sz, unsigned fl = PF_R) {
    return g_il2cpp_img.contains(a, sz, fl);
}
static uintptr_t ImageArrayAddress(uintptr_t a, size_t i, size_t stride) {
    if (!a || !stride || i > (UINTPTR_MAX - a) / stride) return 0;
    return a + i * stride;
}
static bool ImageReadU32(uintptr_t a, uint32_t* v) { return g_il2cpp_img.ru32(a, v); }

// ── 7. ELF symbol resolver ─────────────────────────────────────────────────
class ElfExports {
    const ElfImg& img_;
    uintptr_t sym_=0, str_=0, sysv_=0, gnu_=0;
    size_t strsz_=0;
    void* symAt(uint32_t idx, const char* name) const {
        uintptr_t a = sym_ + idx * sizeof(ElfW(Sym));
        if (!img_.contains(a, sizeof(ElfW(Sym)))) return nullptr;
        ElfW(Sym) s; memcpy(&s, (const void*)a, sizeof(s));
        if (s.st_shndx == SHN_UNDEF || s.st_name >= strsz_) return nullptr;
        unsigned t = s.st_info & 0xf, b = s.st_info >> 4;
        if ((t != STT_FUNC && t != STT_NOTYPE) || (b != STB_GLOBAL && b != STB_WEAK)) return nullptr;
        size_t nl = strlen(name) + 1;
        if (nl > strsz_ - s.st_name) return nullptr;
        if (memcmp((const void*)(str_ + s.st_name), name, nl)) return nullptr;
        uintptr_t fn = (s.st_shndx == SHN_ABS) ? s.st_value : img_.base + s.st_value;
        return img_.contains(fn, 1, PF_X) ? (void*)fn : nullptr;
    }
public:
    explicit ElfExports(const ElfImg& img) : img_(img) {}
    bool init() {
        uintptr_t dyn=0; size_t cnt=0;
        for (size_t i=0; i<img_.phnum; ++i) {
            const ElfW(Phdr)& p = img_.phdr[i];
            if (p.p_type == PT_DYNAMIC) { dyn=img_.base+p.p_vaddr; cnt=p.p_memsz/sizeof(ElfW(Dyn)); break; }
        }
        if (!cnt || cnt>65536 || !img_.contains(dyn, cnt*sizeof(ElfW(Dyn)))) return false;
        const ElfW(Dyn)* d = (const ElfW(Dyn)*)dyn;
        ElfW(Addr) sym=0, str=0, sysv=0, gnu=0; size_t stride=sizeof(ElfW(Sym));
        for (size_t i=0; i<cnt; ++i) {
            if (d[i].d_tag == DT_NULL) break;
            switch(d[i].d_tag) {
                case DT_SYMTAB:   sym=d[i].d_un.d_ptr; break;
                case DT_STRTAB:   str=d[i].d_un.d_ptr; break;
                case DT_STRSZ:    strsz_=d[i].d_un.d_val; break;
                case DT_SYMENT:   stride=d[i].d_un.d_val; break;
                case DT_HASH:     sysv=d[i].d_un.d_ptr; break;
                case DT_GNU_HASH: gnu=d[i].d_un.d_ptr; break;
            }
        }
        if (stride!=sizeof(ElfW(Sym)) || !strsz_) return false;
        sym_=img_.da(sym,sizeof(ElfW(Sym))); str_=img_.da(str,strsz_);
        sysv_=img_.da(sysv,8); gnu_=img_.da(gnu,16);
        return sym_ && str_ && (gnu_ || sysv_);
    }
    void* find(const char* name) const {
        if (!name || !sym_ || !str_) return nullptr;
        if (gnu_) {
            uint32_t bc=0,fs=0,bl=0;
            img_.ru32(gnu_,&bc); img_.ru32(gnu_+4,&fs); img_.ru32(gnu_+8,&bl);
            uintptr_t buckets=gnu_+16+bl*sizeof(ElfW(Addr)), chains=buckets+bc*4;
            if (bc && bl) {
                uint32_t h=5381,idx=0;
                for (const unsigned char* p=(const unsigned char*)name; *p; ++p) h=h*33+*p;
                if (img_.ru32(buckets+(h%bc)*4,&idx) && idx>=fs) {
                    for (size_t step=0; step<1048576; ++step,++idx) {
                        uint32_t c=0; if (!img_.ru32(chains+(idx-fs)*4,&c)) break;
                        if ((c|1u)==(h|1u)) { void* fn=symAt(idx,name); if(fn) return fn; }
                        if (c&1u) break;
                    }
                }
            }
        }
        if (sysv_) {
            uint32_t bc=0,sc=0;
            img_.ru32(sysv_,&bc); img_.ru32(sysv_+4,&sc);
            uintptr_t buckets=sysv_+8, chains=buckets+bc*4;
            if (bc && sc) {
                uint32_t h=0,idx=0;
                for (const unsigned char* p=(const unsigned char*)name; *p; ++p) {
                    h=(h<<4)+*p; uint32_t hi=h&0xf0000000u; if(hi){h^=hi>>24;} h&=~hi;
                }
                if (img_.ru32(buckets+(h%bc)*4,&idx)) {
                    for (size_t step=0; idx && idx<sc && step<1048576; ++step) {
                        void* fn=symAt(idx,name); if(fn) return fn;
                        if (!img_.ru32(chains+idx*4,&idx)) break;
                    }
                }
            }
        }
        return nullptr;
    }
};

static bool FindLib(ElfImg& img, const char* libname) {
    struct Ctx { ElfImg* img; const char* name; bool found; };
    Ctx ctx{&img, libname, false};
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* data) -> int {
        Ctx* c = (Ctx*)data;
        if (!info->dlpi_name || !info->dlpi_phdr || !info->dlpi_phnum) return 0;
        const char* leaf = strrchr(info->dlpi_name, '/');
        leaf = leaf ? leaf+1 : info->dlpi_name;
        if (strcmp(leaf, c->name) != 0) return 0;
        c->img->base=info->dlpi_addr; c->img->phdr=info->dlpi_phdr;
        c->img->phnum=info->dlpi_phnum; c->img->path=info->dlpi_name;
        c->found=true; return 1;
    }, &ctx);
    return ctx.found;
}

static bool isIl2cppLoaded() { return FindLib(g_il2cpp_img, "libil2cpp.so"); }

// ── 8. IL2CPP API pointers ─────────────────────────────────────────────────
static void*       (*il2cpp_domain_get)()                                     = nullptr;
static void*       (*il2cpp_thread_attach)(void*)                             = nullptr;
static void        (*il2cpp_thread_detach)(void*)                             = nullptr;
static void**      (*il2cpp_domain_get_assemblies)(void*, size_t*)            = nullptr;
static void*       (*il2cpp_assembly_get_image)(void*)                        = nullptr;
static void*       (*il2cpp_class_from_name)(void*, const char*, const char*) = nullptr;
static void*       (*il2cpp_class_get_method_from_name)(void*, const char*, int) = nullptr;
static void*       (*il2cpp_object_get_class)(void*)                          = nullptr;
static void*       (*il2cpp_runtime_invoke)(void*, void*, void**, void**)     = nullptr;
static void*       (*il2cpp_class_get_methods)(void*, void**)                 = nullptr;
static const char* (*il2cpp_method_get_name)(void*)                           = nullptr;
static uint32_t    (*il2cpp_method_get_param_count)(void*)                    = nullptr;
static void*       (*il2cpp_method_get_param)(void*, uint32_t)                = nullptr;
static int         (*il2cpp_type_get_type)(void*)                             = nullptr;
static bool        (*il2cpp_type_is_byref)(void*)                             = nullptr;
static void        (*il2cpp_format_exception)(const void*, char*, int)        = nullptr;
static void*       (*il2cpp_class_get_field_from_name)(void*, const char*)    = nullptr;
static int32_t     (*il2cpp_field_get_offset)(void*)                          = nullptr;
static size_t      (*il2cpp_image_get_class_count)(void*)                     = nullptr;
static void*       (*il2cpp_image_get_class)(void*, size_t)                   = nullptr;
static const char* (*il2cpp_class_get_name_raw)(void*)                        = nullptr;
static void* g_il2cpp_handle = nullptr;

static void* GetIl2cppHandle() {
    if (g_il2cpp_handle) return g_il2cpp_handle;
    const int fl = RTLD_NOW | RTLD_NOLOAD;
    if (g_il2cpp_img.path && *g_il2cpp_img.path)
        g_il2cpp_handle = dlopen(g_il2cpp_img.path, fl);
    if (!g_il2cpp_handle)
        g_il2cpp_handle = dlopen(OBFUSCATE("libil2cpp.so"), fl);
    if (!g_il2cpp_handle) LOGI("GetHandle unavailable; will use mapped ELF exports");
    return g_il2cpp_handle;
}

static bool InitIl2cpp() {
    if (!g_il2cpp_img.has()) return false;
    LOGI("[%s] Resolve BEGIN: base=%p", BUILD_ID, (void*)g_il2cpp_img.base);
    void* handle = GetIl2cppHandle();
    ElfExports exports(g_il2cpp_img);
    const bool has_exports = exports.init();
    unsigned from_handle=0, from_image=0, missing=0;
    auto resolve = [&](const char* name, bool required) -> void* {
        void* fn = handle ? dlsym(handle, name) : nullptr;
        if (fn && ImageContains((uintptr_t)fn, 1, PF_X)) { ++from_handle; return fn; }
        fn = has_exports ? exports.find(name) : nullptr;
        if (fn) { ++from_image; return fn; }
        if (required) { ++missing; LOGE("Required API missing: %s", name); }
        return nullptr;
    };
#define LOAD_API(fn, req) fn = reinterpret_cast<decltype(fn)>(resolve(#fn, req))
    LOAD_API(il2cpp_domain_get,              true);
    LOAD_API(il2cpp_thread_attach,           true);
    LOAD_API(il2cpp_domain_get_assemblies,   true);
    LOAD_API(il2cpp_assembly_get_image,      true);
    LOAD_API(il2cpp_class_from_name,         true);
    LOAD_API(il2cpp_class_get_method_from_name, true);
    LOAD_API(il2cpp_object_get_class,        true);
    LOAD_API(il2cpp_runtime_invoke,          true);
    LOAD_API(il2cpp_thread_detach,           false);
    LOAD_API(il2cpp_class_get_methods,       false);
    LOAD_API(il2cpp_method_get_name,         false);
    LOAD_API(il2cpp_method_get_param_count,  false);
    LOAD_API(il2cpp_method_get_param,        false);
    LOAD_API(il2cpp_type_get_type,           false);
    LOAD_API(il2cpp_type_is_byref,           false);
    LOAD_API(il2cpp_format_exception,        false);
    LOAD_API(il2cpp_class_get_field_from_name, false);
    LOAD_API(il2cpp_field_get_offset,        false);
    LOAD_API(il2cpp_image_get_class_count,   false);
    LOAD_API(il2cpp_image_get_class,         false);
    il2cpp_class_get_name_raw = reinterpret_cast<decltype(il2cpp_class_get_name_raw)>(
        resolve("il2cpp_class_get_name", false));
#undef LOAD_API
    LOGI("[%s] Resolve END: handle=%u mappedELF=%u missing=%u", BUILD_ID, from_handle, from_image, missing);
    return missing == 0;
}

static uintptr_t Tim_Ham(const char* ns, const char* cls, const char* method, int argc) {
    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies || !il2cpp_assembly_get_image ||
        !il2cpp_class_from_name || !il2cpp_class_get_method_from_name) return 0;
    void* domain = il2cpp_domain_get(); if (!domain) return 0;
    size_t sz=0;
    void** asms = il2cpp_domain_get_assemblies(domain, &sz);
    for (size_t i=0; i<sz; ++i) {
        void* img = il2cpp_assembly_get_image(asms[i]); if (!img) continue;
        void* k = il2cpp_class_from_name(img, ns, cls);
        if (k) { void* m = il2cpp_class_get_method_from_name(k, method, argc); if (m) return (uintptr_t)*(void**)m; }
    }
    return 0;
}

static bool DoHook(void* fn, void* hook, void** orig, const char* tag) {
    if (!hook || !orig || !ImageContains((uintptr_t)fn, 1, PF_X)) {
        LOGE("Hook SKIP (invalid IL2CPP code address): %s @ %p", tag, fn);
        return false;
    }
    int r = DobbyHook(fn, hook, orig);
    bool ok = r == 0 && *orig != nullptr;
    LOGI("Hook %s: %s @ %p", ok ? "OK" : "FAIL", tag, fn);
    return ok;
}

// ── 9. Speed hooks (UnityEngine.Time.set/get_timeScale) ───────────────────
typedef void  (*t_set_ts)(float, void*);
typedef float (*t_get_ts)(void*);
static t_set_ts o_set_ts = nullptr;
static t_get_ts o_get_ts = nullptr;

static void h_set_ts(float v, void* mi) {
    g_ts_game.store(v, std::memory_order_relaxed);
    int s = (int)g_spd.load(std::memory_order_relaxed);
    float mul = (s >= 0 && s < 4) ? SPD_MUL[s] : 1.0f;
    if (o_set_ts) o_set_ts(v * mul, mi);
}
static float h_get_ts(void* mi) {
    return g_ts_game.load(std::memory_order_relaxed);
}

// ── 10. luaL_loadbufferx hook (libtolua.so) ───────────────────────────────
typedef int (*t_lua_load)(void* L, const char* buf, size_t sz,
                          const char* chunkname, const char* mode);
static t_lua_load o_lua_load = nullptr;

static int h_lua_load(void* L, const char* buf, size_t sz,
                      const char* chunkname, const char* mode) {
    if (chunkname) {
        const char* wrapper = nullptr;
        if (strcmp(chunkname, "@battle/skill/base/object/attackResultData.lua") == 0)
            wrapper = LUA_WRAP_DAMAGE;
        else if (strcmp(chunkname, "@game/module/advertisement/Advertisement.lua") == 0
                 || strcmp(chunkname, "@game/module/advertisement/advertisement.lua") == 0)
            wrapper = LUA_WRAP_ADS;
        if (wrapper) {
            LOGI("intercept '%s'", chunkname);
            {
                std::lock_guard<std::mutex> lock(g_buf_mtx);
                std::string key(chunkname);
                if (g_orig_bufs.find(key) == g_orig_bufs.end())
                    g_orig_bufs[key] = std::vector<char>(buf, buf+sz);
            }
            lua_reg_helpers(L);
            o_lua_load_fwd = (t_lua_load2)o_lua_load;
            return o_lua_load(L, wrapper, strlen(wrapper), chunkname, "t");
        }
    }
    return o_lua_load(L, buf, sz, chunkname, mode);
}

// ── 11. Worker thread ──────────────────────────────────────────────────────
void* h_t(void*) {
    // Phase 1: libtolua.so — Lua hooks (damage + ads)
    for (int i = 0; i < 120; ++i) {
        if (FindLib(g_tolua_img, "libtolua.so") && g_tolua_img.has()) break;
        sleep(1);
    }
    if (!g_tolua_img.has()) { LOGE("libtolua.so NOT FOUND"); }
    else {
        ElfExports tex(g_tolua_img);
        bool hte = tex.init();
        void* tlh = g_tolua_img.path ? dlopen(g_tolua_img.path, RTLD_NOW|RTLD_NOLOAD) : nullptr;
        auto tr = [&](const char* n) -> void* {
            void* f = tlh ? dlsym(tlh, n) : nullptr;
            if (f && g_tolua_img.contains((uintptr_t)f, 1, PF_X)) return f;
            return hte ? tex.find(n) : nullptr;
        };
        void* fn_load = tr("luaL_loadbufferx");
        if (!fn_load) LOGE("luaL_loadbufferx NOT FOUND");
        else if (DobbyHook(fn_load, (void*)h_lua_load, (void**)&o_lua_load) == 0 && o_lua_load) {
            o_lua_load_fwd = (t_lua_load2)o_lua_load;
            LOGI("Hook luaL_loadbufferx OK");
        } else LOGE("Hook luaL_loadbufferx FAIL");
        l_pushcclosure = (decltype(l_pushcclosure))tr("lua_pushcclosure");
        l_setglobal    = (decltype(l_setglobal))   tr("lua_setglobal");
        l_setfield     = (decltype(l_setfield))    tr("lua_setfield");
        l_pushnumber   = (decltype(l_pushnumber))  tr("lua_pushnumber");
        l_tonumber     = (decltype(l_tonumber))    tr("lua_tonumber");
        l_tolstring    = (decltype(l_tolstring))   tr("lua_tolstring");
        l_settop       = (decltype(l_settop))      tr("lua_settop");
        l_gettop       = (decltype(l_gettop))      tr("lua_gettop");
        LOGI("Lua API: cc=%p sg=%p pn=%p tn=%p ts=%p gt=%p",
             l_pushcclosure, l_setglobal, l_pushnumber, l_tonumber, l_tolstring, l_gettop);
    }

    // Phase 2: libil2cpp.so — speed hook
    for (int i = 0; i < 120; ++i) {
        if (isIl2cppLoaded()) break;
        sleep(1);
    }
    if (!InitIl2cpp()) { LOGE("[%s] InitIl2cpp FAILED", BUILD_ID); return nullptr; }

    size_t last=0; int stable=0;
    void* domain = il2cpp_domain_get();
    void* thread = (domain && il2cpp_thread_attach) ? il2cpp_thread_attach(domain) : nullptr;
    for (int i=0; i<60 && stable<5; ++i) {
        sleep(1);
        size_t cnt=0;
        void** asm_ = il2cpp_domain_get_assemblies ? il2cpp_domain_get_assemblies(domain, &cnt) : nullptr;
        if (asm_ && cnt==last && cnt>0) ++stable; else { last=cnt; stable=0; }
    }
    LOGI("Assemblies stable: %zu", last);

    unsigned installed = 0;
    uintptr_t addr_set_ts = Tim_Ham(OBFUSCATE("UnityEngine"), OBFUSCATE("Time"), OBFUSCATE("set_timeScale"), 1);
    installed += DoHook((void*)addr_set_ts, (void*)h_set_ts, (void**)&o_set_ts, "Time.set_timeScale");
    uintptr_t addr_get_ts = Tim_Ham(OBFUSCATE("UnityEngine"), OBFUSCATE("Time"), OBFUSCATE("get_timeScale"), 0);
    installed += DoHook((void*)addr_get_ts, (void*)h_get_ts, (void**)&o_get_ts, "Time.get_timeScale");

    LOGI("[%s] HOOK DONE: %u installed", BUILD_ID, installed);
    if (il2cpp_thread_detach) il2cpp_thread_detach(thread);
    return nullptr;
}

// ── 12. ParseMenuConfig (3 features từ 7-field HAMOD config) ──────────────
static bool ParseMenuConfig(const char* text) {
    if (!text) return false;
    float f[7] = {}; unsigned cnt = 0;
    const char* p = text;
    while (true) {
        while (isspace((unsigned char)*p)) ++p;
        if (!*p) break;
        if (cnt == 7) return false;
        char* e = nullptr; errno = 0;
        float v = strtof(p, &e);
        if (e == p || errno == ERANGE || !std::isfinite(v) || (*e && !isspace((unsigned char)*e))) return false;
        f[cnt++] = v; p = e;
    }
    if (cnt < 1) return false;
    // f0: damage (seekbar 1-99999)
    { float d = f[0]; if (d < 1.0f) d=1.0f; if (d > 99999.0f) d=99999.0f; g_dmg.store(d, std::memory_order_release); }
    // f1: speed index (seekbar 0-3)
    if (cnt >= 2) {
        float s = f[1]; if (s < 0.0f) s=0.0f; if (s > 3.0f) s=3.0f;
        g_spd.store(s, std::memory_order_release);
        // Active apply: game chỉ gọi set_timeScale 1 lần lúc startup
        static float s_last_applied = -1.0f;
        if (o_set_ts && s != s_last_applied) {
            s_last_applied = s;
            int si = (int)s;
            float cur = g_ts_game.load(std::memory_order_relaxed);
            o_set_ts(cur * SPD_MUL[si], nullptr);
            LOGI("[Speed] Applied x%.0f (timeScale=%.2f)", SPD_MUL[si], cur * SPD_MUL[si]);
        }
    }
    // f2: ads toggle (0/1)
    if (cnt >= 3) { g_ads.store(f[2] >= 0.5f ? 1.0f : 0.0f, std::memory_order_release); }
    g_config_loaded_once.store(true, std::memory_order_release);
    return true;
}

// ── 13. Doc_Config_Ngam — poll HAMOD config file ──────────────────────────
void* Doc_Config_Ngam(void*) {
    char root[768];
    const char* cfgroot = getenv("MODMENU_LOG_DIR");
    const char* src = cfgroot && *cfgroot ? cfgroot : "/data/data/menu.nextgen.launcher/files";
    snprintf(root, sizeof(root), "%s", src);
    size_t rlen = strlen(root);
    while (rlen > 1 && root[rlen-1] == '/') root[--rlen] = '\0';
    const char* leaf = strrchr(root, '/'); leaf = leaf ? leaf+1 : root;
    bool root_is_mods = strcmp(leaf, "mods") == 0;
    const char* names[] = {
        "config_com.hhkb.rpg.gp.txt",
        "config_COM.HHKB.RPG.GP.txt"
    };
    char paths[4][1024] = {}; size_t npath = 0;
    for (unsigned loc=0; loc<2; ++loc) {
        bool append_mods = root_is_mods ? loc==1 : loc==0;
        for (const char* name : names) {
            int n = snprintf(paths[npath], sizeof(paths[npath]), "%s/%s%s", root, append_mods?"mods/":"", name);
            if (n > 0 && (size_t)n < sizeof(paths[npath])) ++npath;
        }
    }
    LOGI("[%s] CONFIG begin: pid=%d", BUILD_ID, getpid());
    for (size_t i=0; i<npath; ++i) LOGI("CONFIG candidate[%zu]=%s", i, paths[i]);

    int last_status=-1, last_path=-1;
    float last_dmg=-1.0f, last_spd=-1.0f, last_ads=-1.0f;
    while (true) {
        FILE* file=nullptr; int selected=-1;
        for (size_t i=0; i<npath; ++i) {
            file=fopen(paths[i], "r"); if (file) { selected=(int)i; break; }
        }
        if (!file) {
            if (last_status != 0) LOGE("CONFIG missing; previous values retained");
            last_status=0; usleep(500000); continue;
        }
        char text[256] = {};
        size_t len = fread(text, 1, sizeof(text)-1, file);
        bool read_ok = !ferror(file) && len < sizeof(text)-1;
        fclose(file);
        if (!read_ok || memchr(text, '\0', len) || !ParseMenuConfig(text)) {
            if (last_status != 1 || last_path != selected)
                LOGE("CONFIG invalid: path=%s text='%.40s'", paths[selected], text);
            last_status=1; last_path=selected; usleep(500000); continue;
        }
        float dmg=g_dmg.load(std::memory_order_relaxed);
        float spd=g_spd.load(std::memory_order_relaxed);
        float ads=g_ads.load(std::memory_order_relaxed);
        if (last_status != 2 || last_path != selected || dmg!=last_dmg || spd!=last_spd || ads!=last_ads)
            LOGI("CONFIG applied: path=%s dmg=%.0f spd=%.0f ads=%.0f", paths[selected], dmg, spd, ads);
        last_status=2; last_path=selected; last_dmg=dmg; last_spd=spd; last_ads=ads;
        usleep(500000);
    }
    return nullptr;
}

// ── 14. Entry point ────────────────────────────────────────────────────────
__attribute__((constructor))
void l_m() {
    LOGI("[%s] MOD injected!", BUILD_ID);
    pthread_t t1, t2;
    int rc = pthread_create(&t1, nullptr, h_t, nullptr);
    if (rc == 0) pthread_detach(t1); else LOGE("Worker create failed: %d", rc);
    rc = pthread_create(&t2, nullptr, Doc_Config_Ngam, nullptr);
    if (rc == 0) pthread_detach(t2); else LOGE("Config thread create failed: %d", rc);
}
