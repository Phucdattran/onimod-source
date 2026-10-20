// LGL May Root — Hải Tặc Truyền Kỳ (com.hhkb.rpg.gp)
// Hook luaL_loadbufferx trong libtolua.so, inject wrapper Lua cho:
//   - battle.battlemanager        (tốc độ)
//   - battle.skill.base.object.attackresultdata (sát thương)
//   - game.sdk.adsystem           (bỏ quảng cáo)
#define LOG_TAG      "HTHK_MOD"
#define GAME_NAME    "⚔️ Hải Tặc Truyền Kỳ"
#define GAME_PACKAGE "com.hhkb.rpg.gp"
#include "Includes/LGLTrace.h"

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

// ═══════════════════════════════════════════════════════════
//  Trạng thái menu (đọc/ghi từ Changes() + hook)
//    g_dmg: 0=×1  1=×2  2=×5  3=×20
//    g_spd: 0=×1  1=×3  2=×5  3=×10
//    g_ads: true = bỏ xem video
// ═══════════════════════════════════════════════════════════
static std::atomic<int>  g_dmg{0};
static std::atomic<int>  g_spd{0};
static std::atomic<bool> g_ads{false};

// ═══════════════════════════════════════════════════════════
//  libtolua.so ELF image tracking
// ═══════════════════════════════════════════════════════════
struct ToluaImage {
    uintptr_t         base  = 0;
    const ElfW(Phdr)* phdr  = nullptr;
    size_t            phnum = 0;
    const char*       path  = nullptr;
};
static ToluaImage g_tolua;

static bool TC(uintptr_t addr, size_t sz, unsigned flags = PF_R) {
    if (!addr || !sz || !g_tolua.phdr) return false;
    for (size_t i = 0; i < g_tolua.phnum; ++i) {
        const ElfW(Phdr)& p = g_tolua.phdr[i];
        if (p.p_type != PT_LOAD || (p.p_flags & flags) != flags) continue;
        uintptr_t s = g_tolua.base + p.p_vaddr;
        if (addr >= s && addr - s <= p.p_memsz - sz) return true;
    }
    return false;
}
static bool TRu32(uintptr_t a, uint32_t* v) {
    if (!TC(a, 4)) return false;
    memcpy(v, (const void*)a, 4); return true;
}
static uintptr_t TDA(ElfW(Addr) val, size_t sz) {
    if (!val) return 0;
    if (TC(val, sz)) return val;
    uintptr_t a = g_tolua.base + val;
    return TC(a, sz) ? a : 0;
}
static uintptr_t TAA(uintptr_t b, size_t i, size_t st) {
    return (b && st) ? b + i*st : 0;
}

// ═══════════════════════════════════════════════════════════
//  ELF symbol resolver (GNU_HASH + SYSV_HASH)
// ═══════════════════════════════════════════════════════════
class ToluaExports {
    uintptr_t sym_=0, str_=0, sysv_=0, gnu_=0;
    size_t    ssz_=0;

    void* SA(uint32_t idx, const char* nm) const {
        uintptr_t a = TAA(sym_, idx, sizeof(ElfW(Sym)));
        if (!TC(a, sizeof(ElfW(Sym)))) return nullptr;
        ElfW(Sym) s; memcpy(&s, (const void*)a, sizeof(s));
        if (s.st_shndx == SHN_UNDEF || s.st_name >= ssz_) return nullptr;
        unsigned ty = s.st_info & 0xf, bi = s.st_info >> 4;
        if ((ty != STT_FUNC && ty != STT_NOTYPE) ||
            (bi != STB_GLOBAL && bi != STB_WEAK)) return nullptr;
        size_t nl = strlen(nm)+1;
        if (nl > ssz_ - s.st_name) return nullptr;
        if (memcmp((const void*)(str_ + s.st_name), nm, nl)) return nullptr;
        uintptr_t fn = (s.st_shndx == SHN_ABS) ? s.st_value
                                                 : (g_tolua.base + s.st_value);
        return TC(fn, 1, PF_X) ? (void*)fn : nullptr;
    }
public:
    bool Init() {
        uintptr_t dyn=0; size_t cnt=0;
        for (size_t i = 0; i < g_tolua.phnum; ++i) {
            const ElfW(Phdr)& p = g_tolua.phdr[i];
            if (p.p_type != PT_DYNAMIC) continue;
            dyn = g_tolua.base + p.p_vaddr;
            cnt = p.p_memsz / sizeof(ElfW(Dyn)); break;
        }
        if (!cnt || !TC(dyn, cnt*sizeof(ElfW(Dyn)))) return false;
        const ElfW(Dyn)* d = (const ElfW(Dyn)*)dyn;
        ElfW(Addr) sym=0, str=0, sv=0, gn=0;
        size_t stride=sizeof(ElfW(Sym)); bool term=false;
        for (size_t i = 0; i < cnt; ++i) {
            if (d[i].d_tag == DT_NULL) { term=true; break; }
            switch(d[i].d_tag) {
                case DT_SYMTAB:   sym=d[i].d_un.d_ptr; break;
                case DT_STRTAB:   str=d[i].d_un.d_ptr; break;
                case DT_STRSZ:    ssz_=d[i].d_un.d_val; break;
                case DT_SYMENT:   stride=d[i].d_un.d_val; break;
                case DT_HASH:     sv=d[i].d_un.d_ptr; break;
                case DT_GNU_HASH: gn=d[i].d_un.d_ptr; break;
            }
        }
        if (!term || stride!=sizeof(ElfW(Sym)) || !ssz_) return false;
        sym_=TDA(sym,sizeof(ElfW(Sym))); str_=TDA(str,ssz_);
        sysv_=TDA(sv,8); gnu_=TDA(gn,16);
        return sym_ && str_ && (gnu_ || sysv_);
    }

    void* Find(const char* nm) const {
        if (!nm || !sym_ || !str_) return nullptr;
        if (gnu_) {
            uint32_t bc=0, fs=0, bl=0;
            TRu32(gnu_,&bc); TRu32(gnu_+4,&fs); TRu32(gnu_+8,&bl);
            uintptr_t bkts=TAA(gnu_+16,bl,sizeof(ElfW(Addr)));
            uintptr_t chns=TAA(bkts,bc,4);
            if (bc && bl && bkts && chns) {
                uint32_t h=5381, bv=0;
                for (const unsigned char* p=(const unsigned char*)nm; *p; ++p) h=h*33+*p;
                if (TRu32(TAA(bkts,h%bc,4),&bv) && bv>=fs) {
                    for (size_t s=0; s<1048576 && bv!=UINT32_MAX; ++s,++bv) {
                        uint32_t cv=0;
                        if (!TRu32(TAA(chns,bv-fs,4),&cv)) break;
                        if ((cv|1u)==(h|1u)) { void* f=SA(bv,nm); if (f) return f; }
                        if (cv&1u) break;
                    }
                }
            }
        }
        if (sysv_) {
            uint32_t bc=0, sc=0, bv=0;
            TRu32(sysv_,&bc); TRu32(sysv_+4,&sc);
            uintptr_t bkts=sysv_+8, chns=TAA(bkts,bc,4);
            if (bc && sc && chns) {
                uint32_t h=0;
                for (const unsigned char* p=(const unsigned char*)nm; *p; ++p) {
                    h=(h<<4)+*p; uint32_t hi=h&0xf0000000u;
                    if (hi) h^=hi>>24; h&=~hi;
                }
                if (TRu32(TAA(bkts,h%bc,4),&bv)) {
                    for (size_t s=0; bv && bv<sc && s<1048576; ++s) {
                        void* f=SA(bv,nm); if (f) return f;
                        if (!TRu32(TAA(chns,bv,4),&bv)) break;
                    }
                }
            }
        }
        return nullptr;
    }
};

static bool FindTolua() {
    bool found = false;
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* data) -> int {
        if (!info->dlpi_name || !info->dlpi_phdr || !info->dlpi_phnum) return 0;
        const char* leaf = strrchr(info->dlpi_name, '/');
        leaf = leaf ? leaf+1 : info->dlpi_name;
        if (strcmp(leaf, "libtolua.so") != 0) return 0;
        g_tolua = {info->dlpi_addr, info->dlpi_phdr, (size_t)info->dlpi_phnum,
                   info->dlpi_name};
        *static_cast<bool*>(data) = true;
        return 1;
    }, &found);
    return found;
}

// ═══════════════════════════════════════════════════════════
//  Lua API (opaque state, function pointers)
// ═══════════════════════════════════════════════════════════
typedef struct lua_State lua_State;
typedef int (*lua_CFunction)(lua_State*);

static void        (*f_settop)      (lua_State*, int)                = nullptr;
static void        (*f_pushnumber)  (lua_State*, double)             = nullptr;
static void        (*f_pushcclosure)(lua_State*, lua_CFunction, int) = nullptr;
static void        (*f_setglobal)   (lua_State*, const char*)        = nullptr;
static double      (*f_tonumberx)   (lua_State*, int, int*)          = nullptr;
static const char* (*f_tolstring)   (lua_State*, int, size_t*)       = nullptr;

static int (*o_loadbufferx)(lua_State*, const char*, size_t,
                             const char*, const char*) = nullptr;

// ═══════════════════════════════════════════════════════════
//  Original bytecode storage (per module name)
// ═══════════════════════════════════════════════════════════
static std::mutex                               g_buf_mu;
static std::map<std::string, std::vector<char>> g_orig_buf;

// ═══════════════════════════════════════════════════════════
//  Lua C functions
// ═══════════════════════════════════════════════════════════

// __hthk_orig_load(name) → chunk function on stack
static int c_orig_load(lua_State* L) {
    if (!o_loadbufferx) return 0;
    const char* name = f_tolstring ? f_tolstring(L, 1, nullptr) : nullptr;
    if (!name) return 0;
    std::vector<char> buf;
    {
        std::lock_guard<std::mutex> lk(g_buf_mu);
        auto it = g_orig_buf.find(name);
        if (it == g_orig_buf.end()) {
            LOGE("[HTHK] c_orig_load: no buffer '%s'", name);
            return 0;
        }
        buf = it->second;
    }
    int rc = o_loadbufferx(L, buf.data(), buf.size(), name, "b");
    if (rc != 0) {
        LOGE("[HTHK] c_orig_load: load FAIL rc=%d '%s'", rc, name);
        if (f_settop) f_settop(L, 0);
        return 0;
    }
    return 1;
}

// __hthk_num(index) → number  (0=dmg, 1=spd, 2=ads)
static int c_hthk_num(lua_State* L) {
    int idx = 0;
    if (f_tonumberx) {
        int isnum = 0;
        double v = f_tonumberx(L, 1, &isnum);
        if (isnum) idx = (int)v;
    }
    double val = 0.0;
    switch (idx) {
        case 0: val = (double)g_dmg.load(std::memory_order_relaxed); break;
        case 1: val = (double)g_spd.load(std::memory_order_relaxed); break;
        case 2: val = g_ads.load(std::memory_order_relaxed) ? 1.0 : 0.0; break;
    }
    if (f_pushnumber) f_pushnumber(L, val);
    return 1;
}

// ═══════════════════════════════════════════════════════════
//  Lua wrappers
// ═══════════════════════════════════════════════════════════
static const char WRAP_BATTLEMANAGER[] = R"LUA(
local NAME = "battle.battlemanager"
local chunk = assert(__hthk_orig_load(NAME), "hthk: orig nil " .. NAME)
local BattleManager = chunk(...)
if type(BattleManager) ~= "table" or type(BattleManager.GetBattleSpeed) ~= "function" then
  return BattleManager
end
local LEVELS = {1, 3, 5, 10}
local _get = BattleManager.GetBattleSpeed
function BattleManager:GetBattleSpeed()
  local base = _get(self)
  local lv = __hthk_num(1)
  local m = LEVELS[math.floor(lv) + 1] or 1
  if m <= 1 then return base end
  if type(base) ~= "number" or base <= 0 then return base end
  return base * m
end
return BattleManager
)LUA";

static const char WRAP_ATTACKRESULT[] = R"LUA(
local NAME = "battle.skill.base.object.attackresultdata"
local chunk = assert(__hthk_orig_load(NAME), "hthk: orig nil " .. NAME)
local AttackResultData = chunk(...)
if type(AttackResultData) ~= "table" or type(AttackResultData.GetHurtVal) ~= "function" then
  return AttackResultData
end
local DMG = {1, 2, 5, 20}
local function team_of(p)
  if type(p) ~= "table" or type(p.GetTeamId) ~= "function" then return nil end
  local ok, t = pcall(function() return p:GetTeamId() end)
  return ok and t or nil
end
local _get = AttackResultData.GetHurtVal
function AttackResultData:GetHurtVal()
  local v = _get(self)
  local lv = __hthk_num(0)
  local m = DMG[math.floor(lv) + 1] or 1
  if m <= 1 or type(v) ~= "number" or v <= 0 then return v end
  local atk, def
  pcall(function() atk = team_of(self:GetMaster()) end)
  pcall(function() def = team_of(self:GetTarget()) end)
  if atk ~= 1 or def == nil or def == 1 then return v end
  return v * m
end
return AttackResultData
)LUA";

static const char WRAP_ADSYSTEM[] = R"LUA(
local NAME = "game.sdk.adsystem"
local chunk = assert(__hthk_orig_load(NAME), "hthk: orig nil " .. NAME)
local AD = chunk(...)
if type(AD) ~= "table" or type(AD.ShowRewardAd) ~= "function" then return AD end
local function on() return __hthk_num(2) >= 1 end
local _show  = AD.ShowRewardAd
local _init  = AD.IsInitRewardAd
local _avail = AD.IsRewardAdAvailable
local _param = AD.SetRewardAdParam
AD.IsInitRewardAd = function(...)
  if on() then return true end
  if _init then return _init(...) end
end
AD.IsRewardAdAvailable = function(...)
  if on() then return true end
  if _avail then return _avail(...) end
end
AD.SetRewardAdParam = function(...)
  if not on() then if _param then return _param(...) end; return end
  pcall(_param, ...)
end
local function fire(fn, evt)
  if type(fn) == "function" then
    local ok = pcall(fn)
    if ok then return true end
  end
  if x_eventManager and _EventType and evt and _EventType[evt] then
    local id = _EventType[evt]
    pcall(function() x_eventManager:postEvent(id) end)
  end
end
AD.ShowRewardAd = function(...)
  if not on() then return _show(...) end
  fire(AD.OnRewardAdOpened,          "Event_RewardAd_Open")
  fire(AD.OnAdRewardSuccessCallBack, "Event_RewardAd_ShowEnd")
  fire(AD.OnRewardAdClosed,          "Event_RewardAd_Close")
  return true
end
return AD
)LUA";

struct Target { const char* name; const char* wrapper; };
static const Target TARGETS[] = {
    {"battle.battlemanager",                      WRAP_BATTLEMANAGER},
    {"battle.skill.base.object.attackresultdata", WRAP_ATTACKRESULT},
    {"game.sdk.adsystem",                         WRAP_ADSYSTEM},
};
static constexpr int N_TARGETS = (int)(sizeof(TARGETS)/sizeof(TARGETS[0]));

// ═══════════════════════════════════════════════════════════
//  luaL_loadbufferx hook
// ═══════════════════════════════════════════════════════════
static std::atomic<bool> g_globals_set{false};

static int h_loadbufferx(lua_State* L, const char* buff, size_t sz,
                          const char* name, const char* mode) {
    T_COUNT(loadbufferx);
    if (!name) return o_loadbufferx(L, buff, sz, name, mode);

    int ti = -1;
    for (int i = 0; i < N_TARGETS; ++i)
        if (strcmp(name, TARGETS[i].name) == 0) { ti = i; break; }
    if (ti < 0) return o_loadbufferx(L, buff, sz, name, mode);

    LOGI("[HTHK] intercept '%s' sz=%zu", name, sz);

    {
        std::lock_guard<std::mutex> lk(g_buf_mu);
        g_orig_buf[name] = std::vector<char>(buff, buff + sz);
    }

    bool expected = false;
    if (g_globals_set.compare_exchange_strong(expected, true)) {
        if (f_pushcclosure && f_setglobal) {
            f_pushcclosure(L, c_orig_load, 0);
            f_setglobal(L, "__hthk_orig_load");
            f_pushcclosure(L, c_hthk_num, 0);
            f_setglobal(L, "__hthk_num");
            LOGI("[HTHK] Lua globals registered");
        }
    }

    const char* src = TARGETS[ti].wrapper;
    int rc = o_loadbufferx(L, src, strlen(src), name, "t");
    if (rc != 0) {
        LOGE("[HTHK] wrapper FAIL '%s' rc=%d - fallback", name, rc);
        if (f_settop) f_settop(L, 0);
        return o_loadbufferx(L, buff, sz, name, mode);
    }
    LOGI("[HTHK] wrapper OK '%s'", name);
    return 0;
}

// ═══════════════════════════════════════════════════════════
//  LGL menu
// ═══════════════════════════════════════════════════════════
extern "C" JNIEXPORT jstring JNICALL
Java_com_android_support_Main_GetFeatureList(JNIEnv* env, jclass) {
    return env->NewStringUTF(
        "Category_" GAME_NAME "\n"
        "0_SeekBar_Sát thương ×2/5/20_0_3\n"
        "1_SeekBar_Tốc độ ×3/5/10_0_3\n"
        "2_Toggle_Bỏ quảng cáo"
    );
}

extern "C" JNIEXPORT void JNICALL
Java_com_android_support_Main_Changes(JNIEnv*, jclass, jobject,
        jint featNum, jstring, jfloat value, jboolean boolean, jstring) {
    switch (featNum) {
        case 0: {
            int lv = (int)roundf(value);
            if (lv < 0) lv = 0; else if (lv > 3) lv = 3;
            g_dmg.store(lv, std::memory_order_relaxed);
            const int mults[] = {1, 2, 5, 20};
            LOGI("[HTHK] Sát thương ×%d", mults[lv]);
            break;
        }
        case 1: {
            int lv = (int)roundf(value);
            if (lv < 0) lv = 0; else if (lv > 3) lv = 3;
            g_spd.store(lv, std::memory_order_relaxed);
            const int mults[] = {1, 3, 5, 10};
            LOGI("[HTHK] Tốc độ ×%d", mults[lv]);
            break;
        }
        case 2: {
            bool v = boolean == JNI_TRUE;
            g_ads.store(v, std::memory_order_relaxed);
            LOGI("[HTHK] Bỏ quảng cáo: %s", v ? "ON" : "OFF");
            break;
        }
    }
}

// ═══════════════════════════════════════════════════════════
//  Mục 9 — Khởi tạo (chờ libtolua.so rồi hook)
// ═══════════════════════════════════════════════════════════
static void* mod_thread(void*) {
    T_STEP("start mod_thread");
    LOGI("[HTHK] mod_thread started");

    // Chờ libtolua.so load (tối đa 30 s)
    T_STEP("wait libtolua");
    for (int i = 0; i < 300; ++i) {
        if (FindTolua()) break;
        usleep(100000);
    }
    if (!g_tolua.base) {
        LOGE("[HTHK] libtolua.so NOT FOUND — abort");
        return nullptr;
    }
    LOGI("[HTHK] libtolua base=%p path=%s",
         (void*)g_tolua.base, g_tolua.path ? g_tolua.path : "?");

    // Resolve Lua API
    T_STEP("resolve Lua API");
    void* handle = (g_tolua.path && *g_tolua.path)
                   ? dlopen(g_tolua.path, RTLD_NOW | RTLD_NOLOAD) : nullptr;
    ToluaExports exp;
    bool has_exp = exp.Init();
    LOGI("[HTHK] ELF resolver init=%d", has_exp ? 1 : 0);

    auto res = [&](const char* sym) -> void* {
        void* fn = handle ? dlsym(handle, sym) : nullptr;
        if (fn && TC((uintptr_t)fn, 1, PF_X)) return fn;
        return has_exp ? exp.Find(sym) : nullptr;
    };

    f_settop       = (decltype(f_settop))       res("lua_settop");
    f_pushnumber   = (decltype(f_pushnumber))   res("lua_pushnumber");
    f_pushcclosure = (decltype(f_pushcclosure)) res("lua_pushcclosure");
    f_setglobal    = (decltype(f_setglobal))    res("lua_setglobal");
    f_tonumberx    = (decltype(f_tonumberx))    res("lua_tonumberx");
    f_tolstring    = (decltype(f_tolstring))    res("lua_tolstring");
    void* lbx = res("luaL_loadbufferx");

    LOGI("[HTHK] settop=%p push=%p closure=%p setg=%p tonum=%p tostr=%p lbx=%p",
         (void*)f_settop, (void*)f_pushnumber, (void*)f_pushcclosure,
         (void*)f_setglobal, (void*)f_tonumberx, (void*)f_tolstring, lbx);

    if (!lbx || !f_pushcclosure || !f_setglobal || !f_tonumberx || !f_tolstring) {
        LOGE("[HTHK] Lua API missing — abort");
        return nullptr;
    }

    // Hook luaL_loadbufferx
    T_STEP("DobbyHook luaL_loadbufferx");
    int rc = DobbyHook(lbx, (void*)h_loadbufferx, (void**)&o_loadbufferx);
    if (rc != 0 || !o_loadbufferx) {
        LOGE("[HTHK] DobbyHook FAIL rc=%d", rc);
        return nullptr;
    }
    LOGI("[HTHK] Hook luaL_loadbufferx OK");

    isGameReady = true;
    T_STEP("MOD READY");
    LOGI("[HTHK] MOD READY");
    TraceStartCallReports();

    return nullptr;
}

__attribute__((constructor))
static void on_load() {
    pthread_t t;
    pthread_create(&t, nullptr, mod_thread, nullptr);
    pthread_detach(t);
}

extern "C" JNIEXPORT void JNICALL
Java_com_android_support_Main_CheckOverlayPermission(JNIEnv*, jclass, jobject) {}

#include "Includes/JNI_Boilerplate.h"
