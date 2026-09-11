#include "Configuration/ModelTable.h"

#include <fmt/format.h>

#include "Configuration/ConfigError.h"
#include "Monitor/CommandRegistry.h"

namespace sim36::configuration {

namespace {

struct Entry {
    const char* name;
    CspKind kind;
    const char* variant;
    int maxStorageKb;   // what the model can describe to itself
    const char* note;
};

// Storage ceilings follow the address-compare width and the CCR size codes:
// 19 bits on the 5360, 20 on the 5362 and 5364, 21 on the 5360 Model D.
const std::vector<Entry>& models()
{
    static const std::vector<Entry> table = {
        {"advanced36", CspKind::Virtual, "virtual", 1024,
         "the configuration IBM shipped; volume's control storage library is empty"},
        {"5360-s1", CspKind::Microcode, "5360-s1", 512, "original processor, MIM SY31-9000; not digitised"},
        {"5360-s2", CspKind::Microcode, "5360-s2", 512, "Stage 2, MIM SY31-9015; not digitised"},
        {"5360-s3", CspKind::Microcode, "5360-s3", 512,
         "Stage 3, MIM SY31-9035, DIAG41-44; adds base-plus-displacement"},
        {"5360-d", CspKind::Microcode, "5360-d", 2048, "Model D: 21-bit ACR, MCR gives size per card slot"},
        {"5362", CspKind::Microcode, "5362", 1024, "own DSKT01-03; the name collides with the 5360's"},
        {"5363", CspKind::Microcode, "5363", 1024, "DSKT60-62"},
        {"5364", CspKind::Microcode, "5364", 1024, "DSKT11-12; PC-attached, ARTIC coprocessor comms"},
    };
    return table;
}

const Entry* findEntry(const std::string& model)
{
    for (const Entry& e : models())
        if (monitor::equalsIgnoreCase(model, e.name)) return &e;
    return nullptr;
}

const Entry& get(const std::string& model)
{
    const Entry* e = findEntry(model);
    if (e != nullptr) return *e;
    std::string names;
    for (const Entry& m : models()) {
        if (!names.empty()) names += ", ";
        names += m.name;
    }
    throw ConfigError("model", 0, fmt::format("unknown model '{}'; known models are {}", model, names));
}

}  // namespace

const char* cspKindName(CspKind kind) { return kind == CspKind::Virtual ? "virtual" : "microcode"; }

bool ModelTable::isKnown(const std::string& model) { return findEntry(model) != nullptr; }

std::vector<std::string> ModelTable::known()
{
    std::vector<std::string> names;
    for (const Entry& e : models()) names.emplace_back(e.name);
    return names;
}

CspKind ModelTable::kindOf(const std::string& model) { return get(model).kind; }
std::string ModelTable::variantOf(const std::string& model) { return get(model).variant; }
int ModelTable::maxStorageKb(const std::string& model) { return get(model).maxStorageKb; }
std::string ModelTable::noteFor(const std::string& model) { return get(model).note; }

}  // namespace sim36::configuration
