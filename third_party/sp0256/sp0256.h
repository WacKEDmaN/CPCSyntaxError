// license:BSD-3-Clause
// copyright-holders:Joseph Zbiciak,Tim Lindner
//
// GI SP0256 Narrator Speech Processor -- MAME's sp0256 (src/devices/sound/sp0256.cpp,
// commit 638224548b67), with MAME's device / sound-stream / save-state plumbing taken out
// so it can run on its own. The microsequencer, the 12-pole LPC filter and the coefficient
// tables are MAME's code unchanged; the SPB640 FIFO is left out (no CPC board has one).
// Modified for CPCSyntaxError: see LICENSE beside this file for the BSD-3 terms.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

class Sp0256 {
public:
    Sp0256();
    // The chip's 64K address space; the AL2's 2 KB internal ROM sits at &1000.
    std::vector<uint8_t> rom = std::vector<uint8_t>(0x10000, 0);
    void reset();
    void ald_w(uint8_t data);   // address load: an allophone number
    int lrq_r() const { return m_lrq; }        // 1 = ready to take an ALD
    int sby_r() const { return m_sby_line; }   // 1 = standby (nothing being spoken)
    // The chip's own output rate is its clock / 312; this produces `n` of those samples.
    void render(int16_t* out, int n);
    void bitrevbuff(uint8_t* buffer, unsigned int start, unsigned int length);

private:
    struct lpc12_t {
        int update(int num_samp, int16_t* out, uint32_t* optr);
        void regdec();
        int rpt, cnt;
        uint32_t per, rng;
        int amp;
        int16_t f_coef[6];
        int16_t b_coef[6];
        int16_t z_data[6][2];
        uint8_t r[16];
        int interp;
    private:
        static int16_t limit(int16_t s);
    };
    uint32_t getb(int len);
    void micro();

    int m_sby_line = 1;
    int m_silent = 1;
    std::unique_ptr<int16_t[]> m_scratch;
    uint32_t m_sc_head = 0, m_sc_tail = 0;
    lpc12_t m_filt{};
    int m_lrq = 1, m_ald = 0, m_pc = 0, m_stack = 0, m_fifo_sel = 0, m_halted = 1;
    uint32_t m_mode = 0, m_page = 0x1000 << 3;
    uint32_t m_fifo_head = 0, m_fifo_tail = 0, m_fifo_bitp = 0;
    uint16_t m_fifo[64]{};
};
