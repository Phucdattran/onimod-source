// com.bxzyvn.gg — TAROT: VĨNH HẰNG THẦN NGỮ
// HAMOD version: quét tất cả lib đã nạp (không giới hạn tên) để tìm Lua symbols
// f0=dmg_toggle  f1=dmg_multiplier(1..99999)  f2=god_mode

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

// ─── State ───────────────────────────────────────────────────────────────────
std::atomic<bool>  g_dmg    {false};
std::atomic<float> g_dmgmul {10.0f};
std::atomic<bool>  g_god    {false};
static std::atomic<int>  g_patch_needed{0};
static std::atomic<bool> g_in_patch    {false};
static std::atomic<bool> g_config_loaded{false};
static std::atomic<int>  g_patch_attempts{0};

// ─── FindSymAnywhere — quét TẤT CẢ lib đã nạp ───────────────────────────────
struct FindSymCtx { const char* name; void* result; };

static int find_sym_cb(struct dl_phdr_info* info, size_t, void* data) {
    if (!info->dlpi_name || !*info->dlpi_name) return 0;
    FindSymCtx* ctx = (FindSymCtx*)data;
    uintptr_t base = info->dlpi_addr;
    const ElfW(Phdr)* phdr = info->dlpi_phdr;
    int phnum = info->dlpi_phnum;

    const ElfW(Dyn)* dyn = nullptr;
    for (int i = 0; i < phnum; i++)
        if (phdr[i].p_type == PT_DYNAMIC)
            dyn = (const ElfW(Dyn)*)(base + phdr[i].p_vaddr);
    if (!dyn) return 0;

    const ElfW(Sym)* symtab = nullptr;
    const char* strtab = nullptr;
    const uint32_t* gnu = nullptr;
    const uint32_t* sysv = nullptr;

    for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; ++d) {
        uintptr_t val = (uintptr_t)d->d_un.d_ptr;
        uintptr_t fval = (val < base) ? base + val : val;
        switch (d->d_tag) {
            case DT_SYMTAB:   symtab = (const ElfW(Sym)*)fval; break;
            case DT_STRTAB:   strtab = (const char*)fval;      break;
            case DT_GNU_HASH: gnu    = (const uint32_t*)fval;  break;
            case DT_HASH:     sysv   = (const uint32_t*)fval;  break;
        }
    }
    if (!symtab || !strtab || (!gnu && !sysv)) return 0;

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
        if (strcmp(strtab + s.st_name, ctx->name) == 0) {
            ctx->result = (void*)(base + s.st_value);
            return 1;
        }
    }
    return 0;
}

static void* FindSymAnywhere(const char* name) {
    FindSymCtx ctx = {name, nullptr};
    dl_iterate_phdr(find_sym_cb, &ctx);
    return ctx.result;
}

// ─── Lua function pointers ───────────────────────────────────────────────────
typedef int (*luaL_loadbufferx_t)(void*, const char*, size_t, const char*, const char*);
typedef int (*lua_pcall_t)(void*, int, int, int);
static luaL_loadbufferx_t o_luaL_loadbufferx = nullptr;
static lua_pcall_t        o_lua_pcall         = nullptr;
static void*              g_lua_state         = nullptr;

// ─── ApplyRuntimePatch ───────────────────────────────────────────────────────
static void ApplyRuntimePatch(void* L) {
    if (!L || !o_luaL_loadbufferx || !o_lua_pcall) return;
    bool prev = false;
    if (!g_in_patch.compare_exchange_strong(prev, true, std::memory_order_acq_rel)) return;

    int mul = g_dmg.load(std::memory_order_acquire)
              ? (int)g_dmgmul.load(std::memory_order_acquire) : 1;
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

// ─── Hooks ───────────────────────────────────────────────────────────────────
static int h_luaL_loadbufferx(void* L, const char* buff, size_t sz, const char* name, const char* mode) {
    if (L) g_lua_state = L;
    g_patch_needed.store(1, std::memory_order_release);
    return o_luaL_loadbufferx ? o_luaL_loadbufferx(L, buff, sz, name, mode) : 0;
}

static int h_lua_pcall(void* L, int nargs, int nresults, int msgh) {
    if (L) g_lua_state = L;
    bool want = g_patch_needed.exchange(0, std::memory_order_acq_rel);
    int att = g_patch_attempts.load(std::memory_order_relaxed);
    if ((want || att < 200) && !g_in_patch.load(std::memory_order_acquire)) {
        if (att < 200) g_patch_attempts.fetch_add(1, std::memory_order_relaxed);
        ApplyRuntimePatch(L);
    }
    return o_lua_pcall ? o_lua_pcall(L, nargs, nresults, msgh) : 0;
}

// ─── Hook helper ─────────────────────────────────────────────────────────────
static bool DoHook(void* addr, void* hook, void** orig, const char* name) {
    if (!addr) { LOGE("Hook SKIP (NULL): %s", name); return false; }
    int r = DobbyHook(addr, hook, orig);
    if (r == 0 && *orig) { LOGI("Hook OK: %s @ %p", name, addr); return true; }
    LOGE("Hook FAIL(%d): %s @ %p", r, name, addr); return false;
}

// ─── Retry thread — quét all libs mỗi 2s cho đến khi hook xong ──────────────
static void* lua_retry_thread(void*) {
    LOGI("[Lua] Retry thread bắt đầu (chờ lib Lua nạp)...");
    for (int i = 0; !o_luaL_loadbufferx || !o_lua_pcall; i++) {
        usleep(2000000);
        if (i % 15 == 0)
            LOGI("[Lua] Vẫn chờ symbols... (~%ds)", i*2);

        if (!o_luaL_loadbufferx) {
            void* sym = FindSymAnywhere(OBFUSCATE("luaL_loadbufferx"));
            if (sym) DoHook(sym, (void*)h_luaL_loadbufferx, (void**)&o_luaL_loadbufferx, "luaL_loadbufferx");
        }
        if (!o_lua_pcall) {
            void* sym = FindSymAnywhere(OBFUSCATE("lua_pcall"));
            if (sym) DoHook(sym, (void*)h_lua_pcall, (void**)&o_lua_pcall, "lua_pcall");
        }
        if (o_luaL_loadbufferx && o_lua_pcall) {
            LOGI("[Lua] Cả 2 hook OK sau ~%ds!", i*2);
            g_patch_needed.store(1, std::memory_order_release);
            return nullptr;
        }
        // Log tên lib mỗi 60s để debug
        if (i > 0 && i % 30 == 0) {
            LOGI("[Lua] Danh sách lib hiện tại:");
            dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void*) -> int {
                if (info->dlpi_name && *info->dlpi_name)
                    LOGI("[lib] %s", info->dlpi_name);
                return 0;
            }, nullptr);
        }
    }
    return nullptr;
}

// ─── h_t: hook ngay khi start, spawn retry thread nếu chưa tìm được ─────────
static void* h_t(void*) {
    LOGI("[%s] thread start", BUILD_ID);

    bool need_retry = false;
    int hooked = 0;

    void* sym_load = FindSymAnywhere(OBFUSCATE("luaL_loadbufferx"));
    if (sym_load) {
        if (DoHook(sym_load, (void*)h_luaL_loadbufferx, (void**)&o_luaL_loadbufferx, "luaL_loadbufferx"))
            ++hooked;
    } else { LOGI("[Lua] luaL_loadbufferx chưa thấy — sẽ retry"); need_retry = true; }

    void* sym_call = FindSymAnywhere(OBFUSCATE("lua_pcall"));
    if (sym_call) {
        if (DoHook(sym_call, (void*)h_lua_pcall, (void**)&o_lua_pcall, "lua_pcall"))
            ++hooked;
    } else { LOGI("[Lua] lua_pcall chưa thấy — sẽ retry"); need_retry = true; }

    LOGI("[%s] Hooks ngay: %d/2", BUILD_ID, hooked);
    if (hooked > 0) g_patch_needed.store(1, std::memory_order_release);

    if (need_retry || hooked < 2) {
        pthread_t pt;
        if (pthread_create(&pt, nullptr, lua_retry_thread, nullptr) == 0)
            pthread_detach(pt);
        LOGI("[Lua] Spawn retry thread OK");
    }
    return nullptr;
}

// ─── ParseConfig + Doc_Config_Ngam ───────────────────────────────────────────
// HAMOD ghi: files/mods/config_com.bxzyvn.gg.txt
// Format: "f0 f1 f2 ..."  (7 slots, space-separated float)
//   f0 = g_dmg     (0/1)
//   f1 = g_dmgmul  (1..99999)
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
