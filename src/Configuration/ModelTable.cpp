#include "Configuration/ModelTable.h"

#include <fmt/format.h>

#include "Configuration/ConfigError.h"
#include "Monitor/CommandRegistry.h"

namespace sim36::configuration {

namespace {

struct Entry {
    const char* name;
    int maxStorageKb;   // what the model can describe to itself
    int systemCustomize1;  // blank-disk UDT customize[0], or -1 when unsupported
    const char* note;
};

struct CspEntry {
    const char* name;
    CspKind kind;
    const char* variant;
};

// Storage ceilings follow the address-compare width and the CCR size codes:
// 19 bits on the 5360, 20 on the 5362 and 5364, 21 on the 5360 Model D.
const std::vector<Entry>& models()
{
    static const std::vector<Entry> table = {
        {"advanced36", 1024, 0x8D,
         "the configuration IBM shipped; volume's control storage library is empty"},
        {"5360-s1", 512, -1, "original processor, MIM SY31-9000; not digitised"},
        {"5360-s2", 512, -1, "Stage 2, MIM SY31-9015; not digitised"},
        {"5360-s3", 512, -1,
         "Stage 3, MIM SY31-9035, DIAG41-44; adds base-plus-displacement"},
        {"5360-d", 2048, -1, "Model D: 21-bit ACR, MCR gives size per card slot"},
        {"5362", 1024, -1, "own DSKT01-03; the name collides with the 5360's"},
        {"5363", 1024, 0x8B, "DSKT60-62"},
        {"5364", 1024, 0x8D, "DSKT11-12; PC-attached, ARTIC coprocessor comms"},
    };
    return table;
}

const std::vector<CspEntry>& cspTypes()
{
    static const std::vector<CspEntry> table = {
        {"advanced36", CspKind::Virtual, "virtual"},
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

const CspEntry* findCspEntry(const std::string& type)
{
    for (const CspEntry& e : cspTypes())
        if (monitor::equalsIgnoreCase(type, e.name)) return &e;
    return nullptr;
}

const CspEntry& getCsp(const std::string& type)
{
    const CspEntry* e = findCspEntry(type);
    if (e != nullptr) return *e;
    std::string names;
    for (const CspEntry& c : cspTypes()) {
        if (!names.empty()) names += ", ";
        names += c.name;
    }
    throw ConfigError("csp_type", 0, fmt::format("unknown CSP type '{}'; known types are {}", type, names));
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

int ModelTable::maxStorageKb(const std::string& model) { return get(model).maxStorageKb; }
uint8_t ModelTable::systemCustomize1(const std::string& model)
{
    const Entry& e = get(model);
    if (e.systemCustomize1 < 0)
        throw ConfigError("model", 0, fmt::format("model {} has no SSP customization mapping", model));
    return static_cast<uint8_t>(e.systemCustomize1);
}
std::string ModelTable::noteFor(const std::string& model) { return get(model).note; }

bool CspTypeTable::isKnown(const std::string& type) { return findCspEntry(type) != nullptr; }

std::vector<std::string> CspTypeTable::known()
{
    std::vector<std::string> names;
    for (const CspEntry& e : cspTypes()) names.emplace_back(e.name);
    return names;
}

CspKind CspTypeTable::kindOf(const std::string& type) { return getCsp(type).kind; }
std::string CspTypeTable::variantOf(const std::string& type) { return getCsp(type).variant; }

bool CspTypeTable::supportsModel(const std::string& type, const std::string& model)
{
    return monitor::equalsIgnoreCase(type, "advanced36") &&
           (monitor::equalsIgnoreCase(model, "advanced36") || monitor::equalsIgnoreCase(model, "5363") ||
            monitor::equalsIgnoreCase(model, "5364"));
}

}  // namespace sim36::configuration
