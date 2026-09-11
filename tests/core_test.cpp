// license:BSD-3-Clause
/* Moved here from tools/dcs_extract/ (2026-09-02) when that duplicate of the vendored
 * DCS core was removed: MarsMiner bundles the core, so MarsMiner is where it gets tested.
 * Content unchanged apart from this note. */
/*
 * core_test.cpp — milestone-1 proof that the vendored ADSP-2105 core runs standalone: build it,
 * construct + reset the device, execute one hand-decoded instruction and read back the register.
 *
 * The instruction is the ADSP-218x "load non-data register immediate (group 0)" form
 *   001100xx  <imm14>  <reg4>      (opcode top byte 0x30..0x33)
 * whose decode (adsp2105.cpp) is  write_reg0(BIT(op,0,4), sext(op>>4,14)).
 * reg-group-0 index 0x0a is AR (ctor: m_read0_ptr[0x0a] = &m_core.ar). So loading AR with 0x1234:
 *   op = 0x30<<16 | (0x1234<<4) | 0x0a = 0x31234A
 */
/* Include ORDER matters and must not be sorted: emu_shim.h stands in for MAME's device
   framework — it defines LSB_FIRST (which decides the SR0/SR1 sub-word order inside the
   register unions) and the u16/offs_t/device types that adsp2105.h uses without declaring.
   The vendored MAME header is not self-contained, unlike the engine's own headers. */
// clang-format off
#include "emu_shim.h"
#include "adsp2105.h"
// clang-format on

#include <cstdio>

/* ---- backing memory + the callback seam the shim forwards to ---- */
static uint32_t g_prog[0x4000];  // 24-bit program words
static uint16_t g_data[0x4000];  // 16-bit data words

u16 dcs_data_read(offs_t a) {
    return g_data[a & 0x3fff];
}
void dcs_data_write(offs_t a, u16 d) {
    g_data[a & 0x3fff] = d;
}
u16 dcs_io_read(offs_t) {
    return 0;
}
void dcs_io_write(offs_t, u16) {}
u32 dcs_program_read(offs_t a) {
    return g_prog[a & 0x3fff];
}
void dcs_program_write(offs_t a, u32 d) {
    g_prog[a & 0x3fff] = d;
}
void dcs_sport_tx(int, u32) {}
u32 dcs_sport_rx(int) {
    return 0;
}
void dcs_timer_fired(int) {}
void dcs_dmovlay(u32) {}

/* expose the protected drive points + observe AR */
struct TestCpu : public adsp2105_device {
    TestCpu() : adsp2105_device(*(machine_config *)nullptr, "test", nullptr, 10000000) {}
    void reset() {
        device_reset();
    }
    void set_pc(uint32_t p) {
        m_pc = p;
    }
    void run(int n) {
        m_icount = n;
        execute_run();
    }
    uint16_t ar() const {
        return m_core.ar.u;
    }
    uint32_t pc() const {
        return m_pc;
    }
};

int main() {
    int fails = 0;
    memset(g_prog, 0, sizeof g_prog);
    memset(g_data, 0, sizeof g_data);

    g_prog[0] = 0x31234A;  // AR = 0x1234

    TestCpu cpu;
    cpu.reset();
    cpu.set_pc(0);
    cpu.run(1);

    printf("%-48s [%s]\n", "one instruction executed (PC advanced)", cpu.pc() != 0 ? "OK" : "FAIL");
    if (cpu.pc() == 0)
        fails++;

    printf("%-48s got 0x%04x [%s]\n", "AR loaded with immediate 0x1234", cpu.ar(),
           cpu.ar() == 0x1234 ? "OK" : "FAIL");
    if (cpu.ar() != 0x1234)
        fails++;

    printf("\n%s\n", fails == 0 ? "ADSP-2105 CORE OK" : "FAILURES");
    return fails ? 1 : 0;
}
