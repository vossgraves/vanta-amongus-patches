// language: C++, file: native/tests/hook_test.cpp
// End-to-end check of the inline hook engine on the host (aarch64 Linux).
//
//     clang++ -std=c++17 -O1 -o /tmp/hook_test native/tests/hook_test.cpp -ldl && /tmp/hook_test
//
// The engine itself is included, so this exercises the real write_branch, relocate, map_near
// and trampoline code — not a copy. Two functions are synthesised in anonymous memory:
//
//   fn_a: adrp x0, page ; ldr x0, [x0, #0x10] ; add x0, x0, #1 ; ret
//   fn_b: adrp x0, page ; ldr x0, [x0, #0x18] ; add x0, x0, #1 ; add x0, x0, #2 ; ret
//
// fn_a returns from the displaced `ret`; fn_b runs past the displaced window, so it leaves
// through the trampoline's back-branch into target+16. Both must return the same value the
// unhooked function would.

#include "../src/vanta.cpp"

#include <cstdio>

namespace {

struct Fake {
    uint8_t* code;
    uint64_t* data;
    size_t code_bytes;
};

void emit(uint32_t* at, uint32_t insn) { *at = insn; }

uint32_t adrp(int rd, uintptr_t pc, uintptr_t target) {
    int64_t pages = (static_cast<int64_t>(target & ~uintptr_t(0xFFF)) -
                     static_cast<int64_t>(pc & ~uintptr_t(0xFFF))) >> 12;
    uint32_t imm = static_cast<uint32_t>(pages) & 0x1FFFFFu;
    return 0x90000000u | ((imm & 0x3u) << 29) | ((rd & 0x1Fu)) | ((imm >> 2) << 5);
}

Fake make_fake(uint64_t value_a, uint64_t value_b, bool long_prologue) {
    const size_t span = page_span(0x1000);
    uint8_t* code = static_cast<uint8_t*>(mmap(nullptr, span, PROT_READ | PROT_WRITE | PROT_EXEC,
                                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    uint8_t* data = static_cast<uint8_t*>(mmap(nullptr, span, PROT_READ | PROT_WRITE,
                                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (code == MAP_FAILED || data == MAP_FAILED) { perror("mmap"); exit(2); }

    reinterpret_cast<uint64_t*>(data)[2] = value_a;   // [data + 0x10]
    reinterpret_cast<uint64_t*>(data)[3] = value_b;   // [data + 0x18]

    uint32_t* p = reinterpret_cast<uint32_t*>(code);
    int i = 0;
    const uintptr_t slot = reinterpret_cast<uintptr_t>(data) + (long_prologue ? 0x18 : 0x10);
    p[i++] = adrp(0, reinterpret_cast<uintptr_t>(code), slot);
    p[i++] = long_prologue ? 0xF9400C00u    // ldr x0, [x0, #0x18]
                           : 0xF9400800u;   // ldr x0, [x0, #0x10]
    p[i++] = 0x91000400u;   // add x0, x0, #1
    if (long_prologue) {
        p[i++] = 0x91000800u;   // add x0, x0, #2
        p[i++] = 0xD65F03C0u;   // ret
    } else {
        p[i++] = 0xD65F03C0u;   // ret
        p[i++] = 0xD65F03C0u;   // padding at target+16
    }
    for (; i < 8; ++i) p[i] = 0xD65F03C0u;

    __builtin___clear_cache(reinterpret_cast<char*>(code),
                            reinterpret_cast<char*>(code) + span);
    return {code, reinterpret_cast<uint64_t*>(data), span};
}

}  // namespace

using fn_t = uint64_t (*)();

static fn_t g_tramp_a = nullptr;
static fn_t g_tramp_b = nullptr;

static uint64_t detour_a() { return g_tramp_a() + 10; }
static uint64_t detour_b() { return g_tramp_b() + 10; }

int failures = 0;

void expect(const char* what, uint64_t got, uint64_t want) {
    if (got != want) {
        printf("FAIL  %s: got 0x%llx want 0x%llx\n", what,
               (unsigned long long)got, (unsigned long long)want);
        ++failures;
    } else {
        printf("pass  %s: 0x%llx\n", what, (unsigned long long)got);
    }
}

int main() {
    // fn_a: displaced window holds the whole function body.
    Fake a = make_fake(0x4242, 0, false);
    g_il2cpp_base = reinterpret_cast<uintptr_t>(a.code);
    g_il2cpp_end = g_il2cpp_base + a.code_bytes;

    fn_t fn_a = reinterpret_cast<fn_t>(a.code);
    expect("fn_a unhooked", fn_a(), 0x4243);

    bool ok = hook(reinterpret_cast<void*>(a.code), reinterpret_cast<void*>(detour_a),
                   reinterpret_cast<void**>(&g_tramp_a));

    if (!ok) {
        printf("FAIL  fn_a hook installed\n");
        ++failures;
    } else {
        expect("fn_a trampoline", g_tramp_a(), 0x4243);
        expect("fn_a hooked", fn_a(), 0x424D);
    }

    // fn_b: the displaced window leaves the body mid-flight, exercising the back-branch.
    Fake b = make_fake(0x100, 0x200, true);
    g_il2cpp_base = reinterpret_cast<uintptr_t>(b.code);
    g_il2cpp_end = g_il2cpp_base + b.code_bytes;

    fn_t fn_b = reinterpret_cast<fn_t>(b.code);
    expect("fn_b unhooked", fn_b(), 0x203);

    if (!hook(reinterpret_cast<void*>(b.code), reinterpret_cast<void*>(detour_b),
              reinterpret_cast<void**>(&g_tramp_b))) {
        printf("FAIL  fn_b hook installed\n");
        ++failures;
    } else {
        expect("fn_b trampoline", g_tramp_b(), 0x203);
        expect("fn_b hooked", fn_b(), 0x20D);
    }

    printf(failures ? "\n%d failure(s)\n" : "\ninline hook engine verified\n", failures);
    return failures ? 1 : 0;
}
