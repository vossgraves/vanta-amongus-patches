// language: C++, file: native/src/vanta.cpp
// libvanta.so — Among Us 2026.11.3 (arm64-v8a) mod menu core.
//
// Loaded by app.vanta.extension.Loader through System.loadLibrary. No STL, no JNI
// headers, no liblog link: the only build inputs are clang and the bionic headers.
//
// Hook model: 16-byte absolute branch into our detour, original four instructions
// relocated to a trampoline page. Every detour is a plain C function, so the ABI
// stays System V AArch64 — no assembly thunks to keep in sync.

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include "offsets.h"

// ---------------------------------------------------------------- JNI minimal

typedef struct JavaVM_ JavaVM;
typedef struct JNIEnv_ JNIEnv;
typedef int jint;
typedef unsigned char jboolean;
typedef void* jclass;
typedef float jfloat;

#define VANTA_EXPORT extern "C" __attribute__((visibility("default")))
#define JNI_CALL __attribute__((used))

// ------------------------------------------------------------------- logging

namespace {

using log_fn = int (*)(int, const char*, const char*, ...);
log_fn g_log = nullptr;

void logger_init() {
    void* h = dlopen("liblog.so", RTLD_NOW);
    if (h) g_log = reinterpret_cast<log_fn>(dlsym(h, "__android_log_print"));
}

void logi(const char* fmt, ...) {
    if (!g_log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log(4 /* ANDROID_LOG_INFO */, "vanta", "%s", buf);
}

// ------------------------------------------------------------------ features

struct Flags {
    volatile int no_kill_cd;
    volatile int kill_anyone;
    volatile int vent_anywhere;
    volatile int unlock_cosmetics;
    volatile int unlock_features;
    volatile int task_win;
    volatile int complete_tasks;
    volatile float speed;
};

// speed 0 means "leave the game's own value alone": writing 1.0 every frame would erase
// role and lobby speed settings even when the slider was never touched.
Flags g_flags = {0, 0, 0, 0, 0, 0, 0, 0.0f};
bool g_ready = false;
int g_version_code = 0;

// -------------------------------------------------------------- hook engine
//
// Overwrite model: the first 16 bytes of the target become
//
//     ldr x16, #8 ; br x16 ; .quad detour
//
// which needs no code cave and clobbers only the intra-procedure scratch register.
// The four displaced instructions move to a per-hook page mapped as close to the target
// as the kernel allows, so their pc-relative forms (adrp, adr, ldr literal, b, bl) still
// resolve once their immediates are rebound to the new page. The trampoline ends with a
// plain `b target+16`, five words wide, which touches no register at all — the prologue's
// own registers survive into the displaced code untouched.

uintptr_t g_il2cpp_base = 0;
uintptr_t g_il2cpp_end = 0;

constexpr size_t kHead = 16;   // displaced bytes
constexpr size_t kStubSpan = 64;

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

void flush(void* addr, size_t len) {
    __builtin___clear_cache(static_cast<char*>(addr), static_cast<char*>(addr) + len);
}

bool make_writeable(void* addr, size_t len) {
    uintptr_t page = reinterpret_cast<uintptr_t>(addr) & ~(uintptr_t)(getpagesize() - 1);
    uintptr_t end = reinterpret_cast<uintptr_t>(addr) + len;
    size_t span = end - page;
    if (mprotect(reinterpret_cast<void*>(page), span,
                 PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
        return true;
    }
    // W^X: drop exec while the bytes are written, restore it afterwards.
    return mprotect(reinterpret_cast<void*>(page), span, PROT_READ | PROT_WRITE) == 0;
}

void restore_exec(void* addr, size_t len) {
    uintptr_t page = reinterpret_cast<uintptr_t>(addr) & ~(uintptr_t)(getpagesize() - 1);
    uintptr_t end = reinterpret_cast<uintptr_t>(addr) + len;
    mprotect(reinterpret_cast<void*>(page), end - page, PROT_READ | PROT_EXEC);
}

// Writes LDR x16, #8 ; BR x16 ; .quad <dest> at `at`, 16 bytes.
void write_branch(uint8_t* at, const void* dest) {
    uint32_t insn[2];
    insn[0] = 0x58000050u;  // ldr x16, #8
    insn[1] = 0xD61F0200u;  // br  x16
    memcpy(at, insn, 8);
    uint64_t target = reinterpret_cast<uint64_t>(dest);
    memcpy(at + 8, &target, 8);
}

// Writes `b <dest>` at `at`, 4 bytes.
void write_b(uint8_t* at, uintptr_t dest) {
    uintptr_t from = reinterpret_cast<uintptr_t>(at);
    int64_t words = (static_cast<int64_t>(dest) - static_cast<int64_t>(from)) >> 2;
    uint32_t insn = 0x14000000u | (static_cast<uint32_t>(words) & 0x03FFFFFFu);
    memcpy(at, &insn, 4);
}

inline int64_t sign_extend(uint64_t value, int bits) {
    const uint64_t sign = 1ull << (bits - 1);
    return static_cast<int64_t>((value ^ sign) - sign);
}

inline bool is_b(uint32_t insn)    { return (insn & 0x7C000000u) == 0x14000000u; }
inline bool is_adr(uint32_t insn)  { return (insn & 0x9F000000u) == 0x10000000u; }
inline bool is_adrp(uint32_t insn) { return (insn & 0x9F000000u) == 0x90000000u; }
inline bool is_ldr_lit(uint32_t insn) { return (insn & 0x3B000000u) == 0x18000000u; }

// Rebinds one displaced instruction from `src` to `dst`. False for forms this engine
// does not rewrite (cbz/tbz and friends), so the caller refuses the hook instead of
// installing a trampoline that would jump into the weeds.
bool relocate(uint32_t insn, uintptr_t src, uintptr_t dst, uint32_t* out) {
    const int64_t delta = static_cast<int64_t>(dst) - static_cast<int64_t>(src);

    if (is_adrp(insn)) {
        int64_t imm = sign_extend((((insn >> 5) & 0x7FFFFu) << 2) | ((insn >> 29) & 0x3u), 21);
        imm <<= 12;
        int64_t page_delta = static_cast<int64_t>(dst & ~uintptr_t(0xFFF)) -
                             static_cast<int64_t>(src & ~uintptr_t(0xFFF));
        int64_t moved = imm - page_delta;
        if (moved & 0xFFF) return false;
        int64_t field = moved >> 12;
        if (field < -(1ll << 20) || field >= (1ll << 20)) return false;
        uint32_t bits = static_cast<uint32_t>(field) & 0x1FFFFFu;
        *out = (insn & ~0x60FFFFE0u) | ((bits & 0x3u) << 29) | ((bits >> 2) << 5);
        return true;
    }

    if (is_adr(insn)) {
        int64_t imm = sign_extend((((insn >> 5) & 0x7FFFFu) << 2) | ((insn >> 29) & 0x3u), 21);
        int64_t moved = imm - delta;
        if (moved < -(1ll << 20) || moved >= (1ll << 20)) return false;
        uint32_t bits = static_cast<uint32_t>(moved) & 0x1FFFFFu;
        *out = (insn & ~0x60FFFFE0u) | ((bits & 0x3u) << 29) | ((bits >> 2) << 5);
        return true;
    }

    if (is_ldr_lit(insn)) {
        int64_t words = sign_extend((insn >> 5) & 0x7FFFFu, 19);
        int64_t moved = (words << 2) - delta;
        if (moved & 0x3) return false;
        int64_t field = moved >> 2;
        if (field < -(1ll << 18) || field >= (1ll << 18)) return false;
        *out = (insn & ~0x00FFFFE0u) | ((static_cast<uint32_t>(field) & 0x7FFFFu) << 5);
        return true;
    }

    if (is_b(insn)) {
        int64_t words = sign_extend(insn & 0x03FFFFFFu, 26);
        int64_t dest = static_cast<int64_t>(src) + (words << 2);
        int64_t moved = dest - static_cast<int64_t>(dst);
        if (moved & 0x3) return false;
        int64_t field = moved >> 2;
        if (field < -(1ll << 25) || field >= (1ll << 25)) return false;
        *out = (insn & ~0x03FFFFFFu) | (static_cast<uint32_t>(field) & 0x03FFFFFFu);
        return true;
    }

    if ((insn & 0x3E000000u) == 0x34000000u ||   // cbz / cbnz
        (insn & 0x3E000000u) == 0x36000000u) {   // tbz / tbnz
        return false;
    }

    *out = insn;   // position independent
    return true;
}

size_t page_span(size_t size) {
    const size_t page = static_cast<size_t>(getpagesize());
    return (size + page - 1) / page * page;
}

// Maps rw memory within a megabyte of `target`, then anywhere if the probes all lose.
void* map_near(uintptr_t target, size_t size) {
    const size_t page = static_cast<size_t>(getpagesize());
    const size_t span = page_span(size);
    const uintptr_t base = target & ~(uintptr_t)(page - 1);

    for (uintptr_t step = page; step <= (1u << 20); step += page) {
        for (int dir = 0; dir < 2; ++dir) {
            const uintptr_t want = dir ? base - step : base + step;
            if (want < page) continue;
            void* p = mmap(reinterpret_cast<void*>(want), span, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p == MAP_FAILED) continue;
            if (p == reinterpret_cast<void*>(want)) return p;
            munmap(p, span);   // kernel treated the address as a hint
        }
    }

    void* p = mmap(nullptr, span, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

// Hooks `target` and returns the callable trampoline in `out_orig`.
bool hook(void* target, void* detour, void** out_orig) {
    if (!target || !detour) return false;

    const uintptr_t t = reinterpret_cast<uintptr_t>(target);
    if (t < g_il2cpp_base || t + kHead > g_il2cpp_end) {
        logi("hook target outside libil2cpp: %p", target);
        return false;
    }

    uint32_t head[4];
    memcpy(head, target, sizeof(head));

    uint8_t* stub = static_cast<uint8_t*>(map_near(t, kStubSpan));
    if (!stub) {
        logi("no page near %p", target);
        return false;
    }
    const size_t span = page_span(kStubSpan);

    uint32_t moved[4];
    for (int i = 0; i < 4; ++i) {
        const uintptr_t src = t + i * 4;
        const uintptr_t dst = reinterpret_cast<uintptr_t>(stub) + i * 4;
        if (!relocate(head[i], src, dst, &moved[i])) {
            logi("cannot relocate 0x%08X at %p", head[i], reinterpret_cast<void*>(src));
            munmap(stub, span);
            return false;
        }
    }
    memcpy(stub, moved, kHead);

    const int64_t back = static_cast<int64_t>(t + kHead) -
                         static_cast<int64_t>(reinterpret_cast<uintptr_t>(stub) + kHead);
    if ((back & 0x3) || back < -(1ll << 27) || back >= (1ll << 27)) {
        logi("trampoline too far from %p", target);
        munmap(stub, span);
        return false;
    }
    write_b(stub + kHead, t + kHead);

    if (mprotect(stub, span, PROT_READ | PROT_EXEC) != 0) {
        logi("stub mprotect failed");
        munmap(stub, span);
        return false;
    }
    flush(stub, kStubSpan);

    if (!make_writeable(target, kHead)) {
        logi("mprotect failed at %p", target);
        munmap(stub, span);
        return false;
    }
    write_branch(static_cast<uint8_t*>(target), detour);
    flush(target, kHead);
    restore_exec(target, kHead);

    if (out_orig) *out_orig = stub;
    return true;
}

// ----------------------------------------------------------- il2cpp plumbing

int module_cb(struct dl_phdr_info* info, size_t, void* data) {
    const char* want = static_cast<const char*>(data);
    const char* name = info->dlpi_name ? info->dlpi_name : "";
    if (!name || !strstr(name, want)) return 0;

    uintptr_t lo = ~uintptr_t(0), hi = 0;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD) continue;
        uintptr_t s = info->dlpi_addr + ph.p_vaddr;
        uintptr_t e = s + ph.p_memsz;
        if (s < lo) lo = s;
        if (e > hi) hi = e;
    }
    g_il2cpp_base = lo;
    g_il2cpp_end = hi;
    return 1;
}

bool find_il2cpp() {
    dl_iterate_phdr(module_cb, const_cast<char*>("libil2cpp.so"));
    if (!g_il2cpp_base) {
        logi("libil2cpp.so not mapped yet");
        return false;
    }
    logi("libil2cpp %p-%p", reinterpret_cast<void*>(g_il2cpp_base),
         reinterpret_cast<void*>(g_il2cpp_end));
    return true;
}

inline void* at(uint32_t rva) {
    return reinterpret_cast<void*>(g_il2cpp_base + rva);
}

inline void* field(void* obj, uint32_t offset) {
    if (!obj) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(obj) + offset);
}

// Il2Cpp object header is 16 bytes; List<T> body starts at 0x10.
struct ManagedList {
    void* klass;
    void* monitor;
    void* items;    // T[]
    int32_t size;
    int32_t version;
};

struct ManagedArray {
    void* klass;
    void* monitor;
    uint64_t bounds;
    int32_t max_length;
    // elements follow at 0x20
};

// ------------------------------------------------------------------- targets

bool (*AmOwner)(void*) = nullptr;
void (*SetKillTimer)(void*, float) = nullptr;
void (*CmdCheckMurder)(void*, void*) = nullptr;
void (*RpcMurderPlayer)(void*, void*, bool) = nullptr;
void (*RpcCompleteTask)(void*, uint32_t) = nullptr;
float (*VentCanUse)(void*, void*, bool*, bool*) = nullptr;
bool (*GetPurchase)(void*, void*, void*) = nullptr;
bool (*IsFreechatAllowed)(void*) = nullptr;
bool (*IsFriendsListAllowed)(void*) = nullptr;
void (*CanSetCustomName)(void*, bool) = nullptr;
bool (*CanPlayOnline)(void*) = nullptr;
void (*IsAllowedOnline)(void*, bool) = nullptr;
bool (*IsMinorOrWaiting)(void*) = nullptr;
bool (*CheckTaskCompletion)(void*) = nullptr;
void (*PlayerFixedUpdate)(void*) = nullptr;
void (*PlayerPhysicsLateUpdate)(void*) = nullptr;

// Trampolines. Every detour calls the trampoline, never the target pointer: the target's
// first bytes are the branch into the detour, so calling it would re-enter this file.
void (*t_setkill)(void*, float) = nullptr;
void (*t_cmdcheck)(void*, void*) = nullptr;
float (*t_canuse)(void*, void*, bool*, bool*) = nullptr;
void (*t_fixedupdate)(void*) = nullptr;
void (*t_physicslate)(void*) = nullptr;
bool (*t_getpurchase)(void*, void*, void*) = nullptr;
bool (*t_freechat)(void*) = nullptr;
bool (*t_friends)(void*) = nullptr;
void (*t_customname)(void*, bool) = nullptr;
bool (*t_canplayonline)(void*) = nullptr;
bool (*t_taskwin)(void*) = nullptr;
void (*t_allowedonline)(void*, bool) = nullptr;
bool (*t_minorwaiting)(void*) = nullptr;

// ------------------------------------------------------------------- detours

void detour_setkill(void* self, float time) {
    if (g_flags.no_kill_cd && AmOwner && AmOwner(self)) time = 0.0f;
    t_setkill(self, time);
}

void detour_cmdcheck(void* self, void* target) {
    if (g_flags.kill_anyone && target && RpcMurderPlayer && AmOwner && AmOwner(self)) {
        RpcMurderPlayer(self, target, true);
        return;
    }
    t_cmdcheck(self, target);
}

float detour_canuse(void* self, void* pc, bool* can_use, bool* could_use) {
    float dist = t_canuse(self, pc, can_use, could_use);
    if (g_flags.vent_anywhere) {
        if (can_use) *can_use = true;
        if (could_use) *could_use = true;
    }
    return dist;
}

bool detour_getpurchase(void* self, void* item, void* bundle) {
    if (g_flags.unlock_cosmetics) return true;
    return t_getpurchase(self, item, bundle);
}

bool detour_freechat(void* self) {
    if (g_flags.unlock_features) return true;
    return t_freechat(self);
}

bool detour_friends(void* self) {
    if (g_flags.unlock_features) return true;
    return t_friends(self);
}

void detour_customname(void* self, bool allowed) {
    t_customname(self, g_flags.unlock_features ? true : allowed);
}

bool detour_canplayonline(void* self) {
    if (g_flags.unlock_features) return true;
    return t_canplayonline(self);
}

void detour_allowedonline(void* self, bool can_online) {
    t_allowedonline(self, g_flags.unlock_features ? true : can_online);
}

bool detour_minorwaiting(void* self) {
    bool waiting = t_minorwaiting(self);
    return g_flags.unlock_features ? false : waiting;
}

bool detour_taskwin(void* self) {
    bool done = t_taskwin(self);
    return g_flags.task_win ? true : done;
}

void complete_local_tasks(void* player) {
    auto* list = static_cast<ManagedList*>(field(player, vanta::off::PlayerControl_myTasks));
    if (!list || !list->items || list->size <= 0 || list->size > 64) return;

    auto* arr = static_cast<ManagedArray*>(list->items);
    auto* tasks = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) +
                                           vanta::off::Array_data);
    int n = list->size;
    if (arr->max_length < n) n = arr->max_length;

    for (int i = 0; i < n; ++i) {
        void* task = tasks[i];
        if (!task) continue;
        uint32_t id = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(task) +
                                                   vanta::off::PlayerTask_Id);
        if (RpcCompleteTask) RpcCompleteTask(player, id);
    }
}

// Speed lands after the game's own movement code has run for this frame, so the value
// survives to the next physics step instead of being overwritten by role/option logic.
void detour_physics_lateupdate(void* self) {
    t_physicslate(self);

    if (!g_ready || !self || !AmOwner || !AmOwner(self)) return;
    if (g_flags.speed <= 0.0f) return;

    float* speed = reinterpret_cast<float*>(
        reinterpret_cast<uint8_t*>(self) + vanta::off::PlayerPhysics_Speed);
    float* ghost = reinterpret_cast<float*>(
        reinterpret_cast<uint8_t*>(self) + vanta::off::PlayerPhysics_GhostSpeed);
    *speed = g_flags.speed;
    *ghost = g_flags.speed;
}

void detour_player_fixedupdate(void* self) {
    t_fixedupdate(self);

    if (!g_ready || !self || !AmOwner || !AmOwner(self)) return;

    if (g_flags.complete_tasks) {
        complete_local_tasks(self);
        g_flags.complete_tasks = 0;
    }
}

// ------------------------------------------------------------------ install

bool resolve_targets() {
    AmOwner = reinterpret_cast<bool (*)(void*)>(at(vanta::off::InnerNetObject_get_AmOwner));
    SetKillTimer = reinterpret_cast<void (*)(void*, float)>(at(vanta::off::PlayerControl_SetKillTimer));
    CmdCheckMurder = reinterpret_cast<void (*)(void*, void*)>(at(vanta::off::PlayerControl_CmdCheckMurder));
    RpcMurderPlayer = reinterpret_cast<void (*)(void*, void*, bool)>(at(vanta::off::PlayerControl_RpcMurderPlayer));
    RpcCompleteTask = reinterpret_cast<void (*)(void*, uint32_t)>(at(vanta::off::PlayerControl_RpcCompleteTask));
    VentCanUse = reinterpret_cast<float (*)(void*, void*, bool*, bool*)>(at(vanta::off::Vent_CanUse));
    GetPurchase = reinterpret_cast<bool (*)(void*, void*, void*)>(at(vanta::off::PlayerPurchasesData_GetPurchase));
    IsFreechatAllowed = reinterpret_cast<bool (*)(void*)>(at(vanta::off::EOSManager_IsFreechatAllowed));
    IsFriendsListAllowed = reinterpret_cast<bool (*)(void*)>(at(vanta::off::EOSManager_IsFriendsListAllowed));
    CanSetCustomName = reinterpret_cast<void (*)(void*, bool)>(at(vanta::off::FullAccount_CanSetCustomName));
    CanPlayOnline = reinterpret_cast<bool (*)(void*)>(at(vanta::off::AccountManager_CanPlayOnline));
    IsAllowedOnline = reinterpret_cast<void (*)(void*, bool)>(at(vanta::off::EOSManager_IsAllowedOnline));
    IsMinorOrWaiting = reinterpret_cast<bool (*)(void*)>(at(vanta::off::EOSManager_IsMinorOrWaiting));
    CheckTaskCompletion = reinterpret_cast<bool (*)(void*)>(at(vanta::off::GameManager_CheckTaskCompletion));
    PlayerFixedUpdate = reinterpret_cast<void (*)(void*)>(at(vanta::off::PlayerControl_FixedUpdate));
    PlayerPhysicsLateUpdate = reinterpret_cast<void (*)(void*)>(at(vanta::off::PlayerPhysics_LateUpdate));
    return true;
}

void install() {
    if (g_ready) return;
    if (!find_il2cpp()) return;

    if (g_version_code != vanta::off::kVersionCode) {
        logi("version %d does not match table %d, hooks skipped",
             g_version_code, vanta::off::kVersionCode);
        return;
    }

    resolve_targets();

    int ok = 0;
    ok += hook(reinterpret_cast<void*>(SetKillTimer), reinterpret_cast<void*>(detour_setkill),
               reinterpret_cast<void**>(&t_setkill));
    ok += hook(reinterpret_cast<void*>(CmdCheckMurder), reinterpret_cast<void*>(detour_cmdcheck),
               reinterpret_cast<void**>(&t_cmdcheck));
    ok += hook(reinterpret_cast<void*>(VentCanUse), reinterpret_cast<void*>(detour_canuse),
               reinterpret_cast<void**>(&t_canuse));
    ok += hook(reinterpret_cast<void*>(PlayerFixedUpdate), reinterpret_cast<void*>(detour_player_fixedupdate),
               reinterpret_cast<void**>(&t_fixedupdate));
    ok += hook(reinterpret_cast<void*>(PlayerPhysicsLateUpdate),
               reinterpret_cast<void*>(detour_physics_lateupdate),
               reinterpret_cast<void**>(&t_physicslate));

    ok += hook(reinterpret_cast<void*>(GetPurchase), reinterpret_cast<void*>(detour_getpurchase),
               reinterpret_cast<void**>(&t_getpurchase));
    ok += hook(reinterpret_cast<void*>(IsFreechatAllowed), reinterpret_cast<void*>(detour_freechat),
               reinterpret_cast<void**>(&t_freechat));
    ok += hook(reinterpret_cast<void*>(IsFriendsListAllowed), reinterpret_cast<void*>(detour_friends),
               reinterpret_cast<void**>(&t_friends));
    ok += hook(reinterpret_cast<void*>(CanSetCustomName), reinterpret_cast<void*>(detour_customname),
               reinterpret_cast<void**>(&t_customname));
    ok += hook(reinterpret_cast<void*>(CanPlayOnline), reinterpret_cast<void*>(detour_canplayonline),
               reinterpret_cast<void**>(&t_canplayonline));
    ok += hook(reinterpret_cast<void*>(CheckTaskCompletion), reinterpret_cast<void*>(detour_taskwin),
               reinterpret_cast<void**>(&t_taskwin));
    ok += hook(reinterpret_cast<void*>(IsAllowedOnline), reinterpret_cast<void*>(detour_allowedonline),
               reinterpret_cast<void**>(&t_allowedonline));
    ok += hook(reinterpret_cast<void*>(IsMinorOrWaiting), reinterpret_cast<void*>(detour_minorwaiting),
               reinterpret_cast<void**>(&t_minorwaiting));

    g_ready = true;
    logi("installed %d hooks for %s", ok, vanta::off::kVersionName);
}

}  // namespace

// ------------------------------------------------------------------- exports

VANTA_EXPORT jint JNI_OnLoad(JavaVM*, void*) {
    logger_init();
    logi("libvanta loaded");
    return 0x00010006;  // JNI_VERSION_1_6
}

VANTA_EXPORT JNI_CALL void Java_app_vanta_extension_Menu_nativeStart(JNIEnv*, jclass,
                                                                    jint version_code) {
    g_version_code = version_code;
    install();
}

VANTA_EXPORT JNI_CALL void Java_app_vanta_extension_Menu_setFlag(JNIEnv*, jclass,
                                                                 jint id, jboolean on) {
    const int v = on ? 1 : 0;
    switch (id) {
        case 0: g_flags.no_kill_cd = v; break;
        case 1: g_flags.kill_anyone = v; break;
        case 2: g_flags.vent_anywhere = v; break;
        case 3: g_flags.unlock_cosmetics = v; break;
        case 4: g_flags.unlock_features = v; break;
        case 5: g_flags.task_win = v; break;
        case 6: if (v) g_flags.complete_tasks = 1; break;
        default: break;
    }
}

VANTA_EXPORT JNI_CALL void Java_app_vanta_extension_Menu_setSpeed(JNIEnv*, jclass, jfloat value) {
    if (value <= 0.0f) {          // dial back to the game's own speed
        g_flags.speed = 0.0f;
        return;
    }
    if (value < 0.5f) value = 0.5f;
    if (value > 12.0f) value = 12.0f;
    g_flags.speed = value;
}

// 0 = still waiting for libil2cpp, 1 = hooks installed, 2 = this build is not in the table.
VANTA_EXPORT JNI_CALL jint Java_app_vanta_extension_Menu_nativeStatus(JNIEnv*, jclass) {
    if (g_ready) return 1;
    if (g_version_code != 0 && g_version_code != vanta::off::kVersionCode) return 2;
    return 0;
}
