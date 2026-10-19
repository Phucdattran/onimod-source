// ============================================================
//  LGL MOD — com.bxzyvn.gg (v3.0.6)
//  Test qua LGL Test Loader (máy root), sau đó ghép APK.
//
//  Tính năng (menu LGL):
//    [AUTO] Dump Lua — tự dump mọi script vào LGL_logs/bxzyvn_lua/ (không cần toggle)
//    [0]    Tang Sat Thuong — damage toggle
//    [1]    He So Sat Thuong — seekbar 1-20
//    [2]    Bat Tu (God Mode) — PatcherInfo.preRelease (+0x30) = true
//    [3]    GM Mode — KCommand::Exec
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
static bool  g_gm      = false;
static bool  g_nodebug = false;

// ═══════════════════════════════════════════════════════════
//  5. HOOK: luaL_loadbufferx — dump + damage patch
//  Khi g_dump = true: ghi chunk ra file
//  Khi g_dmg = true và g_dmgmul > 1: patch damage trong Lua bytecode
//  (Pattern cụ thể sẽ điền sau khi phân tích file .luac đã dump)
// ═══════════════════════════════════════════════════════════
typedef int (*luaL_loadbufferx_t)(void* L, const char* buff, size_t sz, const char* name, const char* mode);
static luaL_loadbufferx_t o_luaL_loadbufferx = nullptr;

// Patch hàm M.calModifiedDamage trong Lua: nhân damage
// Pattern từ libonavhtny.so — file này chỉ load khi VÀO TRẬN (không phải lobby)
// → chèn "    damage = damage * N;\r\n" ngay sau dòng khai báo hàm
static const char* lua_patch_dmg(const char* buff, size_t sz, size_t* out_sz, const char* name) {
    static const char NEEDLE_CRLF[] = "function M.calModifiedDamage(instigator, target, damage)\r\n";
    static const char NEEDLE_LF[]   = "function M.calModifiedDamage(instigator, target, damage)\n";

    const char* found = nullptr;
    size_t needle_len = 0;
    if ((found = (const char*)memmem(buff, sz, NEEDLE_CRLF, sizeof(NEEDLE_CRLF)-1)))
        needle_len = sizeof(NEEDLE_CRLF)-1;
    else if ((found = (const char*)memmem(buff, sz, NEEDLE_LF, sizeof(NEEDLE_LF)-1)))
        needle_len = sizeof(NEEDLE_LF)-1;

    if (!found) return nullptr;

    char inject[64];
    int injlen = snprintf(inject, sizeof(inject), "    damage = damage * %d;\r\n", g_dmgmul);

    size_t new_sz = sz + (size_t)injlen;
    void* p = mmap(nullptr, new_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;

    const char* fn_end = found + needle_len;
    size_t head = (size_t)(fn_end - buff);
    memcpy((char*)p,                              buff,   head);
    memcpy((char*)p + head,                       inject, (size_t)injlen);
    memcpy((char*)p + head + (size_t)injlen,      fn_end, sz - head);

    mprotect(p, new_sz, PROT_READ);
    *out_sz = new_sz;
    LOGI("[Dmg] patch M.calModifiedDamage x%d trong '%s' (%zu->%zu byte)",
         g_dmgmul, name ? name : "?", sz, new_sz);
    return (const char*)p;
}

static int h_luaL_loadbufferx(void* L, const char* buff, size_t sz, const char* name, const char* mode) {
    T_COUNT(luaL_loadbufferx);
    if (!buff || sz == 0) goto call_orig;

    // Auto-dump mọi script (không cần toggle); O_EXCL = chỉ ghi lần đầu, bỏ qua nếu đã có
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
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644); // O_EXCL: bỏ qua nếu đã dump
        if (fd >= 0) {
            write(fd, buff, sz);
            close(fd);
            LOGI("[Lua] dump: %s (%zu bytes)", cname, sz);
        }
    }

    if (g_dmg && g_dmgmul > 1) {
        size_t new_sz = sz;
        const char* patched = lua_patch_dmg(buff, sz, &new_sz, name);
        if (patched) {
            int r = o_luaL_loadbufferx ? o_luaL_loadbufferx(L, patched, new_sz, name, mode) : 0;
            munmap((void*)patched, new_sz);
            return r;
        }
    }

call_orig:
    return o_luaL_loadbufferx ? o_luaL_loadbufferx(L, buff, sz, name, mode) : 0;
}

// ═══════════════════════════════════════════════════════════
//  6. HOOK: PatcherInfo::Get — God Mode
//  Static method, 0 params → (void* mi) returns PatcherInfo*
//  Sau khi lấy instance: set field preRelease tại +0x30 = true
//  dump.cs: public static PatcherInfo Get(); // 0x269C9A8
//           public System.Boolean preRelease; // 0x30
// ═══════════════════════════════════════════════════════════
typedef void* (*PatcherInfo_Get_t)(void* mi);
static PatcherInfo_Get_t o_PatcherInfo_Get = nullptr;

static void* h_PatcherInfo_Get(void* mi) {
    T_COUNT(PatcherInfo_Get);
    void* inst = o_PatcherInfo_Get ? o_PatcherInfo_Get(mi) : nullptr;
    if (inst && g_god) {
        uint8_t old_val = Field<uint8_t>(inst, 0x30);
        if (!old_val) {
            Field<uint8_t>(inst, 0x30) = 1;  // preRelease = true → god mode
            LOGI("[God] PatcherInfo.preRelease @%p+0x30: %u -> 1", inst, old_val);
        }
    }
    return inst;
}

// ═══════════════════════════════════════════════════════════
//  7. HOOK: DebugTool::RegisterDebugToolEntry — chặn debug (giữ nguyên)
//  Instance method, argc=1 (Action action)
//  dump.cs: public void RegisterDebugToolEntry(System.Action action)
// ═══════════════════════════════════════════════════════════
void (*o_RegisterDebugToolEntry)(void*, void*, void*) = nullptr;
void  h_RegisterDebugToolEntry(void* self, void* action, void* mi) {
    T_COUNT(RegisterDebugToolEntry);
    if (g_nodebug) { LOGI("[DebugTool] RegisterDebugToolEntry blocked"); return; }
    if (o_RegisterDebugToolEntry) o_RegisterDebugToolEntry(self, action, mi);
}

// ═══════════════════════════════════════════════════════════
//  8. HOOK: KCommand::Exec — intercept lệnh game
//  Static method, argc=1
//  dump.cs: public static bool Exec(System.String cmd)
// ═══════════════════════════════════════════════════════════
bool (*o_KExec)(void*, void*) = nullptr;
bool  h_KExec(void* cmd, void* mi) {
    T_COUNT(KExec);
    // Log lệnh để biết game dùng KCommand gì
    // (cmd là Il2CppString: [0x10] = độ dài, [0x14+] = UTF-16)
    if (cmd) {
        int len = *(int*)((uintptr_t)cmd + 0x10);
        if (len > 0 && len < 256) {
            char buf[256] = {};
            uint16_t* wstr = (uint16_t*)((uintptr_t)cmd + 0x14);
            for (int i = 0; i < len && i < 255; i++) buf[i] = (char)wstr[i];
            LOGI("[KCommand] Exec: \"%s\"", buf);
        }
    }
    return o_KExec ? o_KExec(cmd, mi) : false;
}

// KCommand::Exec wrapper để inject lệnh từ GM mode
// (Gọi thẳng o_KExec với Il2CppString tự tạo)
static bool SendCommand(const char* cmd_utf8) {
    if (!il2cpp_string_new || !o_KExec) return false;
    void* s = il2cpp_string_new(cmd_utf8);
    if (!s) return false;
    bool r = o_KExec(s, nullptr);
    LOGI("[KCommand] Inject \"%s\" -> %d", cmd_utf8, r);
    return r;
}

// thread thử lại hook luaL_loadbufferx (libxlua.so nạp trễ sau ResolveAll)
static void* lua_retry_thread(void*) {
    LOGI("[Lua] libxlua chua nap, thu lai moi 500ms (toi da 30s)...");
    for (int i = 0; i < 60 && !o_luaL_loadbufferx; i++) {
        usleep(500000);
        void* sym = FindSymAnywhere("luaL_loadbufferx");
        if (sym) {
            bool ok = DoHook(sym, (void*)h_luaL_loadbufferx, (void**)&o_luaL_loadbufferx, "luaL_loadbufferx");
            if (ok) { LOGI("[Lua] Hook MUON OK (sau ~%ds)", (i+1)/2); g_hook_ok++; }
            else   { LOGE("[Lua] Hook MUON FAIL");                     g_hook_fail++; }
            return nullptr;
        }
    }
    if (!o_luaL_loadbufferx) {
        LOGE("[Lua] Sau 30s van KHONG THAY luaL_loadbufferx — danh sach tat ca lib:");
        dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void*) -> int {
            if (info->dlpi_name && *info->dlpi_name) LOGI("[lib] %s", info->dlpi_name);
            return 0;
        }, nullptr);
    }
    return nullptr;
}

// ═══════════════════════════════════════════════════════════
//  9. ResolveAll
// ═══════════════════════════════════════════════════════════
static void ResolveAll() {
    LOGI("=== Resolve START ===");

    // [0/1/2] Lua dump + damage — tìm luaL_loadbufferx trong tất cả thư viện
    {
        void* lua_sym = FindSymAnywhere(OBFUSCATE("luaL_loadbufferx"));
        if (lua_sym) {
            bool ok = DoHook(lua_sym, (void*)h_luaL_loadbufferx, (void**)&o_luaL_loadbufferx, "luaL_loadbufferx");
            if (ok) { g_hook_ok++; } else { g_hook_fail++; }
        } else {
            LOGI("[Lua] Chua thay luaL_loadbufferx — lib 'lua' dang nap:");
            dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void*) -> int {
                if (info->dlpi_name && (strstr(info->dlpi_name, "lua") || strstr(info->dlpi_name, "xlua")))
                    LOGI("[Lua?] %s", info->dlpi_name);
                return 0;
            }, nullptr);
            pthread_t pt; pthread_create(&pt, nullptr, lua_retry_thread, nullptr); pthread_detach(pt);
            LOGI("[Lua] Spawn retry thread OK");
        }
    }

    // [3] God Mode — PatcherInfo::Get (static, 0 params)
    // dump.cs: public static PatcherInfo Get(); — no namespace
    T_HOOK("", "PatcherInfo", "Get", 0, PatcherInfo_Get);

    // Debug/GM hooks
    T_HOOK("", "DebugTool", "RegisterDebugToolEntry", 1, RegisterDebugToolEntry);
    T_HOOK("", "KCommand",  "Exec", 1, KExec);

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
        OBFUSCATE("1_SeekBar_He So Sat Thuong_1_20"),
        OBFUSCATE("2_Toggle_Bat Tu (God Mode)"),
        OBFUSCATE("3_Toggle_GM Mode"),
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
    switch (featNum) {
        case 0:
            g_dmg = boolean;
            LOGI("Damage Mod: %s (x%d)", g_dmg ? "BAT" : "TAT", g_dmgmul);
            break;
        case 1:
            g_dmgmul = (value < 1) ? 1 : (value > 20 ? 20 : value);
            LOGI("Damage Multiplier: x%d", g_dmgmul);
            break;
        case 2:
            g_god = boolean;
            LOGI("God Mode: %s", g_god ? "BAT" : "TAT");
            break;
        case 3:
            g_gm = boolean;
            LOGI("GM Mode: %s", g_gm ? "BAT" : "TAT");
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
