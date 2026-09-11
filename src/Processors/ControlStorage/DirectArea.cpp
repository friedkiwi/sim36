#include "Processors/ControlStorage/DirectArea.h"

#include <algorithm>

namespace sim36::processors::controlstorage {

std::vector<int> DirectArea::captureCheckpoint() const
{
    std::vector<int> values;
    for (int i = 0; i < kWords; i++)
        if (written_[i]) {
            values.push_back(i);
            values.push_back(low_[i]);
            values.push_back(high_[i]);
        }
    return values;
}

bool DirectArea::restoreCheckpoint(const std::vector<int>& values)
{
    std::fill(std::begin(low_), std::end(low_), static_cast<uint16_t>(0));
    std::fill(std::begin(high_), std::end(high_), static_cast<uint8_t>(0));
    std::fill(std::begin(written_), std::end(written_), false);
    if (values.size() % 3 != 0) return false;
    for (size_t i = 0; i < values.size(); i += 3) {
        int w = values[i];
        if (w < 0 || w >= kWords) return false;
        low_[w] = static_cast<uint16_t>(values[i + 1]);
        high_[w] = static_cast<uint8_t>(values[i + 2]);
        written_[w] = true;
    }
    return true;
}

std::string DirectArea::describe(int w)
{
    if (w == kTransferControlTable) return "the system transfer control table base, NuEmul[0x448]";
    if (w == kIplSource)
        return "the RELOAD SOURCE - its low byte becomes guest 0x08A9, and phase 1 "
               "reads bits 0x1C of that at 0x146A: zero means DISKETTE (SVC 41), "
               "any bit set means TAPE (SVC 46). Only read when a reload was asked "
               "for; a disk IPL never gets here";
    return "not identified with a NuEmul field";
}

}  // namespace sim36::processors::controlstorage
