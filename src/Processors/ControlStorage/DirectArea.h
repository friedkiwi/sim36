// The control storage direct areas: the words SVC 0F reads and writes, and
// the only place the system transfer control table's address lives.
//
// This is control storage, not guest storage, which is why it is a field of
// the control processor rather than bytes in main storage.  A microcode
// implementation would have real control storage to put it in; a native one
// keeps it as data, one field per word number.
//
// How a word is named: SVC 0F's two inline parameters form ONE value,
//   word = ((inline1 >> 4) & 7) << 8 | inline2
// three bits of area number above eight bits of displacement, which is why
// the constants below read as 821 and 1074 rather than as an area and an
// offset.  The architected range is 40..1136; outside it the call is an
// error (code 79).
//
// What a word holds: two bytes.  The write path stores the index register's
// low halfword; the read path returns a halfword and appends the high byte
// only when inline 1 bit 0x08 is on.  So the manual's "access 2 or 3 bytes"
// is a per-call choice, and both halves are stored here so that a three-byte
// write reads back as it was written.
//
// What a word contains before anything writes it is not established.  Reads
// of an unwritten word return zero and say so in the trace.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sim36::processors::controlstorage {

class DirectArea {
public:
    static constexpr int kAreas = 8;
    static constexpr int kWordsPerArea = 256;
    static constexpr int kWords = kAreas * kWordsPerArea;

    // The architected bounds: word - 40 must be at most 1096, unsigned.
    static constexpr int kFirstWord = 40;
    static constexpr int kLastWord = 1136;

    // Word 821 (area 3, displacement 0x35) is the system transfer control
    // table's guest address.  SVC 0F is the only way it is ever set, so SVC
    // 04 cannot work until SSP has issued that call; phase 1 does, with
    // F4 00 0F 31 35 00 (direct area, write, from XR2, word 821).
    static constexpr int kTransferControlTable = 821;

    // Word 1124 (area 4, displacement 0x64) is the task work area size.  MSIPL
    // phase 2 reads it twice and hands it to SVC 33 as the region size in
    // WR6; no IPL module ever writes it.  SA21-9436's TWAL entry gives the
    // default: "The default value is 60 sectors."  Which machine property
    // supplies it is emulator policy, exposed as a configuration key.
    static constexpr int kTaskWorkAreaSize = 1124;

    // Word 1074 (area 4, displacement 0x32) is the IPL source, the word phase
    // 1 reads before it writes anything: its low byte becomes guest 0x08A9,
    // and phase 1 branches on bits 0x1C of that byte when a system reload
    // has been requested - zero means diskette (SVC 41), any bit set means
    // tape (SVC 46).  A disk IPL never consults the word.  Seeded at control
    // processor bring-up from the ipl_source configuration key.
    static constexpr int kIplSource = 1074;

    static int word(uint8_t inline1, uint8_t inline2) { return (((inline1 >> 4) & 0x07) << 8) | inline2; }
    static bool inRange(int w) { return w >= kFirstWord && w <= kLastWord; }

    bool wasWritten(int w) const { return written_[w]; }
    // The two-byte value, which is what every caller gets.
    int read(int w) const { return low_[w]; }
    // The third byte, for a caller that asked for three.
    uint8_t readHigh(int w) const { return high_[w]; }
    void write(int w, int value)
    {
        low_[w] = static_cast<uint16_t>(value);
        high_[w] = static_cast<uint8_t>(value >> 16);
        written_[w] = true;
    }

    // Written words as (word, low, high) triples.
    std::vector<int> captureCheckpoint() const;
    // False (and the area cleared) when the checkpoint is malformed.
    bool restoreCheckpoint(const std::vector<int>& values);

    // Name a word for the trace, so an unimplemented consumer of one is
    // attributable rather than anonymous.
    static std::string describe(int w);

private:
    uint16_t low_[kWords] = {};
    uint8_t high_[kWords] = {};
    bool written_[kWords] = {};
};

}  // namespace sim36::processors::controlstorage
