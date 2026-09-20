#include "Devices/VirtualFixedDisk.h"

#include <cstring>

#include <fmt/format.h>

#include "Devices/IoBlock.h"
#include "Processors/ControlStorage/ActionControlElement.h"

namespace sim36::devices {

using processors::controlstorage::Ecm;
using storage::DiskBackend;

void IoBlock::complete(machine::MachineState& m, int iob, int code) { Ecm::post(m, iob, code); }

std::string IoBlock::dump(machine::MachineState& m, int iob)
{
    std::string s;
    s += fmt::format("IOB {:06X}\n", iob);
    s += fmt::format("  +00 ecm ace addr {:06X}\n", m.readAddr24(iob + Ecm::kOffAceAddress));
    s += fmt::format("  +05 multi-wait   {:02X}\n", m.readByte(iob + Ecm::kOffMultiWait));
    s += fmt::format("  +06 completion   {:02X} {}\n", m.readByte(iob + Ecm::kOffCompletion),
                     Ecm::isComplete(m, iob) ? "(complete)" : "(pending)");
    s += fmt::format("  +0A command      {:04X}\n", command(m, iob));
    s += fmt::format("  +13 buffer       {:06X}\n", m.readAddr24(iob + kOffDataBuffer));
    s += fmt::format("  +16 count-1      {}\n", m.readAddr24(iob + kOffDiskCount));
    s += fmt::format("  +19 sector       {} (1-based)\n", m.readAddr24(iob + kOffDiskSector));
    return s;
}

bool VirtualFixedDisk::execute(int iob, uint8_t qByte)
{
    // THE COMMAND IS THE HIGH BYTE, +0x0A; +0x0B is a modifier.
    const int command = m_.readByte(iob + IoBlock::kOffCommand);
    const int modifier = m_.readByte(iob + IoBlock::kOffCommandModifier);
    // The wire sector is 1-based.
    const int sector = m_.readAddr24(iob + IoBlock::kOffDiskSector) - 1;
    const int count = m_.readAddr24(iob + IoBlock::kOffDiskCount);
    const int bufferField = m_.readAddr24(iob + IoBlock::kOffDataBuffer);
    const bool bypassCache = (qByte & 0x20) != 0;

    // The work sector is seeded from the start sector before the command is
    // even looked at, so A0 and A4 get the seed too although they transfer
    // nothing.
    m_.writeAddr24(iob + IoBlock::kOffDiskSectorWork, m_.readAddr24(iob + IoBlock::kOffDiskSector));

    trace_.diskIo("SVC 40 iob={:06X} cmd={:02X}/{:02X} sector={} count-1={} buffer={:06X} bypass-cache={}",
                  iob, command, modifier, sector, count, bufferField, bypassCache ? "True" : "False");

    if (command == kCommandCompleteDefault || command == kCommandComplete ||
        command == kCommandCompleteAlt) {
        // NuDiskIo::executeInternal (V4R4 c1864a4c..c1865478) seeds its
        // return value with 0x40.  A1/A2/A3 replace it through their transfer
        // arms; an observed RPGC #MGRE request with command 00 reaches the
        // common return unchanged, just like the explicit A0/A4 early arm.
        // Keep this deliberately limited to the observed 00 default form.
        trace_.diskIo("  command {:02X} completes immediately, no transfer", command);
        IoBlock::complete(m_, iob, 0);
        return true;
    }

    // A READ whose 1-based disk sector address is 0 is not an out-of-volume
    // error: it is a request against an already-resident work-space page,
    // and it completes without a transfer, leaving the buffer intact.  A
    // genuine absolute read always names a 1-based sector >= 1.
    if (command == kCommandRead && m_.readAddr24(iob + IoBlock::kOffDiskSector) == 0) {
        trace_.diskIo("  read with 1-based disk sector 0 - an already-resident work-space "
                      "page (single-level store): completes with no transfer, buffer "
                      "left intact. docs/s36/svc40-fixed-disk-ios.md");
        IoBlock::complete(m_, iob, 0);
        return true;
    }

    if (command != kCommandRead && command != kCommandWrite && command != kCommandScan) {
        trace_.diskIo("  command {:02X} (unknown) is not implemented. docs/s36/device-io.md", command);
        IoBlock::complete(m_, iob, 4);
        return false;
    }

    if (sector < 0 || sector >= volume_.sectorCount()) {
        trace_.diskIo("  sector {} outside the volume ({})", sector, volume_.sectorCount());
        IoBlock::complete(m_, iob, 4);
        return false;
    }

    // The transfer count is held as count-1, so a stored zero is one sector.
    const int sectors = count + 1;
    if (sector + sectors > volume_.sectorCount()) {
        trace_.diskIo("  {} sector(s) from {} runs past the end of the volume", sectors, sector);
        IoBlock::complete(m_, iob, 4);
        return false;
    }

    if (command == kCommandWrite) return write(iob, sector, sectors, modifier, bufferField);
    if (command == kCommandScan) return scan(iob, sector, sectors, modifier, bufferField);

    std::vector<uint8_t> buffer(static_cast<std::size_t>(sectors) * DiskBackend::kSectorBytes);
    for (int i = 0; i < sectors; ++i)
        volume_.readSector(sector + i, buffer.data() + static_cast<std::size_t>(i) * DiskBackend::kSectorBytes);

    lastRead_ = buffer;
    lastReadSector_ = sector;
    ++readsIssued_;
    sectorsRead_ += sectors;

    // Where it lands: bit 0x800000 means the low 16 bits are a
    // task-translated address, resolved through task group 0.
    Extents dest;
    if (!resolveBufferExtents(bufferField, static_cast<int>(buffer.size()), dest) ||
        !extentsInStorage(dest, bufferField)) {
        IoBlock::complete(m_, iob, 4);
        return false;
    }
    writeExtents(dest, buffer);

    trace_.diskIo("  read {} sector(s) from {} to guest {:06X}", sectors, sector, dest[0].first);

    advanceWorkSector(iob, sector, sectors);
    IoBlock::complete(m_, iob, 0);
    return true;
}

// Leave +23..25 naming the LAST sector transferred, 1-based, not the one
// after it: the consumer adds one to get the next sector.
void VirtualFixedDisk::advanceWorkSector(int iob, int sector, int sectors)
{
    m_.writeAddr24(iob + IoBlock::kOffDiskSectorWork, sector + sectors);
}

// Command A3, scan: read sector after sector comparing a key in each against
// a search argument, and stop at the first hit, transferring that sector.
// The search argument is the first keyLength bytes of the data buffer, which
// the hit then overwrites with the found sector.  Scan ID (relation 3) and
// the bisecting arm are not implemented and are refused rather than guessed.
bool VirtualFixedDisk::scan(int iob, int sector, int sectors, int relation, int bufferField)
{
    const int firstKey = m_.readByte(iob + IoBlock::kOffScanFirstKey);
    const int keyLength = m_.readByte(iob + IoBlock::kOffScanKeyLength) + 1;
    const int lastKey = m_.readByte(iob + IoBlock::kOffScanLastKey);
    const int gap = m_.readByte(iob + IoBlock::kOffScanKeyGap);
    const int step = keyLength + gap;

    if (relation >= IoBlock::kScanId) {
        trace_.diskIo("  scan relation {:02X} is not implemented - 00 equal, 01 low "
                      "or equal and 02 high or equal are. docs/s36/device-io.md", relation);
        IoBlock::complete(m_, iob, 4);
        return false;
    }
    if (step <= 0 || firstKey + keyLength > DiskBackend::kSectorBytes) {
        trace_.diskIo("  scan geometry is unusable: first={} length={} last={} gap={}",
                      firstKey, keyLength, lastKey, gap);
        IoBlock::complete(m_, iob, 4);
        return false;
    }

    // The buffer holds the search argument and receives the whole sector on
    // a hit, so it is a 256-byte buffer however few bytes the key is.
    Extents bufferExtents;
    if (!resolveBufferExtents(bufferField, DiskBackend::kSectorBytes, bufferExtents) ||
        !extentsInStorage(bufferExtents, bufferField)) {
        IoBlock::complete(m_, iob, 4);
        return false;
    }
    const int buffer = bufferExtents[0].first;

    std::vector<uint8_t> argument(static_cast<std::size_t>(keyLength));
    readExtents(resolveArgumentExtents(bufferExtents, keyLength), argument);

    // The 62EH attachment treats an FF byte in the argument as a mask; whether
    // the Advanced/36 honours it is unknown.  This compares plainly and says so.
    for (uint8_t b : argument) {
        if (b == 0xFF) {
            trace_.diskIo("  ASSUMED: the search argument contains an FF byte, which the "
                          "62EH attachment treats as a MASK. This comparison is plain. "
                          "docs/s36/device-io.md");
            break;
        }
    }

    trace_.diskIo("  scan {} ({}) over {} sector(s) from {}: {}-byte keys at +{}, step {}, last +{}",
                  relation, relationName(relation), sectors, sector, keyLength, firstKey, step, lastKey);

    uint8_t image[DiskBackend::kSectorBytes];
    for (int s = 0; s < sectors; ++s) {
        volume_.readSector(sector + s, image);
        ++sectorsRead_;
        for (int p = firstKey; p <= lastKey && p + keyLength <= DiskBackend::kSectorBytes; p += step) {
            const int cmp = compare(image, p, argument);
            const bool hit = relation == IoBlock::kScanEqual ? cmp == 0
                           : relation == IoBlock::kScanLowOrEqual ? cmp <= 0 : cmp >= 0;
            if (!hit) continue;

            // The RIGHTMOST byte of the matching key, which is how the
            // System/36 names every multi-byte field.
            m_.writeByte(iob + IoBlock::kOffScanHit, static_cast<uint8_t>(p + keyLength - 1));
            // The work sector, 1-based, naming the sector that hit.
            m_.writeAddr24(iob + IoBlock::kOffDiskSectorWork, sector + s + 1);
            // The whole sector, over the search argument that was there.
            std::vector<uint8_t> whole(image, image + DiskBackend::kSectorBytes);
            writeExtents(bufferExtents, whole);

            // 0x44 when the hit was EXACTLY equal, 0x40 otherwise: the
            // hardware's own SCAN HIT and SCAN EQUAL status bits.
            const int code = cmp == 0 ? 4 : 0;
            trace_.diskIo("  scan HIT in sector {} at +{} (key ends +{}) -> buffer {:06X}, completion {:02X} - {}",
                          sector + s, p, p + keyLength - 1, buffer, 0x40 | code,
                          cmp == 0 ? "EQUAL" : "not equal, the relation was satisfied");
            IoBlock::complete(m_, iob, code);
            return true;
        }
    }

    // No hit is code 2, and it is NOT an error.  The buffer keeps the argument.
    m_.writeAddr24(iob + IoBlock::kOffDiskSectorWork, sector + sectors);
    trace_.diskIo("  scan found no key {} the argument in {} sector(s) - completion 2",
                  relation == IoBlock::kScanEqual ? "equal to"
                  : relation == IoBlock::kScanLowOrEqual ? "at or below" : "at or above", sectors);
    IoBlock::complete(m_, iob, 2);
    return true;
}

const char* VirtualFixedDisk::relationName(int relation)
{
    return relation == IoBlock::kScanEqual ? "equal"
         : relation == IoBlock::kScanLowOrEqual ? "low or equal"
         : relation == IoBlock::kScanHighOrEqual ? "high or equal" : "ID";
}

// Unsigned byte comparison: EBCDIC collating order is the byte order.
int VirtualFixedDisk::compare(const uint8_t* image, int at, const std::vector<uint8_t>& argument)
{
    for (std::size_t i = 0; i < argument.size(); ++i) {
        const int d = static_cast<int>(image[static_cast<std::size_t>(at) + i]) - argument[i];
        if (d != 0) return d;
    }
    return 0;
}

// Command A2, write.  Modifier C0 is a FILL (byte 13 is the fill character);
// modifier 80 is data field WRAP (one 256-byte area is the source of every
// sector); anything else writes the buffer.
bool VirtualFixedDisk::write(int iob, int sector, int sectors, int modifier, int bufferField)
{
    std::vector<uint8_t> buffer(static_cast<std::size_t>(sectors) * DiskBackend::kSectorBytes);

    if (modifier == IoBlock::kModifierFill) {
        const uint8_t fill = static_cast<uint8_t>(bufferField >> 16);   // +0x0D, the first of the three
        std::fill(buffer.begin(), buffer.end(), fill);
        trace_.diskIo("  fill {} sector(s) at {} with {:02X}", sectors, sector, fill);
    } else if (modifier == IoBlock::kModifierWrap) {
        // Only ONE sector of guest storage is read, whatever the count, so a
        // task-translated buffer at the end of a mapped page is legal here.
        Extents src;
        if (!resolveBufferExtents(bufferField, DiskBackend::kSectorBytes, src) || !extentsInStorage(src, bufferField)) {
            IoBlock::complete(m_, iob, 4);
            return false;
        }
        std::vector<uint8_t> one(DiskBackend::kSectorBytes);
        readExtents(src, one);
        for (int i = 0; i < sectors; ++i)
            std::memcpy(buffer.data() + static_cast<std::size_t>(i) * DiskBackend::kSectorBytes, one.data(),
                        DiskBackend::kSectorBytes);
        trace_.diskIo("  write {} sector(s) at {}, data field WRAP: the one 256-byte "
                      "area at guest {:06X} is written to every sector (executeInternal "
                      "c1864f38/c1864f7c; SA21-9243-4 6-4 part 6 bit 0)", sectors, sector, src[0].first);
    } else {
        Extents src;
        if (!resolveBufferExtents(bufferField, static_cast<int>(buffer.size()), src) || !extentsInStorage(src, bufferField)) {
            IoBlock::complete(m_, iob, 4);
            return false;
        }
        readExtents(src, buffer);
        trace_.diskIo("  write {} sector(s) at {} from guest {:06X}", sectors, sector, src[0].first);
    }

    if (volume_.readOnly() && !volume_.isOverlay()) {
        trace_.diskIo("  REFUSED - {} is read-only. `make scratch` copies it to "
                      "var/as36.scratch.img; point volume= at that and set "
                      "volume_readonly = no. The extracted image is never written.", volume_.path());
        IoBlock::complete(m_, iob, 4);
        return false;
    }

    for (int i = 0; i < sectors; ++i)
        volume_.writeSector(sector + i, buffer.data() + static_cast<std::size_t>(i) * DiskBackend::kSectorBytes);

    ++writesIssued_;
    sectorsWritten_ += sectors;
    advanceWorkSector(iob, sector, sectors);
    IoBlock::complete(m_, iob, 0);
    return true;
}

// Resolve the data buffer field, honouring the translated flag.  A real
// buffer is one extent; a task-translated buffer is resolved page by page
// through the task's translation registers.
bool VirtualFixedDisk::resolveBufferExtents(int bufferField, int length, Extents& extents)
{
    extents.clear();
    if ((bufferField & IoBlock::kDataBufferTranslated) == 0) {
        extents.emplace_back(bufferField & 0x7FFFFF, length);
        return true;
    }
    if (!m_.translatedExtents(static_cast<uint16_t>(bufferField), machine::MachineState::kAtrTaskGroup0, length, true,
                              extents)) {
        trace_.diskIo("  buffer {:06X} is task-translated and a page of the {}-byte transfer is not mapped",
                      bufferField, length);
        return false;
    }
    if (extents.size() == 1) {
        trace_.diskIo("  buffer {:06X} is task-translated -> real {:06X}", bufferField, extents[0].first);
    } else {
        std::string parts;
        for (std::size_t i = 0; i < extents.size(); ++i) {
            if (i != 0) parts += ", ";
            parts += fmt::format("{:06X}+{}", extents[i].first, extents[i].second);
        }
        trace_.diskIo("  buffer {:06X} is task-translated and the {}-byte transfer spans {} translation register(s) -> real {}",
                      bufferField, length, extents.size(), parts);
    }
    return true;
}

// Every extent must be inside main storage before any of them is touched.
bool VirtualFixedDisk::extentsInStorage(const Extents& extents, int bufferField)
{
    for (const auto& e : extents) {
        if (e.first < 0 || !m_.inRange(e.first, e.second)) {
            trace_.diskIo("  buffer {:06X} extent {:06X}+{} outside main storage", bufferField, e.first, e.second);
            return false;
        }
    }
    return true;
}

void VirtualFixedDisk::writeExtents(const Extents& extents, const std::vector<uint8_t>& data)
{
    int off = 0;
    for (const auto& e : extents) {
        m_.write(e.first, data.data() + off, e.second);
        off += e.second;
    }
}

VirtualFixedDisk::Extents VirtualFixedDisk::resolveArgumentExtents(const Extents& extents, int length)
{
    Extents head;
    int left = length;
    for (const auto& e : extents) {
        if (left <= 0) break;
        const int n = e.second < left ? e.second : left;
        head.emplace_back(e.first, n);
        left -= n;
    }
    return head;
}

void VirtualFixedDisk::readExtents(const Extents& extents, std::vector<uint8_t>& data)
{
    int off = 0;
    for (const auto& e : extents) {
        m_.read(e.first, data.data() + off, e.second);
        off += e.second;
    }
}

}  // namespace sim36::devices
