// The typed tape catalog used by a folder's manifest.json and by live media
// inspection, plus the IBM standard tape labels it mirrors.
//
// The manifest carries the volume identity (the VOL1 fields) and the ordered
// list of tape files, and nothing about positioning that is not recoverable
// from the blobs: the manifest describes the medium, the backend models the
// head.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace sim36::storage {

// The VOL1 volume label fields: the six-character serial, the
// access-security byte and the 14-character owner identifier.  `density` and
// `labeled` are modelling metadata, not VOL1 fields.
struct TapeVolume {
    std::string volumeId = "S36VOL";
    std::string accessSecurity = " ";
    std::string ownerId;
    std::string density;
    bool labeled = true;

    nlohmann::ordered_json toJson() const;
    static TapeVolume fromJson(const nlohmann::ordered_json& o);
};

// One tape file, a run of blocks between two tape marks, as the manifest
// records it.  Block boundaries come from `blockLengths` when the blocks
// vary, or from `blockLength` x `blockCount` when they are uniform (the
// common save case).  The label group of a standard-labeled dataset is a
// tape file too, with 80-byte blocks; its decoded fields ride along in
// `labels` for legibility, but the blob's bytes are authoritative.
struct TapeFileEntry {
    int sequence = 0;
    std::string kind = "data";       // "label" | "data", informational
    std::string blob;
    int blockLength = 0;             // uniform block size, when blockLengths is empty
    int blockCount = 0;
    bool hasBlockLengths = false;
    std::vector<int> blockLengths;   // per-block sizes, when blocks vary
    std::string recordFormat = "U";  // F/V/U, informational
    int recordLength = 0;            // informational
    nlohmann::ordered_json labels = nlohmann::ordered_json::object();

    // The byte length of every block in this file, in order.
    std::vector<int> resolveBlockLengths() const;

    nlohmann::ordered_json toJson() const;
    static TapeFileEntry fromJson(const nlohmann::ordered_json& o);
};

class TapeManifest {
public:
    TapeVolume volume;
    std::vector<TapeFileEntry> files;

    std::string write() const;
    // Null with a reason for a manifest of the wrong format or one that does
    // not parse: a malformed manifest is a reason, not a crash.
    static std::unique_ptr<TapeManifest> read(const std::string& text, std::string& reason);
};

// Renders and decodes IBM standard tape labels, byte for byte where the
// decompiled label readers pin the layout.  Labels are EBCDIC in code page
// 037; only the fields those readers name are touched, and the rest of an
// 80-byte record is EBCDIC blanks.
class TapeLabel {
public:
    static constexpr uint8_t kEbcdicSpace = 0x40;

    static std::string normaliseVolumeId(const std::string& volumeId);

    // An 80-byte VOL1: "VOL1" at +0, serial at +4 (6), access-security at
    // +10 (1), owner at +37 (14).
    static std::vector<uint8_t> renderVol1(const std::string& volumeId, char access, const std::string& ownerId);
    static nlohmann::ordered_json decodeVol1(const std::vector<uint8_t>& b, std::size_t at);
    // An HDR1/EOF1/EOV1 record's principal fields: the dataset identifier at
    // +4 (17) and the dataset sequence number at +31 (4).  The block count at
    // +54 is a running total this folder format does not depend on, so it is
    // reported, not enforced.
    static nlohmann::ordered_json decodeDataSet1(const std::vector<uint8_t>& b, std::size_t at);
    // Decode a run of 80-byte records as a standard label group, keying each
    // into `into` by its lower-cased label id (vol1, hdr1, hdr2, eof1, ...).
    // Returns false, decoding nothing, unless the FIRST record carries a
    // recognised label id, which is what distinguishes a real label group
    // from a data file that happens to use 80-byte records.
    static bool decodeLabelGroup(const std::vector<std::vector<uint8_t>>& records, nlohmann::ordered_json& into);

private:
    static bool isKnownLabelId(const std::string& id);
    static nlohmann::ordered_json decodeDataSet2Brief(const std::vector<uint8_t>& b, std::size_t at);
    static void putEbcdic(std::vector<uint8_t>& b, std::size_t at, const std::string& s, std::size_t len);
    static std::string get(const std::vector<uint8_t>& b, std::size_t at, std::size_t len);
};

}  // namespace sim36::storage
