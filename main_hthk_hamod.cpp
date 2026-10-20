// ============================================================
//  HAMOD — Hải Tặc Truyền Kỳ (com.hhkb.rpg.gp)
//  Port ONIMOD 3 features via luaL_loadbufferx hook (libtolua.so)
//  Features:
//    f0 = DMG seekbar 0-3  → ×1 / ×2 / ×5 / ×20
//    f1 = SPD seekbar 0-3  → ×1 / ×3 / ×5 / ×10
//    f2 = Ads toggle 0/1   → skip reward ad video
// ============================================================

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
#define GAME_PKG "com.hhkb.rpg.gp"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ═══════════════════════════════════════════════════════════
//  HAMOD CONFIG  (updated every second from config file)
//    f0 = dmg level  (0-3)
//    f1 = spd level  (0-3)
//    f2 = ads skip   (0/1)
// ═══════════════════════════════════════════════════════════
static std::atomic<int>  g_dmg{0};
static std::atomic<int>  g_spd{0};
static std::atomic<bool> g_ads{false};

static void update_config() {
    // HAMOD writes config as 7 space-separated floats
    const char* paths[] = {
        "/data/data/" GAME_PKG "/shared_prefs/config_" GAME_PKG ".txt",
        "/data/data/" GAME_PKG "/files/config_" GAME_PKG ".txt",
    };
    for (const char* p : paths) {
        FILE* f = fopen(p, "r");
        if (!f) continue;
        float f0=0, f1=0, f2=0;
        int n = fscanf(f, "%f %f %f", &f0, &f1, &f2);
        fclose(f);
        if (n >= 1) {
            g_dmg.store((int)roundf(f0), std::memory_order_relaxed);
            g_spd.store((int)roundf(f1), std::memory_order_relaxed);
            g_ads.store(f2 >= 0.5f,      std::memory_order_relaxed);
        }
        return;
    }
}

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
//  Lua API (opaque state, all via function pointers)
// ═══════════════════════════════════════════════════════════
typedef struct lua_State lua_State;
typedef int (*lua_CFunction)(lua_State*);

static void        (*f_settop)      (lua_State*, int)                      = nullptr;
static void        (*f_pushnumber)  (lua_State*, double)                   = nullptr;
static void        (*f_pushcclosure)(lua_State*, lua_CFunction, int)       = nullptr;
static void        (*f_setglobal)   (lua_State*, const char*)              = nullptr;
static double      (*f_tonumberx)   (lua_State*, int, int*)                = nullptr;
static const char* (*f_tolstring)   (lua_State*, int, size_t*)             = nullptr;
static void        (*f_settop2)     (lua_State*, int)                      = nullptr; // alias

// Hook target (Dobby will fill trampoline)
static int (*o_loadbufferx)(lua_State*, const char*, size_t,
                             const char*, const char*)                      = nullptr;

// ═══════════════════════════════════════════════════════════
//  Original bytecode storage (per module name)
// ═══════════════════════════════════════════════════════════
static std::mutex                              g_buf_mu;
static std::map<std::string, std::vector<char>> g_orig_buf;

// ═══════════════════════════════════════════════════════════
//  Lua C functions registered as globals in game Lua state
// ═══════════════════════════════════════════════════════════

// __hthk_orig_load(name) → pushes original chunk onto stack (or nil on fail)
static int c_orig_load(lua_State* L) {
    if (!o_loadbufferx) return 0;
    const char* name = f_tolstring ? f_tolstring(L, 1, nullptr) : nullptr;
    if (!name) return 0;

    std::vector<char> buf;
    {
        std::lock_guard<std::mutex> lk(g_buf_mu);
        auto it = g_orig_buf.find(name);
        if (it == g_orig_buf.end()) {
            LOGE("[%s] c_orig_load: no buffer for '%s'", BUILD_ID, name);
            return 0;
        }
        buf = it->second;
    }

    int rc = o_loadbufferx(L, buf.data(), buf.size(), name, "b");
    if (rc != 0) {
        LOGE("[%s] c_orig_load: load FAIL rc=%d for '%s'", BUILD_ID, rc, name);
        // Error message is on stack; clear it and return nil
        if (f_settop) f_settop(L, 0);
        return 0;
    }
    return 1; // chunk function on top of stack
}

// __hthk_num(index) → returns HAMOD config value as number
//   0 = dmg level, 1 = spd level, 2 = ads enabled
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
//  Lua wrapper source for each target module
//  (injected in place of original bytecode)
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
  fire(AD.OnRewardAdOpened,           "Event_RewardAd_Open")
  fire(AD.OnAdRewardSuccessCallBack,  "Event_RewardAd_ShowEnd")
  fire(AD.OnRewardAdClosed,           "Event_RewardAd_Close")
  return true
end
return AD
)LUA";

// ═══════════════════════════════════════════════════════════
//  Target module table
// ═══════════════════════════════════════════════════════════
struct Target {
    const char* name;
    const char* wrapper;
};
static const Target TARGETS[] = {
    {"battle.battlemanager",                         WRAP_BATTLEMANAGER},
    {"battle.skill.base.object.attackresultdata",    WRAP_ATTACKRESULT},
    {"game.sdk.adsystem",                            WRAP_ADSYSTEM},
};
static constexpr int N_TARGETS = (int)(sizeof(TARGETS)/sizeof(TARGETS[0]));

// ═══════════════════════════════════════════════════════════
//  luaL_loadbufferx hook
// ═══════════════════════════════════════════════════════════
static std::atomic<bool> g_globals_set{false};

static int h_loadbufferx(lua_State* L, const char* buff, size_t sz,
                          const char* name, const char* mode) {
    if (!name) return o_loadbufferx(L, buff, sz, name, mode);

    int ti = -1;
    for (int i = 0; i < N_TARGETS; ++i) {
        if (strcmp(name, TARGETS[i].name) == 0) { ti = i; break; }
    }
    if (ti < 0) return o_loadbufferx(L, buff, sz, name, mode);

    LOGI("[%s] intercept '%s' sz=%zu", BUILD_ID, name, sz);

    // Save original bytecode
    {
        std::lock_guard<std::mutex> lk(g_buf_mu);
        g_orig_buf[name] = std::vector<char>(buff, buff + sz);
    }

    // Register globals once (Lua state is single-threaded so this is safe)
    bool expected = false;
    if (g_globals_set.compare_exchange_strong(expected, true)) {
        if (f_pushcclosure && f_setglobal) {
            f_pushcclosure(L, c_orig_load, 0);
            f_setglobal(L, "__hthk_orig_load");
            f_pushcclosure(L, c_hthk_num, 0);
            f_setglobal(L, "__hthk_num");
            LOGI("[%s] Lua globals registered", BUILD_ID);
        }
    }

    // Load our wrapper source instead of the original bytecode
    const char* src = TARGETS[ti].wrapper;
    int rc = o_loadbufferx(L, src, strlen(src), name, "t");
    if (rc != 0) {
        LOGE("[%s] wrapper load FAIL for '%s' rc=%d — falling back", BUILD_ID, name, rc);
        if (f_settop) f_settop(L, 0);
        return o_loadbufferx(L, buff, sz, name, mode);
    }
    LOGI("[%s] wrapper OK '%s'", BUILD_ID, name);
    return 0;
}

// ═══════════════════════════════════════════════════════════
//  Main mod thread
// ═══════════════════════════════════════════════════════════
static void* mod_thread(void*) {
    LOGI("[%s] started", BUILD_ID);

    // Wait for libtolua.so (up to 30 s)
    for (int i = 0; i < 300; ++i) {
        if (FindTolua()) break;
        usleep(100000);
    }
    if (!g_tolua.base) {
        LOGE("[%s] libtolua.so NOT FOUND — abort", BUILD_ID);
        return nullptr;
    }
    LOGI("[%s] libtolua base=%p path=%s", BUILD_ID,
         (void*)g_tolua.base, g_tolua.path ? g_tolua.path : "?");

    // Resolve Lua API
    void* handle = (g_tolua.path && *g_tolua.path)
                   ? dlopen(g_tolua.path, RTLD_NOW | RTLD_NOLOAD) : nullptr;
    ToluaExports exp;
    bool has_exp = exp.Init();
    LOGI("[%s] ELF resolver init=%d", BUILD_ID, has_exp ? 1 : 0);

    auto resolve = [&](const char* sym) -> void* {
        void* fn = handle ? dlsym(handle, sym) : nullptr;
        if (fn && TC((uintptr_t)fn, 1, PF_X)) return fn;
        return has_exp ? exp.Find(sym) : nullptr;
    };

    f_settop       = (decltype(f_settop))       resolve("lua_settop");
    f_pushnumber   = (decltype(f_pushnumber))   resolve("lua_pushnumber");
    f_pushcclosure = (decltype(f_pushcclosure)) resolve("lua_pushcclosure");
    f_setglobal    = (decltype(f_setglobal))    resolve("lua_setglobal");
    f_tonumberx    = (decltype(f_tonumberx))    resolve("lua_tonumberx");
    f_tolstring    = (decltype(f_tolstring))    resolve("lua_tolstring");

    void* lbx = resolve("luaL_loadbufferx");

    LOGI("[%s] lua_settop=%p pushnumber=%p pushcclosure=%p setglobal=%p"
         " tonumberx=%p tolstring=%p loadbufferx=%p",
         BUILD_ID,
         (void*)f_settop, (void*)f_pushnumber, (void*)f_pushcclosure,
         (void*)f_setglobal, (void*)f_tonumberx, (void*)f_tolstring, lbx);

    if (!lbx || !f_pushcclosure || !f_setglobal || !f_tonumberx || !f_tolstring) {
        LOGE("[%s] essential Lua API missing — abort", BUILD_ID);
        return nullptr;
    }

    int rc = DobbyHook(lbx, (void*)h_loadbufferx, (void**)&o_loadbufferx);
    if (rc != 0 || !o_loadbufferx) {
        LOGE("[%s] DobbyHook luaL_loadbufferx FAIL rc=%d", BUILD_ID, rc);
        return nullptr;
    }
    LOGI("[%s] Hook luaL_loadbufferx OK — MOD READY", BUILD_ID);

    // Config update loop
    while (true) {
        update_config();
        sleep(1);
    }
    return nullptr;
}

__attribute__((constructor))
static void on_load() {
    pthread_t t;
    pthread_create(&t, nullptr, mod_thread, nullptr);
    pthread_detach(t);
}
