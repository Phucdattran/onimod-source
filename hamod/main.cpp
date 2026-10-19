// com.bxzyvn.gg — TAROT: VĨNH HẰNG THẦN NGỮ
// Lua damage mod via libxlua.so hooks
// f0=dmg_toggle  f1=dmg_multiplier(1..100)  f2=god_mode

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
#include <android/log.h>
#include "Includes/obfuscate.h"
#include <Dobby/dobby.h>

#define BUILD_ID "TAROT_MOD"
#define LOG_TAG  "OniHamod"
#define PKG_NAME "com.bxzyvn.gg"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─── State (3 features, index theo menu.js) ──────────────────────────────────
std::atomic<bool>  g_dmg    {false};    // f0 — bật/tắt nhân sát thương
std::atomic<float> g_dmgmul {10.0f};   // f1 — hệ số (1..100)
std::atomic<bool>  g_god    {false};    // f2 — bất tử
static std::atomic<int>  g_patch_needed{0};
static std::atomic<bool> g_in_patch    {false};
static std::atomic<bool> g_config_loaded{false};

// ─── ELF image cho libxlua.so ────────────────────────────────────────────────
struct LibImage { uintptr_t base; const ElfW(Phdr)* phdr; size_t phnum; };
static LibImage g_xlua = {};

static bool LibContains(const LibImage& img, uintptr_t addr, size_t sz, unsigned flags = PF_R) {
    if (!addr || !sz || !img.phdr) return false;
    for (size_t i = 0; i < img.phnum; ++i) {
        const ElfW(Phdr)& p = img.phdr[i];
        if (p.p_type != PT_LOAD || (p.p_flags & flags) != flags) continue;
        if (p.p_vaddr > UINTPTR_MAX - img.base) continue;
        uintptr_t start = img.base + p.p_vaddr;
        if (p.p_memsz > UINTPTR_MAX - start || sz > p.p_memsz) continue;
        if (addr >= start && addr - start <= p.p_memsz - sz) return true;
    }
    return false;
}
static uintptr_t LibArrAddr(uintptr_t base, size_t idx, size_t stride) {
    if (!base || !stride || idx > (UINTPTR_MAX - base) / stride) return 0;
    return base + idx * stride;
}
static bool LibReadU32(const LibImage& img, uintptr_t addr, uint32_t* v) {
    if (!LibContains(img, addr, 4)) return false;
    memcpy(v, reinterpret_cast<const void*>(addr), 4);
    return true;
}
static uintptr_t LibDynAddr(const LibImage& img, ElfW(Addr) val, size_t sz) {
    if (!val) return 0;
    if (LibContains(img, (uintptr_t)val, sz)) return (uintptr_t)val;
    if (val > UINTPTR_MAX - img.base) return 0;
    uintptr_t a = img.base + val;
    return LibContains(img, a, sz) ? a : 0;
}

// ─── ElfResolver — GNU_HASH + SYSV_HASH, không dùng DobbySymbolResolver ─────
class ElfResolver {
    const LibImage& img_;
    uintptr_t syms_ = 0, strs_ = 0, sysv_ = 0, gnu_ = 0;
    size_t    str_sz_ = 0;

    void* SymAt(uint32_t idx, const char* name) const {
        uintptr_t a = LibArrAddr(syms_, idx, sizeof(ElfW(Sym)));
        if (!LibContains(img_, a, sizeof(ElfW(Sym)))) return nullptr;
        ElfW(Sym) s; memcpy(&s, reinterpret_cast<const void*>(a), sizeof(s));
        if (s.st_shndx == SHN_UNDEF || s.st_name >= str_sz_) return nullptr;
        unsigned ty = s.st_info & 0xf, bnd = s.st_info >> 4;
        if ((ty != STT_FUNC && ty != STT_NOTYPE) || (bnd != STB_GLOBAL && bnd != STB_WEAK)) return nullptr;
        size_t len = strlen(name) + 1;
        if (len > str_sz_ - s.st_name) return nullptr;
        if (memcmp(reinterpret_cast<const void*>(strs_ + s.st_name), name, len)) return nullptr;
        uintptr_t fn = s.st_value;
        if (s.st_shndx != SHN_ABS) { if (fn > UINTPTR_MAX - img_.base) return nullptr; fn += img_.base; }
        return LibContains(img_, fn, 1, PF_X) ? reinterpret_cast<void*>(fn) : nullptr;
    }
public:
    explicit ElfResolver(const LibImage& img) : img_(img) {}
    bool Init() {
        uintptr_t dyn = 0; size_t cnt = 0;
        for (size_t i = 0; i < img_.phnum; ++i) {
            const ElfW(Phdr)& p = img_.phdr[i];
            if (p.p_type != PT_DYNAMIC || p.p_vaddr > UINTPTR_MAX - img_.base) continue;
            dyn = img_.base + p.p_vaddr; cnt = p.p_memsz / sizeof(ElfW(Dyn)); break;
        }
        if (!cnt || cnt > 65536 || !LibContains(img_, dyn, cnt * sizeof(ElfW(Dyn)))) return false;
        const ElfW(Dyn)* d = reinterpret_cast<const ElfW(Dyn)*>(dyn);
        ElfW(Addr) sym=0, str=0, sysv=0, gnu=0;
        size_t stride = sizeof(ElfW(Sym)); bool term = false;
        for (size_t i = 0; i < cnt; ++i) {
            if (d[i].d_tag == DT_NULL) { term = true; break; }
            switch (d[i].d_tag) {
                case DT_SYMTAB:   sym  = d[i].d_un.d_ptr; break;
                case DT_STRTAB:   str  = d[i].d_un.d_ptr; break;
                case DT_STRSZ:    str_sz_ = d[i].d_un.d_val; break;
                case DT_SYMENT:   stride = d[i].d_un.d_val; break;
                case DT_HASH:     sysv = d[i].d_un.d_ptr; break;
                case DT_GNU_HASH: gnu  = d[i].d_un.d_ptr; break;
            }
        }
        if (!term || stride != sizeof(ElfW(Sym)) || !str_sz_) return false;
        syms_ = LibDynAddr(img_, sym, sizeof(ElfW(Sym)));
        strs_ = LibDynAddr(img_, str, str_sz_);
        sysv_ = LibDynAddr(img_, sysv, 8);
        gnu_  = LibDynAddr(img_, gnu, 16);
        return syms_ && strs_ && (gnu_ || sysv_);
    }
    void* Find(const char* name) const {
        if (!name || !syms_ || !strs_) return nullptr;
        if (gnu_) {
            uint32_t bc=0, fs=0, blc=0;
            LibReadU32(img_, gnu_, &bc); LibReadU32(img_, gnu_+4, &fs); LibReadU32(img_, gnu_+8, &blc);
            uintptr_t bkts = LibArrAddr(gnu_+16, blc, sizeof(ElfW(Addr)));
            uintptr_t chns = LibArrAddr(bkts, bc, 4);
            if (bc && blc && bkts && chns) {
                uint32_t hash = 5381, idx = 0;
                for (const unsigned char* p = (const unsigned char*)name; *p; ++p) hash = hash*33+*p;
                if (LibReadU32(img_, LibArrAddr(bkts, hash%bc, 4), &idx) && idx >= fs) {
                    for (size_t s=0; s<1048576 && idx!=UINT32_MAX; ++s, ++idx) {
                        uint32_t c=0; if (!LibReadU32(img_, LibArrAddr(chns, idx-fs, 4), &c)) break;
                        if ((c|1u)==(hash|1u)) { void* f=SymAt(idx,name); if (f) return f; }
                        if (c&1u) break;
                    }
                }
            }
        }
        if (sysv_) {
            uint32_t bc=0, sc=0;
            LibReadU32(img_, sysv_, &bc); LibReadU32(img_, sysv_+4, &sc);
            uintptr_t bkts=sysv_+8, chns=LibArrAddr(bkts, bc, 4);
            if (bc && sc && chns) {
                uint32_t hash=0, idx=0;
                for (const unsigned char* p = (const unsigned char*)name; *p; ++p) {
                    hash=(hash<<4)+*p; uint32_t hi=hash&0xf0000000u;
                    if (hi) hash^=hi>>24; hash&=~hi;
                }
                if (LibReadU32(img_, LibArrAddr(bkts, hash%bc, 4), &idx)) {
                    for (size_t s=0; idx && idx<sc && s<1048576; ++s) {
                        void* f=SymAt(idx,name); if (f) return f;
                        if (!LibReadU32(img_, LibArrAddr(chns, idx, 4), &idx)) break;
                    }
                }
            }
        }
        return nullptr;
    }
};

// ─── FindXlua ────────────────────────────────────────────────────────────────
static bool FindXlua() {
    bool found = false;
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* data) -> int {
        if (!info->dlpi_name) return 0;
        const char* leaf = strrchr(info->dlpi_name, '/');
        leaf = leaf ? leaf+1 : info->dlpi_name;
        if (strcmp(leaf, "libxlua.so") != 0 || !info->dlpi_phdr || !info->dlpi_phnum) return 0;
        g_xlua = {info->dlpi_addr, info->dlpi_phdr, info->dlpi_phnum};
        *static_cast<bool*>(data) = true;
        return 1;
    }, &found);
    return found;
}

// ─── Lua function pointers ───────────────────────────────────────────────────
typedef int (*luaL_loadbufferx_t)(void*, const char*, size_t, const char*, const char*);
typedef int (*lua_pcall_t)(void*, int, int, int);
static luaL_loadbufferx_t o_luaL_loadbufferx = nullptr;
static lua_pcall_t        o_lua_pcall         = nullptr;
static void*              g_lua_state         = nullptr;

// ─── ApplyRuntimePatch ───────────────────────────────────────────────────────
// Inject vào Lua state đang chạy:
//   - Cập nhật DU_lgl_mul (hệ số sát thương) và DU_lgl_god (bất tử)
//   - Lần đầu: wrap calModifiedDamage trong package.loaded
//   - Lần sau: chỉ cập nhật biến toàn cục (wrap không chạy lại)
static void ApplyRuntimePatch(void* L) {
    if (!L || !o_luaL_loadbufferx || !o_lua_pcall) return;
    bool prev = false;
    if (!g_in_patch.compare_exchange_strong(prev, true, std::memory_order_acq_rel)) return;

    int mul = g_dmg.load(std::memory_order_acquire) ? (int)g_dmgmul.load(std::memory_order_acquire) : 1;
    if (mul < 1) mul = 1;
    const char* god_str = g_god.load(std::memory_order_acquire) ? "true" : "false";

    char code[2048];
    snprintf(code, sizeof(code),
        "pcall(function()\n"
        "  DU_lgl_mul=%d\n"
        "  DU_lgl_god=%s\n"
        "  if not DU_lgl_orig then\n"
        "    local DU=nil\n"
        "    for _,v in pairs(package.loaded) do\n"
        "      if type(v)=='table' and type(v.calModifiedDamage)=='function' then DU=v;break end\n"
        "    end\n"
        "    if DU then\n"
        "      DU_lgl_orig=DU.calModifiedDamage\n"
        "      DU.calModifiedDamage=function(i,t,d)\n"
        "        local r=DU_lgl_orig(i,t,d)\n"
        "        if DU_lgl_god and t and t.camp==EPlayerCamp.Attacker then return 0 end\n"
        "        local m=DU_lgl_mul\n"
        "        if m and m>1 and i and i.camp==EPlayerCamp.Attacker then return r*m end\n"
        "        return r\n"
        "      end\n"
        "    end\n"
        "  end\n"
        "end)\n",
        mul, god_str
    );

    if (o_luaL_loadbufferx(L, code, strlen(code), "hamod_patch", nullptr) == 0) {
        o_lua_pcall(L, 0, 0, 0);
        LOGI("[PATCH] dmg=%d mul=%d god=%s", (int)g_dmg.load(), mul, god_str);
    } else {
        LOGE("[PATCH] load fail");
    }

    g_in_patch.store(false, std::memory_order_release);
}

// ─── Hooks ──────────────────────────────────────────────────────────────────
// Số lần lua_pcall đầu tiên tự động thử patch (bao phủ mọi Lua state sau khi inject).
// HAMOD inject sau khi game đã chạy → state battle có thể không phải state đầu tiên.
static std::atomic<int> g_patch_attempts{0};

static int h_luaL_loadbufferx(void* L, const char* buff, size_t sz, const char* name, const char* mode) {
    if (L) g_lua_state = L;
    // Script mới load → có thể là state battle → cờ kích hoạt patch
    g_patch_needed.store(1, std::memory_order_release);
    return o_luaL_loadbufferx ? o_luaL_loadbufferx(L, buff, sz, name, mode) : 0;
}

static int h_lua_pcall(void* L, int nargs, int nresults, int msgh) {
    if (L) g_lua_state = L;
    bool want = g_patch_needed.exchange(0, std::memory_order_acq_rel);
    int att = g_patch_attempts.load(std::memory_order_relaxed);
    // Retry 200 lần đầu (đảm bảo hit đúng battle state) + mỗi khi config đổi
    if ((want || att < 200) && !g_in_patch.load(std::memory_order_acquire)) {
        if (att < 200) g_patch_attempts.fetch_add(1, std::memory_order_relaxed);
        ApplyRuntimePatch(L);
    }
    return o_lua_pcall ? o_lua_pcall(L, nargs, nresults, msgh) : 0;
}

// ─── h_t: đợi libxlua.so → hook ─────────────────────────────────────────────
static void* h_t(void*) {
    LOGI("[%s] thread start", BUILD_ID);
    for (int i = 0; i < 120; ++i) {
        if (FindXlua()) break;
        sleep(1);
    }
    if (!g_xlua.base) { LOGE("libxlua.so NOT found"); return nullptr; }
    LOGI("[%s] xlua base=%p", BUILD_ID, reinterpret_cast<void*>(g_xlua.base));

    ElfResolver res(g_xlua);
    if (!res.Init()) { LOGE("xlua ELF init FAIL"); return nullptr; }

    void* sym_load = res.Find(OBFUSCATE("luaL_loadbufferx"));
    void* sym_call = res.Find(OBFUSCATE("lua_pcall"));
    int hooked = 0;

    if (sym_load) {
        if (DobbyHook(sym_load, (void*)h_luaL_loadbufferx, (void**)&o_luaL_loadbufferx) == 0 && o_luaL_loadbufferx)
            { LOGI("Hook OK: luaL_loadbufferx"); ++hooked; }
        else LOGE("Hook FAIL: luaL_loadbufferx");
    } else LOGE("Sym NOT found: luaL_loadbufferx");

    if (sym_call) {
        if (DobbyHook(sym_call, (void*)h_lua_pcall, (void**)&o_lua_pcall) == 0 && o_lua_pcall)
            { LOGI("Hook OK: lua_pcall"); ++hooked; }
        else LOGE("Hook FAIL: lua_pcall");
    } else LOGE("Sym NOT found: lua_pcall");

    LOGI("[%s] READY: %d/2 hooks", BUILD_ID, hooked);
    if (hooked > 0) g_patch_needed.store(1, std::memory_order_release);
    return nullptr;
}

// ─── ParseConfig + Doc_Config_Ngam ───────────────────────────────────────────
// HAMOD ghi: files/mods/config_com.bxzyvn.gg.txt
// Format: "f0 f1 f2 ..."  (7 slots, space-separated float)
//   f0 = g_dmg     (0/1)
//   f1 = g_dmgmul  (1..100)
//   f2 = g_god     (0/1)
static bool ParseConfig(const char* text) {
    if (!text) return false;
    float v[7] = {}; unsigned count = 0;
    const char* c = text;
    while (true) {
        while (isspace((unsigned char)*c)) ++c;
        if (!*c) break;
        if (count == 7) return false;
        char* end = nullptr; errno = 0;
        float val = strtof(c, &end);
        if (end == c || errno == ERANGE || !std::isfinite(val) ||
            (*end && !isspace((unsigned char)*end))) return false;
        v[count++] = val;
        c = end;
    }
    if (count < 3) return false;

    bool  dmg = v[0] >= 0.5f;
    float mul = v[1] < 1.0f ? 1.0f : (v[1] > 99999.0f ? 99999.0f : v[1]);
    bool  god = v[2] >= 0.5f;

    bool changed = (dmg != g_dmg.load(std::memory_order_relaxed)) ||
                   (mul != g_dmgmul.load(std::memory_order_relaxed)) ||
                   (god != g_god.load(std::memory_order_relaxed));
    g_dmg   .store(dmg, std::memory_order_release);
    g_dmgmul.store(mul, std::memory_order_release);
    g_god   .store(god, std::memory_order_release);
    if (changed) g_patch_needed.store(1, std::memory_order_release);
    return true;
}

void* Doc_Config_Ngam(void*) {
    const char* root = getenv("MODMENU_LOG_DIR");
    if (!root || !*root) root = "/data/data/menu.nextgen.launcher/files";
    char paths[2][1024];
    snprintf(paths[0], sizeof(paths[0]), "%s/mods/config_" PKG_NAME ".txt", root);
    snprintf(paths[1], sizeof(paths[1]), "%s/config_"      PKG_NAME ".txt", root);
    LOGI("[%s] config: %s", BUILD_ID, paths[0]);
    int last_status = -1;
    while (true) {
        FILE* f = nullptr; int sel = -1;
        for (int i = 0; i < 2; ++i) { f = fopen(paths[i], "r"); if (f) { sel = i; break; } }
        if (!f) {
            if (last_status != 0) LOGE("Config not found");
            last_status = 0; usleep(500000); continue;
        }
        char text[256] = {};
        size_t len = fread(text, 1, sizeof(text)-1, f);
        bool ok = !ferror(f) && len < sizeof(text)-1;
        fclose(f);
        if (!ok || memchr(text, '\0', len) || !ParseConfig(text)) {
            if (last_status != 1) LOGE("Config invalid: %s", paths[sel]);
            last_status = 1; usleep(500000); continue;
        }
        g_config_loaded.store(true, std::memory_order_release);
        if (last_status != 2)
            LOGI("Config: dmg=%d mul=%.0f god=%d",
                 (int)g_dmg.load(), g_dmgmul.load(), (int)g_god.load());
        last_status = 2;
        usleep(500000);
    }
    return nullptr;
}

// ─── Entry point ─────────────────────────────────────────────────────────────
__attribute__((constructor))
void l_m() {
    LOGI("[%s] injected!", BUILD_ID);
    pthread_t t1, t2;
    if (pthread_create(&t1, nullptr, h_t,             nullptr) == 0) pthread_detach(t1);
    if (pthread_create(&t2, nullptr, Doc_Config_Ngam, nullptr) == 0) pthread_detach(t2);
}
