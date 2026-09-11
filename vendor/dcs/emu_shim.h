// license:BSD-3-Clause
/*
 * emu_shim.h — a minimal stand-in for the slice of MAME's device framework that the vendored
 * ADSP-2100/2105 core (adsp2105.cpp/.h + 2100ops.hxx, BSD-3-Clause, Aaron Giles) actually touches.
 *
 * The point of the shim, rather than a hand-rewrite, is that MAME's CPU LOGIC compiles here 1:1 —
 * we only replace the surrounding scaffolding: address spaces become callback dispatch, devcb
 * callbacks become plain global function pointers (there is exactly one DSP instance in this offline
 * tool), and the debugger / save-state / state-registration surface becomes no-ops. Nothing about
 * the instruction semantics is reimplemented.
 *
 * The harness (dcs2_p2k.cpp) provides the six memory globals + four callback globals below.
 */
#ifndef DCS_EXTRACT_EMU_SHIM_H
#define DCS_EXTRACT_EMU_SHIM_H

#include <cstdint>
#include <cstring>
#include <cstddef>

/* The ADSP register unions (SR0/SR1, MR0/MR1/MR2) order their sub-words by host endianness via
   LSB_FIRST — MAME's build defines it on little-endian hosts. Standalone we must set it ourselves,
   or SR0/SR1 come out swapped and every shift-assembled value loses its byte order. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define LSB_FIRST 1
#endif
#include <string>
#include <vector>
#include <memory>
#include <utility>

/* ---- MAME spelling of fixed-width types ---- */
using u8  = uint8_t;   using s8  = int8_t;
using u16 = uint16_t;  using s16 = int16_t;
using u32 = uint32_t;  using s32 = int32_t;
using u64 = uint64_t;  using s64 = int64_t;
using offs_t = uint32_t;

#define ATTR_COLD
#define ENDIANNESS_LITTLE 0
enum { AS_PROGRAM = 0, AS_DATA = 1, AS_IO = 3 };
enum { CLEAR_LINE = 0, ASSERT_LINE = 1 };
enum { INPUT_LINE_HALT = 0x10000 };

[[noreturn]] inline void fatalerror(const char *, ...) { std::abort(); }

/* ---- BIT / sign-extend helpers (util/coretmpl) ---- */
template <typename T> constexpr T BIT(T x, unsigned n) { return (x >> n) & T(1); }
template <typename T> constexpr T BIT(T x, unsigned n, unsigned len) {
    return (x >> n) & ((T(1) << len) - 1);
}
#include <cstdio>
inline int g_adsp_logerr = 0; inline long g_adsp_logerr_cnt = 0;
#include <ostream>
namespace util {
    template <typename T> constexpr T sext(T v, unsigned bits) {
        /* mask to the low `bits` bits FIRST, then sign-extend — matches MAME's util::sext.
           Callers pass un-masked values (e.g. `op >> 4` for immediate loads), so without the
           mask the upper opcode bits leak through (e.g. "M4 = 0" op 0x380004 -> 0x8000). */
        T const mask = (T(1) << bits) - 1;
        T const m = T(1) << (bits - 1);
        v &= mask;
        return (v ^ m) - m;
    }
    struct disasm_interface {
        virtual ~disasm_interface() = default;
        enum : uint32_t { SUPPORTED = 0x80000000u, STEP_OVER = 0x10000000u,
                          STEP_OUT = 0x20000000u, STEP_COND = 0x40000000u };
        struct data_buffer { const uint32_t *p; uint32_t size;
            uint32_t r32(uint32_t a) const { return a < size ? p[a] : 0; } };
        virtual uint32_t opcode_alignment() const = 0;
        virtual uint32_t disassemble(std::ostream &, uint32_t pc,
                                     const data_buffer &, const data_buffer &) = 0;
    };
    template <typename... A> void stream_format(std::ostream &s, const char *fmt, A... a) {
        char buf[256]; snprintf(buf, sizeof buf, fmt, a...); s << buf;
    }
}
using util::disasm_interface;
using data_buffer = util::disasm_interface::data_buffer;
enum : uint32_t { STEP_OVER = disasm_interface::STEP_OVER, STEP_OUT = disasm_interface::STEP_OUT,
                  STEP_COND = disasm_interface::STEP_COND, SUPPORTED = disasm_interface::SUPPORTED };

/* ============================================================================================
 *  Memory access — the callback seam. Every CPU read/write funnels through these; we route by
 *  which address space the instance was bound to in device_start (space(AS_x).specific/.cache).
 * ========================================================================================== */
extern u16 dcs_data_read(offs_t addr);
extern void dcs_data_write(offs_t addr, u16 data);
extern u16 dcs_io_read(offs_t addr);
extern void dcs_io_write(offs_t addr, u16 data);
extern u32 dcs_program_read(offs_t addr);
extern void dcs_program_write(offs_t addr, u32 data);

template <int AddrWidth, int DataWidth, int AddrShift, int Endian>
struct mem_access {
    int as = AS_PROGRAM;
    u16 read_word(offs_t a) const  { return as == AS_DATA ? dcs_data_read(a) : dcs_io_read(a); }
    void write_word(offs_t a, u16 d) const { if (as == AS_DATA) dcs_data_write(a, d); else dcs_io_write(a, d); }
    u32 read_dword(offs_t a) const { return dcs_program_read(a); }        // program + cache
    void write_dword(offs_t a, u32 d) const { dcs_program_write(a, d); }
};

template <int AddrWidth, int DataWidth, int AddrShift, int Endian>
struct memory_access {
    using specific = mem_access<AddrWidth, DataWidth, AddrShift, Endian>;
    using cache    = mem_access<AddrWidth, DataWidth, AddrShift, Endian>;
};

struct address_space_config {
    address_space_config(const char *, int, int, int, int) {}
};

/* the fluent space(AS).specific(x)/.cache(x) binder used in device_start */
struct space_binder {
    int as;
    template <class M> void specific(M &m) const { m.as = as; }
    template <class M> void cache(M &m) const    { m.as = as; }
};

/* ============================================================================================
 *  devcb callbacks -> global function pointers (single instance). The write32 form is used for
 *  BOTH SPORT TX (3-arg: port,data,mask) and DMOVLAY (1-arg: data); the argument count
 *  disambiguates, so each routes to its own global unambiguously.
 * ========================================================================================== */
extern void dcs_sport_tx(int port, u32 data);   // SPORT1 autobuffer sample out
extern u32  dcs_sport_rx(int port);
extern void dcs_timer_fired(int state);
extern void dcs_dmovlay(u32 data);

struct devcb_write32 {
    template <class D> devcb_write32(D &) {}
    template <class D> devcb_write32(D &, int) {}
    devcb_write32 &bind() { return *this; }
    void resolve() {}
    void operator()(u32 port, u32 data, u32 /*mask*/) { dcs_sport_tx((int)port, data); }
    void operator()(u32 data) { dcs_dmovlay(data); }
};
struct devcb_read32 {
    template <class D> devcb_read32(D &) {}
    template <class D> devcb_read32(D &, int) {}
    devcb_read32 &bind() { return *this; }
    void resolve() {}
    u32 operator()(int port) { return dcs_sport_rx(port); }
};
struct devcb_write_line {
    template <class D> devcb_write_line(D &) {}
    template <class D> devcb_write_line(D &, int) {}
    devcb_write_line &bind() { return *this; }
    void resolve() {}
    void operator()(int state) { dcs_timer_fired(state); }
};

/* ============================================================================================
 *  Device / save-state / debugger surface — all no-ops for the offline renderer.
 * ========================================================================================== */
struct machine_config {};
struct device_t {};
using device_type = const char *;
#define DECLARE_DEVICE_TYPE(Type, Class) extern device_type Type;
#define DEFINE_DEVICE_TYPE(Type, Class, Short, Full) device_type Type = Short;

/* string_format: only feeds ignored debug-state names */
template <typename... A> inline std::string string_format(const char *, A...) { return std::string(); }

/* state_add(...).mask().callimport()... — a self-returning stub swallowing the whole chain */
struct state_entry_stub {
    template <typename T> state_entry_stub &mask(T) { return *this; }
    template <typename T> state_entry_stub &signed_mask(T) { return *this; }
    state_entry_stub &callimport() { return *this; }
    state_entry_stub &noshow() { return *this; }
    state_entry_stub &formatstr(const char *) { return *this; }
};

struct device_state_entry { int index() const { return 0; } };

/* NAME() unwraps to the value; save_item ignores it */
#define NAME(x) (x)

struct device_memory_interface {
    using space_config_vector = std::vector<std::pair<int, const address_space_config *>>;
};

struct cpu_device : public device_memory_interface {
    cpu_device(const machine_config &, device_type, const char *, device_t *, u32) {}
    virtual ~cpu_device() = default;

    // scaffolding the core calls — all inert here
    space_binder space(int as) { return space_binder{as}; }
    bool has_space(int) const { return false; }             // adsp2105: program + data only
    template <typename T> void save_item(const T &, const char * = nullptr) {}
    template <typename T> state_entry_stub &state_add(int, const char *, T &) {
        static state_entry_stub s; return s;
    }
    void set_icountptr(int &) {}                            // m_icount is driven directly
    bool input_line_state(int) const { return false; }      // never externally halted
    void debugger_instruction_hook(offs_t) {}
    void debugger_wait_hook() {}
    bool debugger_enabled() const { return false; }
    template <typename... A> void logerror(const char *fmt, A... a) const {
        if (g_adsp_logerr) { g_adsp_logerr_cnt++;
            if (g_adsp_logerr_cnt <= 80) { fprintf(stderr, "[LOGERR] "); fprintf(stderr, fmt, a...); } }
    }

    // device_execute / state / disasm overrides the core provides
    virtual void device_start() {}
    virtual void device_reset() {}
    virtual void device_post_load() {}
    virtual u32 execute_min_cycles() const noexcept { return 1; }
    virtual u32 execute_max_cycles() const noexcept { return 1; }
    virtual void execute_run() {}
    virtual void execute_set_input(int, int) {}
    virtual void state_import(const device_state_entry &) {}
    virtual void state_string_export(const device_state_entry &, std::string &) const {}
    virtual std::unique_ptr<util::disasm_interface> create_disassembler() { return nullptr; }
    virtual device_memory_interface::space_config_vector memory_space_config() const { return {}; }
};

/* debug state ids referenced by state_add — value irrelevant (state_add is a no-op) */
enum {
    STATE_GENPC = 0x10000, STATE_GENPCBASE, STATE_GENFLAGS,
};

#endif
