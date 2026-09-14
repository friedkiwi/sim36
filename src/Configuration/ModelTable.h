// Machine identity and control-storage processor implementation are separate
// choices.  A virtual CSP may expose a physical model's SSP-visible identity
// even though that pairing could not exist as IBM hardware.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sim36::configuration {

// How a control storage processor is provided for a model.
enum class CspKind {
    Virtual,     // the architected contract in native code; still a CSP
    Microcode,   // interprets real CSP microcode for the model and its stage
};

const char* cspKindName(CspKind kind);   // "virtual" / "microcode"

class ModelTable {
public:
    static bool isKnown(const std::string& model);
    static std::vector<std::string> known();
    static int maxStorageKb(const std::string& model);
    static uint8_t systemCustomize1(const std::string& model);
    static std::string noteFor(const std::string& model);
};

class CspTypeTable {
public:
    static bool isKnown(const std::string& type);
    static std::vector<std::string> known();
    static CspKind kindOf(const std::string& type);
    static std::string variantOf(const std::string& type);
    static bool supportsModel(const std::string& type, const std::string& model);
};

}  // namespace sim36::configuration
