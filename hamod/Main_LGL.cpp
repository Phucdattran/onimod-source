// ============================================================
//  LGL MOD — com.bxzyvn.gg (v3.0.6)
//  Test qua LGL Test Loader (máy root), sau đó ghép APK.
//
//  Tính năng (menu LGL):
//    [0]    Tang Sat Thuong — damage toggle
//    [1]    He So Sat Thuong — InputValue (gõ số bất kỳ, tối đa 9999999)
//    [2]    Toc Do Tran — Spinner x1/x2/x3/x5 (hook Time.get_deltaTime)
//    [3]    Bat Tu (God Mode) — calModifiedDamage trả 0 cho phe mình là target
// ============================================================

#define LOG_TAG      "BXZYVN_MOD"
#define GAME_NAME    "🎮 BXZYvn Mod"
#define GAME_PACKAGE "com.bxzyvn.gg"

#include "Includes/LGLTrace.h"

#include <pthread.h>
#include <jni.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <link.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <android/log.h>

#include "Includes/obfuscate.h"
#include "Includes/Logger.h"
#include "Includes/Utils.h"
#include "Menu/Setup.h"
#include <Dobby/dobby.h>
#include "MenuStrings.h"
#include "Includes/EngineVIP.h"

// ═══════════════════════════════════════════════════════════
//  1. IL2CPP API
// ═══════════════════════════════════════════════════════════
uintptr_t il2cpp_base = 0;

void*       (*il2cpp_domain_get)()                                         = nullptr;
void*       (*il2cpp_thread_attach)(void*)                                 = nullptr;
void**      (*il2cpp_domain_get_assemblies)(void*, size_t*)                = nullptr;
void*       (*il2cpp_assembly_get_image)(void*)                            = nullptr;
size_t      (*il2cpp_image_get_class_count)(void*)                         = nullptr;
void*       (*il2cpp_image_get_class)(void*, size_t)                       = nullptr;
void*       (*il2cpp_class_from_name)(void*, const char*, const char*)     = nullptr;
const char* (*il2cpp_class_get_name)(void*)                                = nullptr;
void*       (*il2cpp_class_get_methods)(void*, void**)                     = nullptr;
const char* (*il2cpp_method_get_name)(void*)                               = nullptr;
uint32_t    (*il2cpp_method_get_param_count)(void*)                        = nullptr;
void*       (*il2cpp_class_get_field_from_name)(void*, const char*)        = nullptr;
size_t      (*il2cpp_field_get_offset)(void*)                              = nullptr;
void*       (*il2cpp_string_new)(const char*)                              = nullptr;

// ═══════════════════════════════════════════════════════════
//  2. ElfSym — KHÔNG dùng dlopen (chỉ đọc bộ nhớ)
// ═══════════════════════════════════════════════════════════
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
    LOAD(il2cpp_domain_get);       LOAD(il2cpp_thread_attach);
    LOAD(il2cpp_domain_get_assemblies); LOAD(il2cpp_assembly_get_image);
    LOAD(il2cpp_image_get_class_count); LOAD(il2cpp_image_get_class);
    LOAD(il2cpp_class_from_name);  LOAD(il2cpp_class_get_name);
    LOAD(il2cpp_class_get_methods); LOAD(il2cpp_method_get_name);
    LOAD(il2cpp_method_get_param_count);
    LOAD(il2cpp_class_get_field_from_name); LOAD(il2cpp_field_get_offset);
    LOAD(il2cpp_string_new);
    #undef LOAD
    bool ok = il2cpp_domain_get && il2cpp_domain_get_assemblies &&
              il2cpp_assembly_get_image && il2cpp_class_from_name &&
              il2cpp_class_get_methods && il2cpp_method_get_name;
    LOGI("InitIL2CppAPI: %s", ok ? "OK" : "FAIL");
    return ok;
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

// ── Tìm symbol trong BẤT KỲ thư viện nào đã nạp (dùng cho luaL_loadbufferx)
struct FindSymCtx { const char* name; void* result; };
static int find_sym_cb(struct dl_phdr_info* info, size_t, void* data) {
    if (!info->dlpi_name || !*info->dlpi_name) return 0;
    FindSymCtx* ctx = (FindSymCtx*)data;
    uintptr_t base = info->dlpi_addr;
    const ElfW(Phdr)* phdr = info->dlpi_phdr;
    int phnum = info->dlpi_phnum;
    auto fix = [base](uintptr_t p) { return p < base ? base + p : p; };

    const ElfW(Dyn)* dyn = nullptr;
    for (int i = 0; i < phnum; i++)
        if (phdr[i].p_type == PT_DYNAMIC)
            dyn = (const ElfW(Dyn)*)(base + phdr[i].p_vaddr);
    if (!dyn) return 0;

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
    if (!symtab || !strtab || (!gnu && !sysv)) return 0;
    size_t nsyms = 0;
    if (sysv) {
        nsyms = sysv[1];
    } else {
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

// ═══════════════════════════════════════════════════════════
//  3. HELPERS
// ═══════════════════════════════════════════════════════════
static inline void* MethodFnPtr(void* mi) { return mi ? *(void**)mi : nullptr; }

void* FindClass(const char* ns, const char* name) {
    size_t n = 0;
    void** list = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &n);
    for (size_t i = 0; i < n; i++) {
        void* img = il2cpp_assembly_get_image(list[i]); if (!img) continue;
        if (void* k = il2cpp_class_from_name(img, ns, name)) return k;
    }
    return nullptr;
}
void* FindClassAnywhere(const char* name) {
    size_t n = 0;
    void** list = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &n);
    for (size_t i = 0; i < n; i++) {
        void* img = il2cpp_assembly_get_image(list[i]); if (!img) continue;
        if (il2cpp_image_get_class_count && il2cpp_image_get_class && il2cpp_class_get_name) {
            size_t nc = il2cpp_image_get_class_count(img);
            for (size_t j = 0; j < nc; j++) {
                void* k = il2cpp_image_get_class(img, j);
                const char* kn = k ? il2cpp_class_get_name(k) : nullptr;
                if (kn && strcmp(kn, name) == 0) return k;
            }
        } else if (void* k = il2cpp_class_from_name(img, "", name)) return k;
    }
    return nullptr;
}
void* FindMethod(void* klass, const char* name, int nParams) {
    if (!klass) return nullptr;
    void* iter = nullptr;
    while (void* m = il2cpp_class_get_methods(klass, &iter)) {
        const char* n = il2cpp_method_get_name(m); if (!n || strcmp(n, name) != 0) continue;
        if (nParams >= 0 && il2cpp_method_get_param_count &&
            (int)il2cpp_method_get_param_count(m) != nParams) continue;
        return m;
    }
    return nullptr;
}
static void* GetClass(const char* ns, const char* cls) {
    return (ns && *ns) ? FindClass(ns, cls) : FindClassAnywhere(cls);
}
void* MethodAddr(const char* ns, const char* cls, const char* method, int nParams) {
    void* k = GetClass(ns, cls);
    if (!k) { LOGE("NOT FOUND class: %s", cls); return nullptr; }
    void* m = FindMethod(k, method, nParams);
    if (!m) { LOGE("NOT FOUND: %s.%s(%d)", cls, method, nParams); return nullptr; }
    return MethodFnPtr(m);
}
template <typename T> static inline T& Field(void* obj, int off) {
    return *(T*)((uintptr_t)obj + off);
}
static bool DoHook(void* addr, void* hook, void** orig, const char* name) {
    if (!addr) { LOGE("Hook SKIP (NULL): %s", name); return false; }
    int r = DobbyHook(addr, hook, orig);
    if (r == 0) { LOGI("Hook OK: %s @ %p", name, addr); return true; }
    LOGE("Hook FAIL(%d): %s @ %p", r, name, addr); return false;
}
#define HOOK(ns, cls, method, nParams, fn) \
    DoHook(MethodAddr(ns, cls, method, nParams), (void*)h_##fn, (void**)&o_##fn, cls "." method)

// ═══════════════════════════════════════════════════════════
//  4. TRẠNG THÁI
// ═══════════════════════════════════════════════════════════
static bool  g_dmg     = false;
static int   g_dmgmul  = 1;
static bool  g_god     = false;
static bool  g_spd     = false;
static int   g_spdmul  = 1;

// Lua runtime patch (package.loaded: calModifiedDamage + setCurHp)
typedef int (*lua_pcall_t)(void*, int, int, int);
static lua_pcall_t  o_lua_pcall    = nullptr;   // Dobby trampoline → original lua_pcall
static void*        g_L            = nullptr;   // Lua state, capture từ h_lua_pcall
static bool         g_dmg_seen     = false;     // DamageUtil.lua đã qua hook
static int          g_patch_try    = 0;         // số lần thử install (tối đa 3)
static volatile int g_patch_needed = 0;         // 1 = cần re-apply (set bởi Changes)
static volatile int g_in_patch     = 0;         // re-entrancy guard cho ApplyRuntimePatch

// ═══════════════════════════════════════════════════════════
//  5. HOOK: luaL_loadbufferx (dump Lua) + lua_pcall (trigger patch)
//
//  Runtime patch strategy:
//  - lua_pcall hook (h_lua_pcall) captures Lua state + triggers ApplyRuntimePatch
//    (luaL_loadbufferx không trigger patch vì script load trước khi hook được gắn)
//  - ApplyRuntimePatch dùng o_luaL_loadbufferx + o_lua_pcall để inject Lua code:
//      a) Wrap calModifiedDamage → DU_lgl_mul (damage multiplier)
//      b) Wrap setCurHp → PD_lgl_god (god mode)
// ═══════════════════════════════════════════════════════════
typedef int (*luaL_loadbufferx_t)(void* L, const char* buff, size_t sz, const char* name, const char* mode);
static luaL_loadbufferx_t o_luaL_loadbufferx = nullptr;

// Inject Lua code vào Lua state để patch calModifiedDamage
// - Damage multiplier: i.camp==EPlayerCamp.Attacker → nhân
// - God mode: t.camp==EPlayerCamp.Attacker → trả 0 damage
// Dùng outer pcall để absorb mọi lỗi runtime
static void ApplyRuntimePatch(void* L) {
    if (!o_lua_pcall || !o_luaL_loadbufferx) return;
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
        "  if not LGL_diag_done then\n"
        "    LGL_diag_done=true\n"
        "    pcall(function()\n"
        "      local f=io.open('/data/data/com.bxzyvn.gg/files/lgl_modules.txt','w')\n"
        "      if f then\n"
        "        for k,v in pairs(package.loaded) do\n"
        "          if type(v)=='table' then\n"
        "            f:write('=='..tostring(k)..'==\\n')\n"
        "            for fk,fv in pairs(v) do\n"
        "              if type(fv)=='function' then f:write('  '..tostring(fk)..'\\n') end\n"
        "            end\n"
        "          end\n"
        "        end\n"
        "        f:close()\n"
        "      end\n"
        "    end)\n"
        "  end\n"
        "end)\n",
        g_dmg ? g_dmgmul : 1, g_god ? "true" : "false"
    );
    int r1 = o_luaL_loadbufferx(L, code, strlen(code), "@lgl_patch", nullptr);
    if (r1 != 0) { LOGE("[Patch] compile FAIL r=%d", r1); return; }
    o_lua_pcall(L, 0, 0, 0);
    int eff = g_dmg ? g_dmgmul : 1;
    LOGI("[Patch] inject OK: dmg=%d x%d god=%d", g_dmg, eff, g_god);
}

static int h_luaL_loadbufferx(void* L, const char* buff, size_t sz, const char* name, const char* mode) {
    T_COUNT(luaL_loadbufferx);
    if (!buff || sz == 0) goto call_orig;

    // Lưu Lua state
    if (!g_L) g_L = L;

    // Auto-dump mọi script (O_EXCL = chỉ ghi lần đầu)
    {
        static bool dir_made = false;
        if (!dir_made) {
            mkdir("/sdcard/Download/LGL_logs", 0755);
            mkdir("/sdcard/Download/LGL_logs/bxzyvn_lua", 0755);
            dir_made = true;
        }
        char path[256];
        const char* cname = (name && *name) ? name : "unknown";
        char safe[64]; int si = 0;
        for (int i = 0; cname[i] && si < 60; i++) {
            char c = cname[i];
            if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.')
                safe[si++] = c;
            else safe[si++] = '_';
        }
        safe[si] = 0;
        snprintf(path, sizeof(path), "/sdcard/Download/LGL_logs/bxzyvn_lua/%s.lua", safe);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) { write(fd, buff, sz); close(fd); }
    }

    // Đánh dấu DamageUtil đã thấy (chỉ dùng để log)
    if (!g_dmg_seen && name && strstr(name, "DamageUtil")) {
        g_dmg_seen = true;
        LOGI("[Lua] DamageUtil seen — hook da cai, thu patch luon");
    }

call_orig:
    return o_luaL_loadbufferx ? o_luaL_loadbufferx(L, buff, sz, name, mode) : 0;
}

// lua_pcall hook — capture Lua state + trigger ApplyRuntimePatch
// Được gọi teên mỗi pcall của game → luôn có Lua state L hợp lệ
// g_in_patch guard: ApplyRuntimePatch gọi o_lua_pcall nội bộ → tránh đệ quy vô hạn
static int h_lua_pcall(void* L, int nargs, int nresults, int msgh) {
    T_COUNT(lua_pcall);
    if (!g_in_patch && o_luaL_loadbufferx) {
        if (!g_L) g_L = L;
        if (g_patch_try < 3 || g_patch_needed) {
            g_in_patch = 1;
            if (g_patch_try < 3) g_patch_try++;
            if (g_patch_needed)  g_patch_needed = 0;
            ApplyRuntimePatch(L);
            g_in_patch = 0;
        }
    }
    return o_lua_pcall ? o_lua_pcall(L, nargs, nresults, msgh) : 0;
}

// ═══════════════════════════════════════════════════════════
//  6. HOOK: Time.get_deltaTime — Tốc độ trận
//  Nhân deltaTime → Lua SceneManager chạy nhiều tick hơn mỗi frame
//  dump.cs: public static System.Single get_deltaTime()
// ═══════════════════════════════════════════════════════════
typedef float (*get_deltaTime_t)(void*);
static get_deltaTime_t o_get_deltaTime = nullptr;

static float h_get_deltaTime(void* mi) {
    T_COUNT(get_deltaTime);
    float dt = o_get_deltaTime ? o_get_deltaTime(mi) : 0.016f;
    return (g_spd && g_spdmul > 1) ? dt * g_spdmul : dt;
}

// thread thử lại hook luaL_loadbufferx + lua_pcall — chạy vô hạn cho đến khi tìm được
// libxlua.so chỉ nạp khi vào trận nên không giới hạn thời gian chờ
static void* lua_retry_thread(void*) {
    LOGI("[Lua] Retry thread: cho libxlua.so nap (co the chi load khi vao tran)...");
    for (int i = 0; !o_luaL_loadbufferx || !o_lua_pcall; i++) {
        usleep(2000000); // 2s mỗi lần
        if (i % 15 == 0) LOGI("[Lua] Van cho libxlua... (~%ds)", i*2);
        if (!o_luaL_loadbufferx) {
            void* sym = FindSymAnywhere("luaL_loadbufferx");
            if (sym) {
                bool ok = DoHook(sym, (void*)h_luaL_loadbufferx, (void**)&o_luaL_loadbufferx, "luaL_loadbufferx");
                if (ok) { LOGI("[Lua] Hook MUON luaL_loadbufferx OK (sau ~%ds)", i*2); g_hook_ok++; }
                else   { LOGE("[Lua] Hook MUON luaL_loadbufferx FAIL"); g_hook_fail++; }
            }
        }
        if (!o_lua_pcall) {
            void* pcall_sym = FindSymAnywhere("lua_pcall");
            if (pcall_sym) {
                bool ok2 = DoHook(pcall_sym, (void*)h_lua_pcall, (void**)&o_lua_pcall, "lua_pcall");
                if (ok2) { LOGI("[Lua] Hook MUON lua_pcall OK (sau ~%ds)", i*2); g_hook_ok++; }
                else     { LOGE("[Lua] Hook MUON lua_pcall FAIL"); g_hook_fail++; }
            }
        }
        if (o_luaL_loadbufferx && o_lua_pcall) {
            LOGI("[Lua] Ca 2 hook OK sau ~%ds — san sang patch!", i*2);
            return nullptr;
        }
        // Cứ 60s log danh sách lib để debug (chỉ khi vẫn chưa tìm được)
        if (i > 0 && i % 30 == 0) {
            LOGI("[Lua] Sau %ds: danh sach lib:", i*2);
            dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void*) -> int {
                if (info->dlpi_name && *info->dlpi_name) LOGI("[lib] %s", info->dlpi_name);
                return 0;
            }, nullptr);
        }
    }
    return nullptr;
}

// ═══════════════════════════════════════════════════════════
//  9. ResolveAll
// ═══════════════════════════════════════════════════════════
static void ResolveAll() {
    LOGI("=== Resolve START ===");

    // [0/1/2] Lua dump + runtime patch — hook luaL_loadbufferx + lua_pcall
    {
        bool need_retry = false;
        void* lua_sym = FindSymAnywhere(OBFUSCATE("luaL_loadbufferx"));
        if (lua_sym) {
            bool ok = DoHook(lua_sym, (void*)h_luaL_loadbufferx, (void**)&o_luaL_loadbufferx, "luaL_loadbufferx");
            if (ok) { g_hook_ok++; } else { g_hook_fail++; need_retry = true; }
        } else {
            LOGI("[Lua] Chua thay luaL_loadbufferx — lib 'lua' dang nap:");
            dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void*) -> int {
                if (info->dlpi_name && (strstr(info->dlpi_name, "lua") || strstr(info->dlpi_name, "xlua")))
                    LOGI("[Lua?] %s", info->dlpi_name);
                return 0;
            }, nullptr);
            need_retry = true;
        }
        void* pcall_sym = FindSymAnywhere(OBFUSCATE("lua_pcall"));
        if (pcall_sym) {
            bool ok2 = DoHook(pcall_sym, (void*)h_lua_pcall, (void**)&o_lua_pcall, "lua_pcall");
            if (ok2) { g_hook_ok++; } else { g_hook_fail++; need_retry = true; }
        } else {
            LOGI("[Lua] Chua thay lua_pcall");
            need_retry = true;
        }
        if (need_retry) {
            pthread_t pt; pthread_create(&pt, nullptr, lua_retry_thread, nullptr); pthread_detach(pt);
            LOGI("[Lua] Spawn retry thread OK");
        }
    }

    // [2] Speed — Time.get_deltaTime (UnityEngine)
    T_HOOK("UnityEngine", "Time", "get_deltaTime", 0, get_deltaTime);

    TraceReport();
    LOGI("=== Resolve DONE ===");
}

// ═══════════════════════════════════════════════════════════
//  10. MENU — khớp menu.js (4 tính năng; dump tự chạy, ko cần toggle)
// ═══════════════════════════════════════════════════════════
jobjectArray GetFeatureList(JNIEnv* env, jobject) {
    const char* features[] = {
        OBFUSCATE("Category_" GAME_NAME),
        OBFUSCATE("0_Toggle_Tang Sat Thuong"),
        OBFUSCATE("1_InputValue_He So Sat Thuong"),
        OBFUSCATE("2_Spinner_Toc Do Tran_x1,x2,x3,x5"),
        OBFUSCATE("3_Toggle_Bat Tu (God Mode)"),
    };
    int total = sizeof(features) / sizeof(features[0]);
    jobjectArray ret = env->NewObjectArray(
        total, env->FindClass("java/lang/String"), env->NewStringUTF(""));
    for (int i = 0; i < total; i++) {
        jstring s = env->NewStringUTF(features[i]);
        env->SetObjectArrayElement(ret, i, s);
        env->DeleteLocalRef(s);
    }
    return ret;
}

void Changes(JNIEnv* env, jclass, jobject,
             jint featNum, jstring featName, jint value, jboolean boolean, jstring str) {
    T_STEP("bam tinh nang so %d (value=%d, bat=%d)", featNum, value, boolean);
    LOGI("Menu: tinh nang %d -> value=%d bat=%d", featNum, value, boolean);
    static const int spd_table[] = {1, 2, 3, 5};
    switch (featNum) {
        case 0:
            g_dmg = boolean;
            LOGI("Damage Mod: %s (x%d)", g_dmg ? "BAT" : "TAT", g_dmgmul);
            g_patch_needed = 1;
            break;
        case 1: {
            int v = (value < 1) ? 1 : (value > 9999999 ? 9999999 : value);
            g_dmgmul = v;
            LOGI("Damage Multiplier: x%d", g_dmgmul);
            g_patch_needed = 1;
            break;
        }
        case 2: {
            int idx = (value < 0) ? 0 : (value > 3 ? 3 : value);
            g_spdmul = spd_table[idx];
            g_spd = (g_spdmul > 1);
            LOGI("Speed: x%d (idx=%d)", g_spdmul, idx);
            break;
        }
        case 3:
            g_god = boolean;
            LOGI("God Mode: %s", g_god ? "BAT" : "TAT");
            g_patch_needed = 1;
            break;
    }
}

// ═══════════════════════════════════════════════════════════
//  11. KHỞI TẠO — KHÔNG SỬA
// ═══════════════════════════════════════════════════════════
static volatile bool g_il2cpp_ready = false;
static int (*o_il2cpp_init)(const char*) = nullptr;
static int h_il2cpp_init(const char* domain_name) {
    int r = o_il2cpp_init ? o_il2cpp_init(domain_name) : 0;
    g_il2cpp_ready = true;
    return r;
}

void* h_t(void*) {
    T_STEP("cho libil2cpp.so");
    while (!IsIL2CppLoaded()) usleep(100000);
    LOGI("libil2cpp.so base: 0x%" PRIxPTR, il2cpp_base);

    if (!InitIL2CppAPI()) { LOGE("InitIL2CppAPI FAIL - dung"); return nullptr; }

    T_STEP("hook il2cpp_init");
    void* initAddr = ElfSym("il2cpp_init");
    bool hooked = initAddr && DobbyHook(initAddr, (void*)h_il2cpp_init, (void**)&o_il2cpp_init) == 0;
    LOGI("Hook il2cpp_init: %s", hooked ? "OK" : "FAIL");

    T_STEP("cho il2cpp_init");
    for (int i = 0; i < 300 && !g_il2cpp_ready; i++) usleep(100000);
    if (g_il2cpp_ready) LOGI("il2cpp_init xong");
    else LOGE("Khong thay il2cpp_init -> cho them 5s");
    for (int i = 0; i < 20; i++) usleep(250000);

    T_STEP("attach + cho assemblies");
    void* domain = il2cpp_domain_get();
    if (domain && il2cpp_thread_attach) il2cpp_thread_attach(domain);
    { size_t last = 0; int stable = 0;
      while (stable < 5) {
          size_t n = 0;
          il2cpp_domain_get_assemblies(domain, &n);
          if (n > 0 && n == last) stable++; else { last = n; stable = 0; }
          usleep(100000);
      }
      LOGI("Assemblies on dinh: %zu", last); }

    T_STEP("ResolveAll");
    ResolveAll();
    isGameReady = true;
    LOGI("✅ MOD READY!");
    TraceStartCallReports();
    return nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_com_android_support_Main_CheckOverlayPermission(JNIEnv*, jclass, jobject) {}

__attribute__((constructor))
void l_m() {
    LOGI(".so injected");
    pthread_t ptid;
    pthread_create(&ptid, nullptr, h_t, nullptr);
}

#include "Includes/JNI_Boilerplate.h"
