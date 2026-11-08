/*
 * Night Slashers native runtime: sound section.
 *
 * Board (MAME src/mame/dataeast/fghthist.cpp, nslasher()):
 *   Z80 at 32.22 MHz / 9      program 0x0000-0x7fff ROM, 0x8000-0x87ff RAM,
 *                             0xa000 YM2151, 0xb000 OKI #1, 0xc000 OKI #2,
 *                             0xd000 sound latch (reading clears the latch IRQ);
 *                             I/O space reads the sound ROM.
 *   YM2151 at 32.22 MHz / 9   CT1/CT2 select the OKI sample banks; IRQ to the Z80
 *   OKI M6295 #1 at /32, #2 at /16, pin 7 high (clock / 132)
 *   Mix: YM 0.40, OKI #1 0.80, OKI #2 0.10, all scaled by the 0x150001 volume.
 *
 * The Z80 sound program is run by the superzazu/z80 core (MIT): unlike the ARM main
 * program it is not yet translated ahead of time (see docs/KNOWN_ISSUES.md). The
 * YM2151 is Aaron Giles' ymfm (BSD-3-Clause), the reference MAME itself uses. The OKI
 * ADPCM follows MAME's okim6295.cpp/okiadpcm.cpp.
 *
 * Timing: the main CPU cannot read anything back from the sound CPU, so sound-latch
 * writes are time-stamped in ARM cycles during the frame and replayed at the same
 * moment of Z80 time when the sound section runs for that frame.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "ymfm_opm.h"

extern "C" {
#include "nsl/sound.h"
#include "nsl/cpu.h"
#include "nsl/machine.h"
#include "z80.h"
}

#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr int kOutputRate = 48000;
/* time unit: 1/9 Z80 cycle, so every device period is an integer */
constexpr uint64_t kYmPeriod = 64 * 9;         /* YM2151 sample = 64 clocks */
constexpr uint64_t kOki1Period = 32 * 132;     /* (32.22 MHz / 32 / 132) in Z80/9 units */
constexpr uint64_t kOki2Period = 16 * 132;
constexpr double kYmRate = 3580000.0 / 64.0;   /* 55937.5 Hz */

const uint8_t *g_program;
const uint8_t *g_oki_rom[2];

int g_diff_lookup[49 * 16];
const int8_t kIndexShift[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
const int kVolumeTable[16] = { 0x20, 0x16, 0x10, 0x0b, 0x08, 0x06, 0x04, 0x03, 0x02, 0, 0, 0, 0, 0, 0, 0 };

struct OkiVoice {
    int32_t playing;
    uint32_t base_offset;
    uint32_t sample;
    uint32_t count;
    int32_t volume;
    int32_t signal;
    int32_t step;
};

struct Oki {
    OkiVoice voice[4];
    int32_t command;
    uint32_t bank;
    int32_t output;            /* last sample, -2048*32..2047*32 summed */
};

/* All plain state of the section, serialised as one block (plus the Z80 and ymfm). */
struct SoundState {
    uint8_t ram[0x800];
    uint64_t z80_time;            /* in 1/9 Z80 cycles */
    uint64_t ym_next, oki_next[2];
    int64_t timer_remaining[2];   /* YM timers, Z80 cycles; <0 disabled */
    uint64_t busy_until;          /* Z80 cycle */
    uint32_t latch;
    int32_t latch_irq;
    int32_t ym_irq;
    Oki oki[2];
    int32_t ym_out[2];
    double resample_phase;
    float last_mix[2];
    uint32_t latch_reads;
};

SoundState g_state;
z80 g_z80;

struct LatchEvent { uint64_t arm_cycle; uint8_t value; };
std::vector<LatchEvent> g_events;
std::vector<float> g_mix;                 /* YM-rate stereo mix for the current frame */
std::vector<int16_t> g_output;
uint64_t g_frame_arm_end;

uint64_t z80_cycles_now() { return g_state.z80_time / 9; }

/* ---- OKI M6295 ---------------------------------------------------------- */

void oki_compute_tables() {
    static const int8_t nbl2bit[16][4] = {
        { 1, 0, 0, 0}, { 1, 0, 0, 1}, { 1, 0, 1, 0}, { 1, 0, 1, 1},
        { 1, 1, 0, 0}, { 1, 1, 0, 1}, { 1, 1, 1, 0}, { 1, 1, 1, 1},
        {-1, 0, 0, 0}, {-1, 0, 0, 1}, {-1, 0, 1, 0}, {-1, 0, 1, 1},
        {-1, 1, 0, 0}, {-1, 1, 0, 1}, {-1, 1, 1, 0}, {-1, 1, 1, 1}
    };
    for (int step = 0; step <= 48; step++) {
        int stepval = (int)std::floor(16.0 * std::pow(11.0 / 10.0, (double)step));
        for (int nib = 0; nib < 16; nib++) {
            g_diff_lookup[step * 16 + nib] = nbl2bit[nib][0] *
                (stepval * nbl2bit[nib][1] + stepval / 2 * nbl2bit[nib][2] + stepval / 4 * nbl2bit[nib][3] + stepval / 8);
        }
    }
}

uint8_t oki_rom(int chip, uint32_t offset) {
    return g_oki_rom[chip][(g_state.oki[chip].bank * 0x40000 + (offset & 0x3FFFF)) & 0x7FFFF];
}

int32_t oki_generate(int chip) {
    Oki &oki = g_state.oki[chip];
    int32_t sum = 0;
    for (auto &v : oki.voice) {
        if (!v.playing) continue;
        int nibble = oki_rom(chip, v.base_offset + v.sample / 2) >> (((v.sample & 1) << 2) ^ 4);
        v.signal += g_diff_lookup[v.step * 16 + (nibble & 15)];
        if (v.signal > 2047) v.signal = 2047; else if (v.signal < -2048) v.signal = -2048;
        v.step += kIndexShift[nibble & 7];
        if (v.step > 48) v.step = 48; else if (v.step < 0) v.step = 0;
        sum += v.signal * v.volume;            /* volume in 1/32 units */
        if (++v.sample >= v.count) v.playing = 0;
    }
    return sum;
}

uint8_t oki_read(int chip) {
    uint8_t result = 0xf0;
    for (int i = 0; i < 4; i++) if (g_state.oki[chip].voice[i].playing) result |= (uint8_t)(1 << i);
    return result;
}

void oki_write(int chip, uint8_t command) {
    Oki &oki = g_state.oki[chip];
    if (oki.command != -1) {
        int mask = command >> 4;
        for (int i = 0; i < 4; i++, mask >>= 1) {
            if (!(mask & 1)) continue;
            OkiVoice &v = oki.voice[i];
            if (v.playing) continue;
            uint32_t base = (uint32_t)oki.command * 8;
            uint32_t start = ((oki_rom(chip, base) << 16) | (oki_rom(chip, base + 1) << 8) | oki_rom(chip, base + 2)) & 0x3ffff;
            uint32_t stop = ((oki_rom(chip, base + 3) << 16) | (oki_rom(chip, base + 4) << 8) | oki_rom(chip, base + 5)) & 0x3ffff;
            if (start < stop) {
                v.playing = 1;
                v.base_offset = start;
                v.sample = 0;
                v.count = 2 * (stop - start + 1);
                v.signal = 0;
                v.step = 0;
                v.volume = kVolumeTable[command & 0x0f];
            }
        }
        oki.command = -1;
    } else if (command & 0x80) {
        oki.command = command & 0x7f;
    } else {
        int mask = command >> 3;
        for (int i = 0; i < 4; i++, mask >>= 1) if (mask & 1) oki.voice[i].playing = 0;
    }
}

/* ---- YM2151 through ymfm ------------------------------------------------ */

class YmInterface : public ymfm::ymfm_interface {
public:
    void ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) override {
        g_state.timer_remaining[tnum & 1] = duration_in_clocks;   /* YM clock == Z80 clock */
    }
    void ymfm_set_busy_end(uint32_t clocks) override { g_state.busy_until = z80_cycles_now() + clocks; }
    bool ymfm_is_busy() override { return z80_cycles_now() < g_state.busy_until; }
    void ymfm_update_irq(bool asserted) override { g_state.ym_irq = asserted ? 1 : 0; }
    void ymfm_external_write(ymfm::access_class type, uint32_t address, uint8_t data) override {
        (void)type; (void)address;
        g_state.oki[0].bank = data & 1;          /* fghthist_common_state::sound_bankswitch_w */
        g_state.oki[1].bank = (data >> 1) & 1;
    }
    void expire(uint32_t tnum) { m_engine->engine_timer_expired(tnum); }
};

YmInterface g_ym_intf;
ymfm::ym2151 *g_ym;

/* ---- Z80 bus ------------------------------------------------------------ */

uint8_t z80_read(void *, uint16_t address) {
    if (address < 0x8000) return g_program[address];
    if (address < 0x8800) return g_state.ram[address - 0x8000];
    if (address == 0xa000 || address == 0xa001) return g_ym->read(address & 1);
    if (address == 0xb000) return oki_read(0);
    if (address == 0xc000) return oki_read(1);
    if (address == 0xd000) {
        g_state.latch_irq = 0;                   /* deco_146_base_device::soundlatch_r */
        g_state.latch_reads++;
        return (uint8_t)g_state.latch;
    }
    return 0;
}

void z80_write(void *, uint16_t address, uint8_t value) {
    if (address >= 0x8000 && address < 0x8800) { g_state.ram[address - 0x8000] = value; return; }
    if (address == 0xa000 || address == 0xa001) { g_ym->write(address & 1, value); return; }
    if (address == 0xb000) { oki_write(0, value); return; }
    if (address == 0xc000) { oki_write(1, value); return; }
}

uint8_t z80_port_in(z80 *, uint16_t port) { return g_program[port]; }
void z80_port_out(z80 *, uint8_t, uint8_t) {}

void z80_bind() {
    g_z80.read_byte = z80_read;
    g_z80.write_byte = z80_write;
    g_z80.port_in = z80_port_in;
    g_z80.port_out = z80_port_out;
    g_z80.userdata = nullptr;
}

/* ---- time advance ------------------------------------------------------- */

float output_gain() {
    return (float)(0xff - (nsl_machine.volume & 0xff)) / 255.0f;
}

void advance_devices(uint64_t until) {
    /* runs the YM/OKI sample clocks and YM timers up to 'until' (1/9 cycle units) */
    while (true) {
        uint64_t next = g_state.ym_next;
        if (g_state.oki_next[0] < next) next = g_state.oki_next[0];
        if (g_state.oki_next[1] < next) next = g_state.oki_next[1];
        if (next > until) break;
        if (next == g_state.oki_next[0]) { g_state.oki[0].output = oki_generate(0); g_state.oki_next[0] += kOki1Period; }
        if (next == g_state.oki_next[1]) { g_state.oki[1].output = oki_generate(1); g_state.oki_next[1] += kOki2Period; }
        if (next == g_state.ym_next) {
            ymfm::ym2151::output_data out;
            g_ym->generate(&out, 1);
            g_state.ym_out[0] = out.data[0];
            g_state.ym_out[1] = out.data[1];
            g_state.ym_next += kYmPeriod;
            float gain = output_gain();
            float oki = (g_state.oki[0].output / 32.0f / 2048.0f) * 0.80f
                      + (g_state.oki[1].output / 32.0f / 2048.0f) * 0.10f;
            g_mix.push_back(gain * (g_state.ym_out[0] / 32768.0f * 0.40f + oki));
            g_mix.push_back(gain * (g_state.ym_out[1] / 32768.0f * 0.40f + oki));
        }
    }
}

void tick_timers(uint32_t cycles) {
    for (int t = 0; t < 2; t++) {
        if (g_state.timer_remaining[t] < 0) continue;
        g_state.timer_remaining[t] -= cycles;
        if (g_state.timer_remaining[t] <= 0) {
            int64_t overshoot = -g_state.timer_remaining[t];
            g_state.timer_remaining[t] = -1;
            g_ym_intf.expire((uint32_t)t);           /* re-arms through ymfm_set_timer */
            if (g_state.timer_remaining[t] > 0) g_state.timer_remaining[t] -= overshoot;
        }
    }
}

void run_z80_until(uint64_t until) {
    while (g_state.z80_time < until) {
        g_z80.int_pending = (g_state.latch_irq || g_state.ym_irq) ? 1 : 0;
        g_z80.int_data = 0xff;
        g_z80.cyc = 0;
        z80_step(&g_z80);
        uint32_t cycles = (uint32_t)g_z80.cyc;
        if (cycles == 0) cycles = 4;
        g_state.z80_time += (uint64_t)cycles * 9;
        tick_timers(cycles);
        advance_devices(g_state.z80_time);
    }
}

uint64_t arm_to_z80_time(uint64_t arm_cycles) {
    /* 3.58 MHz / 7 MHz in 1/9 cycle units: arm * 358 * 9 / 700 */
    return arm_cycles * 358ull * 9ull / 700ull;
}

void resample_frame() {
    /* Linear interpolation from the YM rate to 48 kHz. Source index -1 is the last
     * sample of the previous frame; 'resample_phase' is carried across frames. */
    g_output.clear();
    const long frames = (long)(g_mix.size() / 2);
    const double step = kYmRate / kOutputRate;
    double phase = g_state.resample_phase;
    while (frames > 0 && phase < (double)(frames - 1)) {
        long i0 = (long)std::floor(phase);
        double frac = phase - (double)i0;
        float l0 = i0 < 0 ? g_state.last_mix[0] : g_mix[(size_t)i0 * 2];
        float r0 = i0 < 0 ? g_state.last_mix[1] : g_mix[(size_t)i0 * 2 + 1];
        float l1 = g_mix[(size_t)(i0 + 1) * 2], r1 = g_mix[(size_t)(i0 + 1) * 2 + 1];
        float l = (float)(l0 + (l1 - l0) * frac), r = (float)(r0 + (r1 - r0) * frac);
        long li = std::lround(l * 32767.0f), ri = std::lround(r * 32767.0f);
        if (li > 32767) li = 32767; else if (li < -32768) li = -32768;
        if (ri > 32767) ri = 32767; else if (ri < -32768) ri = -32768;
        g_output.push_back((int16_t)li);
        g_output.push_back((int16_t)ri);
        phase += step;
    }
    if (frames > 0) {
        g_state.resample_phase = phase - (double)frames;
        g_state.last_mix[0] = g_mix[(size_t)(frames - 1) * 2];
        g_state.last_mix[1] = g_mix[(size_t)(frames - 1) * 2 + 1];
    }
    g_mix.clear();
}

std::vector<uint8_t> ym_state() {
    std::vector<uint8_t> buffer;
    ymfm::ymfm_saved_state saver(buffer, true);
    g_ym->save_restore(saver);
    return buffer;
}

size_t g_ym_state_size;

}  // namespace

extern "C" {

void nsl_sound_init(const uint8_t *z80_program, const uint8_t *oki1, const uint8_t *oki2) {
    g_program = z80_program;
    g_oki_rom[0] = oki1;
    g_oki_rom[1] = oki2;
    oki_compute_tables();
    delete g_ym;
    g_ym = new ymfm::ym2151(g_ym_intf);
    nsl_sound_reset();
    g_ym_state_size = ym_state().size();
}

void nsl_sound_reset(void) {
    std::memset(&g_state, 0, sizeof(g_state));
    g_state.timer_remaining[0] = g_state.timer_remaining[1] = -1;
    g_state.oki[0].command = g_state.oki[1].command = -1;
    g_state.ym_next = kYmPeriod;
    g_state.oki_next[0] = kOki1Period;
    g_state.oki_next[1] = kOki2Period;
    z80_init(&g_z80);
    z80_bind();
    if (g_ym) g_ym->reset();
    g_events.clear();
    g_mix.clear();
    /* the board's frame timeline starts with the ARM at cycle 0 */
    g_state.z80_time = arm_to_z80_time(nsl_cpu.cycles);
    g_state.ym_next += g_state.z80_time;
    g_state.oki_next[0] += g_state.z80_time;
    g_state.oki_next[1] += g_state.z80_time;
}

void nsl_sound_shutdown(void) {
    delete g_ym;
    g_ym = nullptr;
    g_program = nullptr;
}

void nsl_sound_latch_write(uint8_t value) {
    g_events.push_back({ nsl_cpu.cycles, value });
}

void nsl_sound_begin_frame(void) {
    g_events.clear();
}

void nsl_sound_end_frame(void) {
    if (!g_program || !g_ym) { g_output.assign(1600, 0); return; }
    for (const LatchEvent &event : g_events) {
        run_z80_until(arm_to_z80_time(event.arm_cycle));
        g_state.latch = event.value;
        g_state.latch_irq = 1;                       /* soundlatch_irq_cb(ASSERT_LINE) */
    }
    g_events.clear();
    run_z80_until(arm_to_z80_time(nsl_cpu.cycles));
    resample_frame();
}

int nsl_sound_output_rate(void) { return kOutputRate; }

size_t nsl_sound_output(const int16_t **samples) {
    *samples = g_output.data();
    return g_output.size() / 2;
}

/* state: SoundState | Z80 registers (pointers excluded) | ymfm blob */
size_t nsl_sound_state_size(void) {
    return sizeof(SoundState) + sizeof(z80) + g_ym_state_size;
}

void nsl_sound_save(uint8_t *out) {
    std::memcpy(out, &g_state, sizeof(g_state));
    z80 copy = g_z80;
    copy.read_byte = nullptr; copy.write_byte = nullptr;
    copy.port_in = nullptr; copy.port_out = nullptr; copy.userdata = nullptr;
    std::memcpy(out + sizeof(g_state), &copy, sizeof(copy));
    std::vector<uint8_t> ym = ym_state();
    std::memcpy(out + sizeof(g_state) + sizeof(z80), ym.data(), g_ym_state_size);
}

void nsl_sound_load(const uint8_t *in) {
    std::memcpy(&g_state, in, sizeof(g_state));
    std::memcpy(&g_z80, in + sizeof(g_state), sizeof(g_z80));
    z80_bind();
    std::vector<uint8_t> ym(in + sizeof(g_state) + sizeof(z80), in + sizeof(g_state) + sizeof(z80) + g_ym_state_size);
    ymfm::ymfm_saved_state loader(ym, false);
    g_ym->save_restore(loader);
    g_events.clear();
    g_mix.clear();
    g_output.clear();
}

uint32_t nsl_sound_debug_latch_reads(void) { return g_state.latch_reads; }

}  // extern "C"
