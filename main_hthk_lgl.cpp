// LGL May Root — Hải Tặc Truyền Kỳ (com.hhkb.rpg.gp)
#define LOG_TAG      "HTHK_MOD"
#define GAME_NAME    "Hai Tac Truyen Ky"
#define GAME_PACKAGE "com.hhkb.rpg.gp"
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
#include <map>
#include <string>
#include <vector>
#include <mutex>
#include <android/log.h>
#include "Includes/obfuscate.h"
#include <Dobby/dobby.h>

// ── State ──────────────────────────────────────────────────────────────────
static std::atomic<int>   g_dmg{1};
static std::atomic<int>   g_spd{0};
static std::atomic<bool>  g_ads{false};
static std::atomic<float> g_ts_game{1.0f};
static const float SPD_MUL[4] = {1.0f, 3.0f, 5.0f, 10.0f};

// ── Lua API (từ libtolua.so) ───────────────────────────────────────────────
typedef int (*lua_CFunction)(void*);
static void        (*l_pushcclosure)(void*, lua_CFunction, int) = nullptr;
static void        (*l_setglobal)(void*, const char*)           = nullptr;
static void        (*l_setfield)(void*, int, const char*)       = nullptr;
static void        (*l_pushnumber)(void*, double)               = nullptr;
static double      (*l_tonumber)(void*, int)                    = nullptr;
static const char* (*l_tolstring)(void*, int, size_t*)          = nullptr;
static void        (*l_settop)(void*, int)                      = nullptr;
static int         (*l_gettop)(void*)                           = nullptr;
#define L_GLOBALSINDEX (-10002)  // Lua 5.1; dùng lua_setglobal nếu 5.3

static std::map<std::string, std::vector<char>> g_orig_bufs;
static std::mutex g_buf_mtx;

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
        case 2: val = g_ads.load(std::memory_order_relaxed) ? 1.0 : 0.0; break;
    }
    if (l_pushnumber) l_pushnumber(L, val);
    return 1;
}

// forward declare — o_lua_load defined later
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
        l_pushcclosure(L, c_hthk_num, 0);
        l_setglobal(L, "__hthk_num");
        l_pushcclosure(L, c_hthk_orig_load, 0);
        l_setglobal(L, "__hthk_orig_load");
        l_pushcclosure(L, c_hthk_log, 0);
        l_setglobal(L, "__hthk_log");
    } else if (l_setfield) {
        l_pushcclosure(L, c_hthk_num, 0);
        l_setfield(L, L_GLOBALSINDEX, "__hthk_num");
        l_pushcclosure(L, c_hthk_orig_load, 0);
        l_setfield(L, L_GLOBALSINDEX, "__hthk_orig_load");
        l_pushcclosure(L, c_hthk_log, 0);
        l_setfield(L, L_GLOBALSINDEX, "__hthk_log");
    }
}

// ── ElfImg ─────────────────────────────────────────────────────────────────
struct ElfImg {
    uintptr_t          base  = 0;
    const ElfW(Phdr)*  phdr  = nullptr;
    size_t             phnum = 0;
    const char*        path  = nullptr;
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
        if (!contains(a, 4)) return false;
        memcpy(v, (const void*)a, 4); return true;
    }
    uintptr_t da(ElfW(Addr) val, size_t sz) const {
        if (!val) return 0;
        if (contains((uintptr_t)val, sz)) return (uintptr_t)val;
        uintptr_t a = base + val;
        return contains(a, sz) ? a : 0;
    }
};

// ── ElfExports ─────────────────────────────────────────────────────────────
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
        uintptr_t dyn = 0; size_t cnt = 0;
        for (size_t i = 0; i < img_.phnum; ++i) {
            const ElfW(Phdr)& p = img_.phdr[i];
            if (p.p_type == PT_DYNAMIC) { dyn = img_.base + p.p_vaddr; cnt = p.p_memsz / sizeof(ElfW(Dyn)); break; }
        }
        if (!cnt || cnt > 65536 || !img_.contains(dyn, cnt * sizeof(ElfW(Dyn)))) return false;
        const ElfW(Dyn)* d = (const ElfW(Dyn)*)dyn;
        ElfW(Addr) sym=0, str=0, sysv=0, gnu=0; size_t stride = sizeof(ElfW(Sym));
        for (size_t i = 0; i < cnt; ++i) {
            if (d[i].d_tag == DT_NULL) break;
            switch (d[i].d_tag) {
                case DT_SYMTAB: sym=d[i].d_un.d_ptr; break;
                case DT_STRTAB: str=d[i].d_un.d_ptr; break;
                case DT_STRSZ:  strsz_=d[i].d_un.d_val; break;
                case DT_SYMENT: stride=d[i].d_un.d_val; break;
                case DT_HASH:   sysv=d[i].d_un.d_ptr; break;
                case DT_GNU_HASH: gnu=d[i].d_un.d_ptr; break;
            }
        }
        if (stride != sizeof(ElfW(Sym)) || !strsz_) return false;
        sym_ = img_.da(sym, sizeof(ElfW(Sym)));
        str_ = img_.da(str, strsz_);
        sysv_ = img_.da(sysv, 8);
        gnu_  = img_.da(gnu,  16);
        return sym_ && str_ && (gnu_ || sysv_);
    }
    void* find(const char* name) const {
        if (!name || !sym_ || !str_) return nullptr;
        if (gnu_) {
            uint32_t bc=0, fs=0, bl=0;
            img_.ru32(gnu_, &bc); img_.ru32(gnu_+4, &fs); img_.ru32(gnu_+8, &bl);
            uintptr_t buckets = gnu_ + 16 + bl * sizeof(ElfW(Addr));
            uintptr_t chains  = buckets + bc * 4;
            if (bc && bl) {
                uint32_t h=5381, idx=0;
                for (const unsigned char* p=(const unsigned char*)name; *p; ++p) h=h*33+*p;
                if (img_.ru32(buckets + (h % bc) * 4, &idx) && idx >= fs) {
                    for (size_t step=0; step<1048576; ++step, ++idx) {
                        uint32_t c=0;
                        if (!img_.ru32(chains + (idx-fs)*4, &c)) break;
                        if ((c|1u)==(h|1u)) { void* fn=symAt(idx,name); if(fn) return fn; }
                        if (c&1u) break;
                    }
                }
            }
        }
        if (sysv_) {
            uint32_t bc=0, sc=0;
            img_.ru32(sysv_, &bc); img_.ru32(sysv_+4, &sc);
            uintptr_t buckets = sysv_+8, chains = buckets + bc*4;
            if (bc && sc) {
                uint32_t h=0, idx=0;
                for (const unsigned char* p=(const unsigned char*)name; *p; ++p) {
                    h=(h<<4)+*p; uint32_t hi=h&0xf0000000u; if(hi){h^=hi>>24;} h&=~hi;
                }
                if (img_.ru32(buckets + (h%bc)*4, &idx)) {
                    for (size_t step=0; idx && idx<sc && step<1048576; ++step) {
                        void* fn=symAt(idx,name); if(fn) return fn;
                        if (!img_.ru32(chains+idx*4, &idx)) break;
                    }
                }
            }
        }
        return nullptr;
    }
};

// ── FindLib ────────────────────────────────────────────────────────────────
static bool FindLib(ElfImg& img, const char* libname) {
    struct Ctx { ElfImg* img; const char* name; bool found; };
    Ctx ctx{&img, libname, false};
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* data) -> int {
        Ctx* c = (Ctx*)data;
        const char* path = info->dlpi_name;
        if (!path || !info->dlpi_phdr || !info->dlpi_phnum) return 0;
        const char* leaf = strrchr(path, '/'); leaf = leaf ? leaf+1 : path;
        if (strcmp(leaf, c->name) != 0) return 0;
        c->img->base  = info->dlpi_addr;
        c->img->phdr  = info->dlpi_phdr;
        c->img->phnum = info->dlpi_phnum;
        c->img->path  = path;
        c->found = true;
        return 1;
    }, &ctx);
    return ctx.found;
}

// ── IL2CPP API ─────────────────────────────────────────────────────────────
static void*   (*i_domain_get)()                                    = nullptr;
static void*   (*i_thread_attach)(void*)                            = nullptr;
static void**  (*i_domain_assemblies)(void*, size_t*)               = nullptr;
static void*   (*i_asm_image)(void*)                                = nullptr;
static void*   (*i_class_from_name)(void*, const char*, const char*)= nullptr;
static void*   (*i_class_get_method)(void*, const char*, int)       = nullptr;

static bool InitIL2CppAPI(const ElfImg& img) {
    ElfExports ex(img); if (!ex.init()) { LOGE("il2cpp ElfExports init FAIL"); return false; }
    void* h = img.path ? dlopen(img.path, RTLD_NOW|RTLD_NOLOAD) : nullptr;
    auto res = [&](const char* n) -> void* {
        void* fn = h ? dlsym(h, n) : nullptr;
        if (fn && img.contains((uintptr_t)fn, 1, PF_X)) return fn;
        return ex.find(n);
    };
    i_domain_get       = (decltype(i_domain_get))      res("il2cpp_domain_get");
    i_thread_attach    = (decltype(i_thread_attach))   res("il2cpp_thread_attach");
    i_domain_assemblies= (decltype(i_domain_assemblies))res("il2cpp_domain_get_assemblies");
    i_asm_image        = (decltype(i_asm_image))       res("il2cpp_assembly_get_image");
    i_class_from_name  = (decltype(i_class_from_name)) res("il2cpp_class_from_name");
    i_class_get_method = (decltype(i_class_get_method))res("il2cpp_class_get_method_from_name");
    bool ok = i_domain_get && i_thread_attach && i_domain_assemblies &&
              i_asm_image && i_class_from_name && i_class_get_method;
    LOGI("IL2CPP API: %s", ok ? "OK" : "FAIL");
    return ok;
}

static void* FindIL2CppClass(const char* ns, const char* cls) {
    void* domain = i_domain_get(); if (!domain) return nullptr;
    size_t n = 0;
    void** asms = i_domain_assemblies(domain, &n);
    for (size_t i = 0; i < n; ++i) {
        void* img = i_asm_image(asms[i]); if (!img) continue;
        void* k = i_class_from_name(img, ns, cls);
        if (k) return k;
    }
    return nullptr;
}

// ── Speed hooks (Time.set/get_timeScale) ───────────────────────────────────
typedef void  (*t_set_ts)(float, void*);
typedef float (*t_get_ts)(void*);
static t_set_ts o_set_ts = nullptr;
static t_get_ts o_get_ts = nullptr;

static void h_set_ts(float v, void* mi) {
    T_COUNT(set_ts);
    g_ts_game.store(v, std::memory_order_relaxed);
    int s = g_spd.load(std::memory_order_relaxed);
    float mul = (s >= 0 && s < 4) ? SPD_MUL[s] : 1.0f;
    if (o_set_ts) o_set_ts(v * mul, mi);
}
static float h_get_ts(void* mi) {
    T_COUNT(get_ts);
    return g_ts_game.load(std::memory_order_relaxed);
}

// ── libtolua hook ──────────────────────────────────────────────────────────
typedef int (*t_lua_load)(void* L, const char* buf, size_t sz,
                          const char* chunkname, const char* mode);
static t_lua_load o_lua_load = nullptr;
// sync forward pointer after hook
static inline void sync_fwd() { o_lua_load_fwd = (t_lua_load2)o_lua_load; }

// ── Lua wrappers ───────────────────────────────────────────────────────────
#define LUA_WRAP_DAMAGE \
"local NAME='@battle/skill/base/object/attackResultData.lua'\n" \
"local chunk=assert(__hthk_orig_load(NAME),'hthk:nil '..NAME)\n" \
"local ARD=chunk(...)\n" \
"if type(ARD)~='table' or type(ARD.GetHurtVal)~='function' then return ARD end\n" \
"local _g=ARD.GetHurtVal\n" \
"local function team_of(p)\n" \
"  if type(p)~='table' or type(p.GetTeamId)~='function' then return nil end\n" \
"  local ok,t=pcall(function() return p:GetTeamId() end)\n" \
"  if not ok then return nil end\n" \
"  return t\n" \
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

#define LUA_WRAP_ADS \
"local NAME='@game/sdk/adSystem.lua'\n" \
"local chunk=assert(__hthk_orig_load(NAME),'hthk:nil '..NAME)\n" \
"local M=chunk(...)\n" \
"if type(M)~='table' then return M end\n" \
"local _show=M.ShowRewardAd\n" \
"if type(_show)=='function' then\n" \
"  function M:ShowRewardAd(id,cb,...)\n" \
"    if __hthk_num(2)>=1 then\n" \
"      if type(cb)=='function' then cb(true) end\n" \
"      return\n" \
"    end\n" \
"    return _show(self,id,cb,...)\n" \
"  end\n" \
"end\n" \
"return M\n"

// ── luaL_loadbufferx hook ──────────────────────────────────────────────────
static int h_lua_load(void* L, const char* buf, size_t sz,
                      const char* chunkname, const char* mode) {
    T_COUNT(lua_load);
    if (chunkname) {
        const char* wrapper = nullptr;
        if (strcmp(chunkname, "@battle/skill/base/object/attackResultData.lua") == 0)
            wrapper = LUA_WRAP_DAMAGE;
        else if (strcmp(chunkname, "@game/sdk/adSystem.lua") == 0)
            wrapper = LUA_WRAP_ADS;
        if (wrapper) {
            LOGI("intercept '%s'", chunkname);
            {
                std::lock_guard<std::mutex> lock(g_buf_mtx);
                std::string key(chunkname);
                if (g_orig_bufs.find(key) == g_orig_bufs.end())
                    g_orig_bufs[key] = std::vector<char>(buf, buf + sz);
            }
            lua_reg_helpers(L);
            return o_lua_load(L, wrapper, strlen(wrapper), chunkname, "t");
        }
    }
    return o_lua_load(L, buf, sz, chunkname, mode);
}

// ── ResolveAll ─────────────────────────────────────────────────────────────
static void ResolveAll() {
    T_STEP("ResolveAll");
    // Phase 1: libtolua
    ElfImg tolua;
    if (!FindLib(tolua, "libtolua.so")) { LOGE("libtolua.so NOT FOUND"); }
    else {
        ElfExports ex(tolua); ex.init();
        void* fn = ex.find("luaL_loadbufferx");
        if (!fn) { LOGE("luaL_loadbufferx NOT FOUND"); }
        else {
            if (DobbyHook(fn, (void*)h_lua_load, (void**)&o_lua_load) == 0 && o_lua_load) {
                sync_fwd();
                LOGI("Hook luaL_loadbufferx OK");
            } else {
                LOGE("Hook luaL_loadbufferx FAIL");
            }
        }
        // Lua API
        void* lh = tolua.path ? dlopen(tolua.path, RTLD_NOW|RTLD_NOLOAD) : nullptr;
        auto lr = [&](const char* n) -> void* {
            void* f = lh ? dlsym(lh, n) : nullptr;
            if (f && tolua.contains((uintptr_t)f, 1, PF_X)) return f;
            return ex.find(n);
        };
        l_pushcclosure = (decltype(l_pushcclosure))lr("lua_pushcclosure");
        l_setglobal    = (decltype(l_setglobal))   lr("lua_setglobal");
        l_setfield     = (decltype(l_setfield))    lr("lua_setfield");
        l_pushnumber   = (decltype(l_pushnumber))  lr("lua_pushnumber");
        l_tonumber     = (decltype(l_tonumber))    lr("lua_tonumber");
        l_tolstring    = (decltype(l_tolstring))   lr("lua_tolstring");
        l_settop       = (decltype(l_settop))      lr("lua_settop");
        l_gettop       = (decltype(l_gettop))      lr("lua_gettop");
        LOGI("Lua API: cc=%p sg=%p sf=%p pn=%p tn=%p ts=%p st=%p gt=%p",
             l_pushcclosure, l_setglobal, l_setfield, l_pushnumber, l_tonumber, l_tolstring,
             l_settop, l_gettop);
    }
    TraceReport();
    isGameReady = true;
    TraceStartCallReports();
    LOGI("MOD READY");
}

// ── Phase 2: IL2CPP speed hooks ────────────────────────────────────────────
static void SetupSpeedHooks() {
    T_STEP("SetupSpeedHooks");
    ElfImg il2cpp;
    for (int retry = 0; retry < 60; ++retry) {
        if (FindLib(il2cpp, "libil2cpp.so") && il2cpp.has()) break;
        TraceSleep(1);
    }
    if (!il2cpp.has()) { LOGE("libil2cpp.so NOT FOUND"); return; }
    LOGI("libil2cpp base=%p", (void*)il2cpp.base);

    if (!InitIL2CppAPI(il2cpp)) return;

    // Wait for il2cpp domain
    void* domain = nullptr;
    void* thread = nullptr;
    for (int retry = 0; retry < 60; ++retry) {
        domain = i_domain_get();
        if (domain) break;
        TraceSleep(1);
    }
    if (!domain) { LOGE("il2cpp domain NOT READY"); return; }

    thread = i_thread_attach(domain);
    TraceSleep(2); // let assemblies stabilize

    // Find UnityEngine.Time
    void* klass = FindIL2CppClass("UnityEngine", "Time");
    if (!klass) { LOGE("UnityEngine.Time NOT FOUND"); return; }
    LOGI("Time class: %p", klass);

    // set_timeScale(1 param)
    void* m_set = i_class_get_method(klass, "set_timeScale", 1);
    if (m_set) {
        void* fn = *(void**)m_set;
        if (DobbyHook(fn, (void*)h_set_ts, (void**)&o_set_ts) == 0 && o_set_ts)
            LOGI("Hook set_timeScale OK");
        else LOGE("Hook set_timeScale FAIL");
    } else LOGE("set_timeScale(1) NOT FOUND");

    // get_timeScale(0 params)
    void* m_get = i_class_get_method(klass, "get_timeScale", 0);
    if (m_get) {
        void* fn = *(void**)m_get;
        if (DobbyHook(fn, (void*)h_get_ts, (void**)&o_get_ts) == 0 && o_get_ts)
            LOGI("Hook get_timeScale OK");
        else LOGE("Hook get_timeScale FAIL");
    } else LOGE("get_timeScale(0) NOT FOUND");
}

// ── mod_thread ─────────────────────────────────────────────────────────────
static void* mod_thread(void*) {
    T_STEP("wait libtolua");
    ElfImg tolua;
    for (int retry = 0; retry < 120; ++retry) {
        if (FindLib(tolua, "libtolua.so") && tolua.has()) break;
        TraceSleep(1);
    }
    if (!tolua.has()) { LOGE("libtolua.so NEVER LOADED"); return nullptr; }
    TraceSleep(1);
    ResolveAll();
    SetupSpeedHooks();
    return nullptr;
}

__attribute__((constructor))
static void on_load() {
    T_STEP("on_load");
    pthread_t t; pthread_create(&t, nullptr, mod_thread, nullptr);
}

// ── JNI stubs required by JNI_Boilerplate.h ───────────────────────────────
static jstring IconWebViewData(JNIEnv* env, jclass)  { return env->NewStringUTF(""); }
static jstring GetMenuTitle(JNIEnv* env, jclass)     { return env->NewStringUTF(GAME_NAME); }
static jstring GetMenuSubtitle(JNIEnv* env, jclass)  { return env->NewStringUTF("LGL Mod"); }
static jstring GetTeleLink(JNIEnv* env, jclass)      { return env->NewStringUTF(""); }
static jstring GetTeleText(JNIEnv* env, jclass)      { return env->NewStringUTF(""); }
static jstring GetWebLink(JNIEnv* env, jclass)       { return env->NewStringUTF(""); }
static jstring GetWebText(JNIEnv* env, jclass)       { return env->NewStringUTF(""); }
static jstring GetZaloLink(JNIEnv* env, jclass)      { return env->NewStringUTF(""); }
static jstring GetZaloText(JNIEnv* env, jclass)      { return env->NewStringUTF(""); }
static jboolean VerifyKeyNative(JNIEnv*, jclass, jstring) { return JNI_TRUE; }
static void Init(JNIEnv*, jclass, jobject, jobject, jobject) {}
static jobjectArray SettingsList(JNIEnv* env, jclass) {
    return env->NewObjectArray(0, env->FindClass("java/lang/String"), nullptr);
}

static jobjectArray GetFeatureList(JNIEnv* env, jclass) {
    const char* items[] = {
        "0_SeekBar_Sat thuong_1_99999",
        "1_SeekBar_Toc do_0_3",
        "2_Toggle_Bo quang cao"
    };
    int n = 3;
    jclass sc = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray(n, sc, nullptr);
    for (int i = 0; i < n; ++i)
        env->SetObjectArrayElement(arr, i, env->NewStringUTF(items[i]));
    return arr;
}

static void Changes(JNIEnv*, jclass, jobject,
    jint featNum, jstring, jint value, jboolean enabled, jstring) {
    switch (featNum) {
        case 0: {
            int lv = (int)value;
            if (lv < 1) lv = 1; if (lv > 99999) lv = 99999;
            g_dmg.store(lv, std::memory_order_relaxed);
            LOGI("Damage x%d", lv);
            break;
        }
        case 1: {
            int lv = (int)value;
            if (lv < 0) lv = 0; if (lv > 3) lv = 3;
            g_spd.store(lv, std::memory_order_relaxed);
            float cur = g_ts_game.load(std::memory_order_relaxed);
            if (o_set_ts) o_set_ts(cur * SPD_MUL[lv], nullptr);
            LOGI("Speed x%d (mul=%.0f)", lv, SPD_MUL[lv]);
            break;
        }
        case 2:
            g_ads.store(enabled == JNI_TRUE, std::memory_order_relaxed);
            LOGI("Ads skip: %d", (int)(enabled == JNI_TRUE));
            break;
    }
}

static void CheckOverlayPermission(JNIEnv*, jclass, jobject) {}

extern "C" JNIEXPORT jstring JNICALL Java_com_android_support_Menu_getMenuTitle(JNIEnv* env, jclass) {
    return env->NewStringUTF("Hai Tac Truyen Ky");
}

#include "Includes/JNI_Boilerplate.h"
