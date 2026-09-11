// The machine models, and what each implies.
//
// Model is the user-facing knob; CSP kind, CSP variant and the storage
// ceiling all derive from it.  There are more CSP variants than machine types
// because the 5360 alone spans three documented processor stages, and Stage 3
// adds base-plus-displacement instructions the earlier ones lack, so "5360"
// without a stage does not name a processor.
#pragma once

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
    static CspKind kindOf(const std::string& model);
    static std::string variantOf(const std::string& model);
    static int maxStorageKb(const std::string& model);
    static std::string noteFor(const std::string& model);
};

}  // namespace sim36::configuration
