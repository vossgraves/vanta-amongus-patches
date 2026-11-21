// language: C++, file: native/tests/relocate_test.cpp
// Host-side check of the trampoline relocator: every displaced prologue instruction
// must still resolve to the address it named at its original site.
//
//     clang++ -std=c++17 -O1 -o /tmp/relocate_test native/tests/relocate_test.cpp && /tmp/relocate_test
//
// The instruction words are the real first four instructions of the hooked methods in
// libil2cpp.so 2026.11.3 (virtual addresses are file-relative here, which does not matter:
// only deltas are exercised).

#include <cstdint>
#include <cstdio>
#include <cstring>

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



// Independent decoder: computes the absolute target an instruction refers to.
uint64_t effective(uint32_t insn, uint64_t pc, bool* known) {
    *known = true;
    uint64_t immlo, immhi, imm;
    if ((insn & 0x9F000000u) == 0x90000000u || (insn & 0x9F000000u) == 0x10000000u) {
        int64_t v = sign_extend((((insn >> 5) & 0x7FFFFu) << 2) | ((insn >> 29) & 0x3u), 21);
        if ((insn & 0x9F000000u) == 0x90000000u) {
            return (pc & ~0xFFFull) + static_cast<uint64_t>(v << 12);
        }
        return pc + static_cast<uint64_t>(v);
    }
    if ((insn & 0x3B000000u) == 0x18000000u) {
        int64_t v = sign_extend((insn >> 5) & 0x7FFFFu, 19);
        return pc + static_cast<uint64_t>(v << 2);
    }
    if ((insn & 0x7C000000u) == 0x14000000u) {
        int64_t v = sign_extend(insn & 0x03FFFFFFu, 26);
        return pc + static_cast<uint64_t>(v << 2);
    }
    *known = false;
    return 0;
}

int failures = 0;

void check(const char* what, uint32_t insn, uint64_t src, uint64_t dst) {
    uint32_t out = 0;
    bool ok = relocate(insn, src, dst, &out);
    bool known_src = false, known_dst = false;
    uint64_t want = effective(insn, src, &known_src);
    if (!ok) { printf("FAIL  %s: refused at delta %lld\n", what, (long long)(dst - src)); failures++; return; }
    if (!known_src) { printf("pass  %s: copied unchanged 0x%08X\n", what, out); return; }
    uint64_t got = effective(out, dst, &known_dst);
    if (!known_dst || got != want) {
        printf("FAIL  %s: want %llx got %llx (in 0x%08X out 0x%08X)\n", what,
               (unsigned long long)want, (unsigned long long)got, insn, out);
        failures++;
        return;
    }
    printf("pass  %s: %llx -> %llx\n", what, (unsigned long long)want, (unsigned long long)got);
}

int main() {
    // Real prologues from libil2cpp 2026.11.3 (vaddr base 0x21D1DE0 etc.).
    struct { const char* name; uint64_t pc; uint32_t insns[4]; } cases[] = {
        {"SetKillTimer", 0x21D1DE0, {0x6dbd23e9u, 0xf9000bfeu, 0xa9024ff4u, 0xd00159b4u}},
        {"CmdCheckMurder", 0x21DE51Cu, {0xf81d0ffeu, 0xa90157f6u, 0xa9024ff4u, 0xb0015956u}},
        {"IsFreechat", 0x20A78F4, {0xa9be57feu, 0xa9014ff4u, 0x90016315u, 0xf0014654u}},
        {"PhysicsLate", 0x21E5468, {0x6dbe23e9u, 0xa9014ffeu, 0xaa1f03e1u, 0xaa0003f3u}},
    };

    const long long deltas[] = {-0x1000, -0x40000, 0x1000, 0x40000, 0x7F0000, -0x7F0000};
    for (auto& c : cases) {
        for (long long d : deltas) {
            char label[128];
            for (int i = 0; i < 4; ++i) {
                snprintf(label, sizeof(label), "%s d=%lld i=%d", c.name, d, i);
                check(label, c.insns[i], c.pc + i * 4,
                      static_cast<uint64_t>(static_cast<long long>(c.pc) + i * 4 + d));
            }
        }
    }

    printf(failures ? "\n%d failure(s)\n" : "\nall relocated instructions land on their original target\n", failures);
    return failures ? 1 : 0;
}
