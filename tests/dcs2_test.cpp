// license:BSD-3-Clause
/*
 * dcs2_test.cpp — milestone-2 proof: boot the real DCS-2 board from the original ROMs and confirm
 * the DSP reaches its documented post-boot state. We drive the vendored ADSP-2105 through the P2K
 * harness (SDRC map, 28F800 flash boot) and check that the self-boot converges exactly where the
 * reference emulator's host-reset waits for it:
 *   - PC enters the resident-engine idle band [0x3D9C, 0x3DC1] (engine @0x3980);
 *   - PM[0x3980] != 0 and PM[0x3DEB] != 0 (the engine is paged into program SRAM) — exactly the
 *     reference host-reset's convergence test.
 * That validates the flash boot, the SDRC memory map and the CPU core end-to-end. Turning the
 * resident self-test/control engine into the streaming player needs the host's ACE1 init
 * sequence, which dcs2_p2k.cpp performs.
 */
#include "dcs2_p2k.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

/* harness bring-up introspection (dcs2_p2k.cpp) */
uint32_t dcs2_dbg_pc(void);
int dcs2_dbg_run(int cycles);
uint32_t dcs2_dbg_pm(uint16_t addr);

static std::vector<uint8_t> load(const char *path) {
    std::vector<uint8_t> v;
    FILE *f = fopen(path, "rb");
    if (!f)
        return v;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    v.resize(n);
    if (fread(v.data(), 1, n, f) != (size_t)n)
        v.clear();
    fclose(f);
    return v;
}

int main(int argc, char **argv) {
    const char *roms = argc > 1 ? argv[1] : "roms";
    auto u109 = load((std::string(roms) + "/chips/rfm_u109.bin").c_str());
    auto u110 = load((std::string(roms) + "/chips/rfm_u110.bin").c_str());
    auto flash = load((std::string(roms) + "/update_0180/pin2000_50070_0180_sf.rom").c_str());
    if (u109.empty() || u110.empty() || flash.empty()) {
        printf("[skip] ROMs not found under %s — pass your ROM directory as argv[1]\n", roms);
        return 0;
    }

    int fails = 0;
    bool ok = dcs2_prepare(u109.data(), u109.size(), u110.data(), u110.size(), flash.data(),
                           flash.size());
    printf("%-52s [%s]\n", "prepare(): assets loaded + DSP booted", ok ? "OK" : "FAIL");
    if (!ok) {
        printf("\nFAILURES\n");
        return 1;
    }

    /* run the self-boot until it reaches the resident-engine idle band */
    int reached = 0;
    for (int i = 0; i < 200000 && !reached; i++) {
        dcs2_dbg_run(20);
        uint32_t pc = dcs2_dbg_pc();
        if (pc >= 0x3d9c && pc <= 0x3dc1)
            reached = 1;
    }
    printf("%-52s [%s]\n", "self-boot reaches engine idle band 0x3D9C..", reached ? "OK" : "FAIL");
    if (!reached)
        fails++;

    uint32_t pm3980 = dcs2_dbg_pm(0x3980), pm3deb = dcs2_dbg_pm(0x3deb);
    printf("%-52s PM[3980]=%06x [%s]\n", "engine paged into program SRAM (PM[3980]!=0)", pm3980,
           pm3980 != 0 ? "OK" : "FAIL");
    if (pm3980 == 0)
        fails++;
    printf("%-52s PM[3DEB]=%06x [%s]\n", "engine data word populated (PM[3DEB]!=0)", pm3deb,
           pm3deb != 0 ? "OK" : "FAIL");
    if (pm3deb == 0)
        fails++;

    printf("\n%s\n", fails == 0 ? "DCS-2 HARNESS OK (boot verified)" : "FAILURES");
    return fails ? 1 : 0;
}
