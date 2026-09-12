// A diskette image in the drive: a flat physical sector image in C/H/S
// order, whose geometry is derived from the volume itself.
//
// There is deliberately no configuration knob for the geometry.  A System/36
// diskette declares its own layout in the VOL1 label on the label track, and
// that declaration is enough to recover every geometry in the media corpus,
// so the user experience is "insert the image and it works" rather than
// "insert the image and then describe it".
//
// Flat only: a container format belongs in a conversion tool, not in a
// device model, so the drive has exactly one thing to understand.
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace sim36::storage {

// The geometry of one diskette volume, derived in four steps, every one of
// them measured against the corpus rather than assumed:
//
// 1. Probe the label track for the EBCDIC VOL1 (E5 D6 D3 F1) at label sector
//    7: byte offset 6 x 128 first, then 6 x 256.  Whichever hits gives the
//    label-track sector size, and therefore whether this is 8-inch or 5 1/4-inch
//    media.  A volume with neither is not S/36 media.
// 2. VOL1+75 is the data-area sector size: F3 1024, F2 512, F1 256, and
//    EBCDIC blank (40) = 128, the 3740 default the "Diskette 1" volumes carry.
// 3. Sectors per track follows from that size: 1024 -> 8, 512 -> 15, 256 -> 26,
//    128 -> 26.
// 4. The cylinder count comes from the remaining file length.  Total size
//    alone identifies nothing: two volumes of the same length can have
//    different layouts.
class DisketteGeometry {
public:
    // 26 on every S/36 label track, both media sizes: the label track is the
    // 3740's index track and its sector count never changed.
    static constexpr int kLabelSectorsPerTrack = 26;
    // The VOL1 label lives in label sector 7, 1-based, which is also the
    // record number SSP asks for: IPL phase 1's CHRNX field reads C=0 H=0 R=7
    // N=0 X=2, three 128-byte records from sector 7.
    static constexpr int kVolumeLabelSector = 7;
    // Offset of the physical record length code within VOL1.
    static constexpr int kVolumeLabelPrlOffset = 75;
    // Cylinder 0 head 1 is 26 x 256 on BOTH media sizes.  On 8-inch media head
    // 0 is FM 26 x 128 while head 1 is MFM 26 x 256, so a single cylinder
    // carries two tracks of different lengths; on 5 1/4-inch media both heads
    // are 26 x 256.  Getting this wrong shifts every data cylinder by 3 328
    // bytes.
    static constexpr int kBackLabelTrackSectorBytes = 256;

    int labelSectorBytes() const { return labelSectorBytes_; }
    int dataSectorBytes() const { return dataSectorBytes_; }
    int dataSectorsPerTrack() const { return dataSectorsPerTrack_; }
    int heads() const { return heads_; }
    int cylinders() const { return cylinders_; }

    // The volume identifier from VOL1+4, EBCDIC, for the monitor.
    const std::string& volumeId() const { return volumeId_; }
    // The owner identifier from VOL1+37: the program-product number on IBM
    // product media, IBMROCHESTERMN on microcode.
    const std::string& ownerId() const { return ownerId_; }

    // 8-inch media is the one whose label track is 128-byte sectors.  A
    // report, not an input: nothing in the addressing depends on it.
    bool isEightInch() const { return labelSectorBytes_ == 128; }

    // Bytes per data track.  Not a constant of the volume: cylinder 0 is the
    // label track and is sized differently.
    int dataTrackBytes() const { return dataSectorsPerTrack_ * dataSectorBytes_; }

    int cylinderZeroBytes() const
    {
        return kLabelSectorsPerTrack * labelSectorBytes_ +
               (heads_ > 1 ? kLabelSectorsPerTrack * kBackLabelTrackSectorBytes : 0);
    }

    long long totalBytes() const
    {
        return cylinderZeroBytes() + static_cast<long long>(cylinders_ - 1) * heads_ * dataTrackBytes();
    }

    // Sector size of one physical track.  Per track, never per volume: this
    // is the single fact a naive reader gets wrong.
    int trackSectorBytes(int cylinder, int head) const
    {
        if (cylinder != 0) return dataSectorBytes_;
        return head == 0 ? labelSectorBytes_ : kBackLabelTrackSectorBytes;
    }

    int trackSectors(int cylinder, int /*head*/) const
    {
        return cylinder == 0 ? kLabelSectorsPerTrack : dataSectorsPerTrack_;
    }

    int trackBytes(int cylinder, int head) const
    {
        return trackSectors(cylinder, head) * trackSectorBytes(cylinder, head);
    }

    // Byte offset of the start of one track in the flat image, in the C/H/S
    // linearisation the export tool writes.
    long long trackOffset(int cylinder, int head) const;

    // Byte offset of one record.  `record` is the recorded sector ID, 1-based,
    // which is what a CHRNX field carries.
    long long recordOffset(int cylinder, int head, int record) const
    {
        return trackOffset(cylinder, head) + static_cast<long long>(record - 1) * trackSectorBytes(cylinder, head);
    }

    // The last cylinder sequential addressing reaches.  NOT the last cylinder
    // on the volume: SA21-9243-4 8-2, "Of the 77 cylinders, only 75 are
    // normally used.  Cylinder 00 is the index cylinder ...; cylinders 1
    // through 74, the primary cylinders, store data records.  Cylinders 75 and
    // 76 (alternative cylinders) are available for data storage in the event
    // that one or two of the primary cylinders becomes damaged", and 8-12 on
    // the address space itself: it "extends through the last sector on
    // cylinder 74".  The two spare cylinders are reachable only by CHRNX.
    int lastSequentialCylinder() const { return cylinders_ - 3; }

    // The highest sequential-sector address on this volume: the last sector
    // of lastSequentialCylinder().  1184 on 8-inch 1024-byte media, which is
    // the constant the Advanced/36's own diskette driver compares against.
    int lastSequentialSector() const { return lastSequentialCylinder() * heads_ * dataSectorsPerTrack_; }

    // Sequential sector address -> cylinder/head/record, 1-based.
    //
    // SA21-9243-4 8-12: "Sequential sector addressing starts at hexadecimal
    // address 0001 (cylinder 1, data head 0, sector 1), increases by 1 for
    // each following sector, and extends through the last sector on cylinder
    // 74.  Cylinder 0 cannot be addressed by sequential sector addressing."
    // The label track is outside this address space entirely.  The traversal
    // is head-first then cylinder, the same order next() walks and the same
    // order HDR1 extents are written in.
    bool sequentialToRecord(int ss, int& cylinder, int& head, int& record) const;

    bool isValid(int cylinder, int head, int record) const
    {
        return cylinder >= 0 && cylinder < cylinders_ && head >= 0 && head < heads_ && record >= 1 &&
               record <= trackSectors(cylinder, head);
    }

    // Derive the geometry from the first bytes of the image and its length.
    // Returns false with a reason naming exactly what was missing, never a
    // silent default: a diskette whose layout is guessed reads plausible
    // rubbish.
    static bool tryProbe(const std::vector<uint8_t>& labelTrack, long long length, DisketteGeometry& g,
                         std::string& reason);

    std::string toString() const;

private:
    DisketteGeometry() = default;

    int labelSectorBytes_ = 0;
    int dataSectorBytes_ = 0;
    int dataSectorsPerTrack_ = 0;
    int heads_ = 0;
    int cylinders_ = 0;
    std::string volumeId_;
    std::string ownerId_;

    friend class DisketteBackend;
};

class DisketteBackend {
public:
    ~DisketteBackend();
    DisketteBackend(const DisketteBackend&) = delete;
    DisketteBackend& operator=(const DisketteBackend&) = delete;

    const std::string& path() const { return path_; }
    bool readOnly() const { return readOnly_; }
    const DisketteGeometry& geometry() const { return geometry_; }

    // Open an image and derive its geometry.  Returns null with a reason
    // rather than throwing, because "that is not a System/36 diskette" is an
    // answer the operator should see, not an emulator failure.
    static std::unique_ptr<DisketteBackend> open(const std::string& path, bool readOnly, std::string& reason);

    // A data set named by an HDR1 label on the cylinder-0 label track.
    // Extents are CCHRR: two digits of cylinder, one of head, two of record,
    // so 01001 is cylinder 1, head 0, record 1.
    struct DataSet {
        std::string name;
        int cylinder = 0, head = 0, record = 0;            // beginning of extent
        int endCylinder = 0, endHead = 0, endRecord = 0;   // end of extent
        int recordBytes = 0;                               // from the HDR1 PRL code
    };

    // Find a data set by name, walking the HDR1 labels from label-track
    // record 8.  False when there is none, which is an ordinary answer: most
    // volumes in the corpus carry no #IPLBOOT.  The label is 128 bytes
    // wherever it lives (on 5 1/4-inch media it is left-justified in a
    // 256-byte sector), so only the first 128 bytes of the record are read.
    bool findDataSet(const std::string& name, DataSet& out);

    // Read one record.  `record` is the 1-based recorded sector ID.  False
    // when the image is shorter than its geometry declares.
    bool readRecord(int cylinder, int head, int record, std::vector<uint8_t>& out);

    // Write one record.  `count` is the caller's record length, which may be
    // SHORTER than the physical sector: a 128-byte label lives in a 256-byte
    // sector on 5 1/4-inch media.  A short record is read-modify-written so
    // the rest of the sector keeps whatever the medium already had.  False on
    // a read-only medium, a record longer than its sector, or a host I/O
    // failure; `why` says which.
    bool writeRecord(int cylinder, int head, int record, const uint8_t* src, int count, std::string& why);

    // The record after (c, h, r) in the volume's linear traversal: head first,
    // then cylinder, which is the order HDR1 extents are written in (57008 is
    // followed by 57101, so the head digit carries).
    bool next(int& cylinder, int& head, int& record) const;

private:
    DisketteBackend(const std::string& path, bool readOnly, std::FILE* f, const DisketteGeometry& g)
        : path_(path), readOnly_(readOnly), file_(f), geometry_(g) {}

    std::string path_;
    bool readOnly_;
    std::FILE* file_;
    DisketteGeometry geometry_;
};

}  // namespace sim36::storage
