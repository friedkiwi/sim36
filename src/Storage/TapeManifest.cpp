#include "Storage/TapeManifest.h"

#include <cctype>
#include <cstdio>
#include <exception>

#include "Storage/Ebcdic.h"
#include "Storage/FolderTapeBackend.h"

namespace sim36::storage {

using nlohmann::ordered_json;

namespace {

// The manifest's own serialisation: two-space indentation, `"key": value`,
// arrays of scalars on one line, arrays of objects broken out one per line,
// empty objects and arrays as `{}` and `[]`, and a trailing newline.  This
// is the reference writer's layout, which the generic pretty printer does
// not reproduce (it breaks scalar arrays out one element per line), so the
// tree is walked by hand.
void indent(std::string& sb, int n) { sb.append(static_cast<std::size_t>(n) * 2, ' '); }

void writeString(std::string& sb, const std::string& s)
{
    sb += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': sb += "\\\""; break;
            case '\\': sb += "\\\\"; break;
            case '\b': sb += "\\b"; break;
            case '\f': sb += "\\f"; break;
            case '\n': sb += "\\n"; break;
            case '\r': sb += "\\r"; break;
            case '\t': sb += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    sb += buf;
                } else {
                    sb += static_cast<char>(c);
                }
                break;
        }
    }
    sb += '"';
}

void writeValue(std::string& sb, const ordered_json& v, int level);

void writeObject(std::string& sb, const ordered_json& o, int level)
{
    if (o.empty()) {
        sb += "{}";
        return;
    }
    sb += "{\n";
    std::size_t i = 0;
    for (auto it = o.begin(); it != o.end(); ++it) {
        indent(sb, level + 1);
        writeString(sb, it.key());
        sb += ": ";
        writeValue(sb, it.value(), level + 1);
        if (++i < o.size()) sb += ',';
        sb += '\n';
    }
    indent(sb, level);
    sb += '}';
}

void writeArray(std::string& sb, const ordered_json& a, int level)
{
    if (a.empty()) {
        sb += "[]";
        return;
    }
    bool scalarOnly = true;
    for (const auto& item : a)
        if (item.is_object() || item.is_array()) {
            scalarOnly = false;
            break;
        }
    if (scalarOnly) {
        sb += '[';
        for (std::size_t i = 0; i < a.size(); i++) {
            if (i > 0) sb += ", ";
            writeValue(sb, a[i], level);
        }
        sb += ']';
        return;
    }
    sb += "[\n";
    for (std::size_t i = 0; i < a.size(); i++) {
        indent(sb, level + 1);
        writeValue(sb, a[i], level + 1);
        if (i + 1 < a.size()) sb += ',';
        sb += '\n';
    }
    indent(sb, level);
    sb += ']';
}

void writeValue(std::string& sb, const ordered_json& v, int level)
{
    if (v.is_null()) sb += "null";
    else if (v.is_string()) writeString(sb, v.get<std::string>());
    else if (v.is_boolean()) sb += v.get<bool>() ? "true" : "false";
    else if (v.is_number_integer() || v.is_number_unsigned()) sb += std::to_string(v.get<long long>());
    else if (v.is_number_float()) sb += v.dump();
    else if (v.is_object()) writeObject(sb, v, level);
    else if (v.is_array()) writeArray(sb, v, level);
}

std::string str(const ordered_json& o, const char* key, const std::string& fallback)
{
    auto it = o.find(key);
    if (it != o.end() && it->is_string()) return it->get<std::string>();
    return fallback;
}

long long integer(const ordered_json& o, const char* key, long long fallback)
{
    auto it = o.find(key);
    if (it != o.end() && !it->is_null()) {
        if (it->is_number_integer() || it->is_number_unsigned()) return it->get<long long>();
        if (it->is_number_float()) return static_cast<long long>(it->get<double>());
    }
    return fallback;
}

bool boolean(const ordered_json& o, const char* key, bool fallback)
{
    auto it = o.find(key);
    if (it != o.end() && it->is_boolean()) return it->get<bool>();
    return fallback;
}

std::string trimEnd(std::string s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    return s;
}

std::string lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

// ---- TapeVolume --------------------------------------------------------------

ordered_json TapeVolume::toJson() const
{
    ordered_json d = ordered_json::object();
    d["volumeId"] = volumeId;
    d["accessSecurity"] = accessSecurity;
    d["ownerId"] = ownerId;
    if (!density.empty()) d["density"] = density;
    d["labeled"] = labeled;
    return d;
}

TapeVolume TapeVolume::fromJson(const ordered_json& o)
{
    TapeVolume v;
    v.volumeId = str(o, "volumeId", "S36VOL");
    v.accessSecurity = str(o, "accessSecurity", " ");
    v.ownerId = str(o, "ownerId", "");
    v.density = str(o, "density", "");
    v.labeled = boolean(o, "labeled", true);
    return v;
}

// ---- TapeFileEntry ---------------------------------------------------------------

std::vector<int> TapeFileEntry::resolveBlockLengths() const
{
    if (hasBlockLengths) return blockLengths;
    return std::vector<int>(static_cast<std::size_t>(blockCount), blockLength);
}

ordered_json TapeFileEntry::toJson() const
{
    ordered_json d = ordered_json::object();
    d["sequence"] = sequence;
    d["kind"] = kind;
    d["blob"] = blob;
    if (hasBlockLengths) {
        d["blockLengths"] = blockLengths;
        d["blockCount"] = static_cast<long long>(blockLengths.size());
    } else {
        d["blockLength"] = blockLength;
        d["blockCount"] = blockCount;
    }
    d["recordFormat"] = recordFormat;
    d["recordLength"] = recordLength;
    if (!labels.empty()) d["labels"] = labels;
    return d;
}

TapeFileEntry TapeFileEntry::fromJson(const ordered_json& o)
{
    TapeFileEntry f;
    f.sequence = static_cast<int>(integer(o, "sequence", 0));
    f.kind = str(o, "kind", "data");
    f.blob = str(o, "blob", "");
    f.blockLength = static_cast<int>(integer(o, "blockLength", 0));
    f.blockCount = static_cast<int>(integer(o, "blockCount", 0));
    f.recordFormat = str(o, "recordFormat", "U");
    f.recordLength = static_cast<int>(integer(o, "recordLength", 0));

    auto bl = o.find("blockLengths");
    if (bl != o.end() && bl->is_array()) {
        f.hasBlockLengths = true;
        f.blockLengths.clear();
        for (const auto& n : *bl) {
            if (n.is_number_float()) f.blockLengths.push_back(static_cast<int>(n.get<double>()));
            else f.blockLengths.push_back(static_cast<int>(n.get<long long>()));
        }
        f.blockCount = static_cast<int>(f.blockLengths.size());
    }

    auto labels = o.find("labels");
    if (labels != o.end() && labels->is_object())
        for (auto it = labels->begin(); it != labels->end(); ++it) f.labels[it.key()] = it.value();
    return f;
}

// ---- TapeManifest ----------------------------------------------------------------

std::string TapeManifest::write() const
{
    ordered_json root = ordered_json::object();
    root["format"] = FolderTapeBackend::kFormatId;
    root["formatVersion"] = FolderTapeBackend::kFormatVersion;
    root["volume"] = volume.toJson();
    ordered_json list = ordered_json::array();
    for (const TapeFileEntry& f : files) list.push_back(f.toJson());
    root["files"] = list;

    std::string sb;
    writeValue(sb, root, 0);
    sb += '\n';
    return sb;
}

std::unique_ptr<TapeManifest> TapeManifest::read(const std::string& text, std::string& reason)
{
    reason.clear();
    try {
        ordered_json root = ordered_json::parse(text);
        if (!root.is_object()) throw std::runtime_error("expected a JSON object");

        std::string format = str(root, "format", "");
        bool present = root.contains("format") && root["format"].is_string();
        if (!present || format != FolderTapeBackend::kFormatId) {
            reason = "manifest format is '" + (present ? format : std::string("<missing>")) + "', expected '" +
                     FolderTapeBackend::kFormatId + "'";
            return nullptr;
        }
        auto version = root.find("formatVersion");
        if (version == root.end() || (!version->is_number_integer() && !version->is_number_unsigned()) ||
            version->get<long long>() != FolderTapeBackend::kFormatVersion) {
            reason = "manifest formatVersion is not supported (expected " +
                     std::to_string(FolderTapeBackend::kFormatVersion) + ")";
            return nullptr;
        }

        auto m = std::make_unique<TapeManifest>();
        auto vol = root.find("volume");
        if (vol == root.end() || !vol->is_object()) throw std::runtime_error("volume must be a JSON object");
        m->volume = TapeVolume::fromJson(*vol);
        auto files = root.find("files");
        if (files == root.end() || !files->is_array()) throw std::runtime_error("files must be a JSON array");
        for (const auto& f : *files) {
            if (!f.is_object()) throw std::runtime_error("each file must be a JSON object");
            if (!f.contains("sequence") || (!f["sequence"].is_number_integer() && !f["sequence"].is_number_unsigned()))
                throw std::runtime_error("each file must declare an integer sequence");
            if (!f.contains("blob") || !f["blob"].is_string())
                throw std::runtime_error("each file must declare a string blob path");
            bool variable = f.contains("blockLengths");
            bool fixed = f.contains("blockLength");
            if (variable == fixed) throw std::runtime_error("each file must declare exactly one block length form");
            if (!f.contains("blockCount") || (!f["blockCount"].is_number_integer() && !f["blockCount"].is_number_unsigned()))
                throw std::runtime_error("each file must declare an integer blockCount");
            if (f["blockCount"].get<long long>() < 0) throw std::runtime_error("blockCount cannot be negative");
            if (fixed && !f["blockLength"].is_number_integer() && !f["blockLength"].is_number_unsigned())
                throw std::runtime_error("blockLength must be an integer");
            if (variable && (!f["blockLengths"].is_array() ||
                             f["blockCount"].get<long long>() != static_cast<long long>(f["blockLengths"].size())))
                throw std::runtime_error("blockCount does not match blockLengths");
            if (variable)
                for (const auto& length : f["blockLengths"])
                    if (!length.is_number_integer() && !length.is_number_unsigned())
                        throw std::runtime_error("every blockLengths entry must be an integer");
            m->files.push_back(TapeFileEntry::fromJson(f));
        }
        return m;
    } catch (const std::exception& e) {   // a malformed manifest is a reason, not a crash
        reason = std::string("manifest is not valid: ") + e.what();
        return nullptr;
    }
}

// ---- TapeLabel --------------------------------------------------------------------

std::string TapeLabel::normaliseVolumeId(const std::string& volumeId)
{
    std::size_t a = 0, b = volumeId.size();
    while (a < b && volumeId[a] == ' ') a++;
    while (b > a && volumeId[b - 1] == ' ') b--;
    std::string v = volumeId.substr(a, b - a);
    if (v.empty()) v = "S36VOL";
    if (v.size() > 6) v = v.substr(0, 6);
    return v;
}

std::vector<uint8_t> TapeLabel::renderVol1(const std::string& volumeId, char access, const std::string& ownerId)
{
    std::vector<uint8_t> b(static_cast<std::size_t>(FolderTapeBackend::kLabelLength), kEbcdicSpace);
    putEbcdic(b, 0, "VOL1", 4);
    putEbcdic(b, 4, normaliseVolumeId(volumeId), 6);
    b[10] = Ebcdic::fromChar(static_cast<uint32_t>(static_cast<unsigned char>(access == '\0' ? ' ' : access)),
                             Ebcdic::CodePage::Cp037);
    putEbcdic(b, 37, ownerId, 14);
    return b;
}

ordered_json TapeLabel::decodeVol1(const std::vector<uint8_t>& b, std::size_t at)
{
    ordered_json d = ordered_json::object();
    d["volumeId"] = get(b, at + 4, 6);
    d["accessSecurity"] = get(b, at + 10, 1);
    d["ownerId"] = get(b, at + 37, 14);
    return d;
}

ordered_json TapeLabel::decodeDataSet1(const std::vector<uint8_t>& b, std::size_t at)
{
    ordered_json d = ordered_json::object();
    d["labelId"] = get(b, at + 0, 4);
    d["dataSetId"] = get(b, at + 4, 17);
    d["dataSetSequenceNumber"] = get(b, at + 31, 4);
    d["blockCount"] = get(b, at + 54, 6);
    return d;
}

bool TapeLabel::decodeLabelGroup(const std::vector<std::vector<uint8_t>>& records, ordered_json& into)
{
    if (records.empty()) return false;
    std::string firstId = get(records[0], 0, 4);
    if (!isKnownLabelId(firstId)) return false;

    for (const auto& rec : records) {
        std::string id = get(rec, 0, 4);
        if (!isKnownLabelId(id)) continue;
        if (id == "VOL1") into[lower(id)] = decodeVol1(rec, 0);
        else if (id == "HDR2" || id == "EOF2" || id == "EOV2") into[lower(id)] = decodeDataSet2Brief(rec, 0);
        else into[lower(id)] = decodeDataSet1(rec, 0);
    }
    return !into.empty();
}

bool TapeLabel::isKnownLabelId(const std::string& id)
{
    return id == "VOL1" || id == "HDR1" || id == "HDR2" || id == "EOF1" || id == "EOF2" || id == "EOV1" || id == "EOV2";
}

// The HDR2/EOF2/EOV2 fields this format cares about: the record format at +4
// and the block length at +5 (5).
ordered_json TapeLabel::decodeDataSet2Brief(const std::vector<uint8_t>& b, std::size_t at)
{
    ordered_json d = ordered_json::object();
    d["labelId"] = get(b, at + 0, 4);
    d["recordFormat"] = get(b, at + 4, 1);
    d["blockLength"] = get(b, at + 5, 5);
    return d;
}

void TapeLabel::putEbcdic(std::vector<uint8_t>& b, std::size_t at, const std::string& s, std::size_t len)
{
    std::string text = s.size() > len ? s.substr(0, len) : s;
    std::vector<uint8_t> enc = Ebcdic::fromAscii(text, Ebcdic::CodePage::Cp037);
    for (std::size_t i = 0; i < len; i++) b[at + i] = i < enc.size() ? enc[i] : kEbcdicSpace;
}

std::string TapeLabel::get(const std::vector<uint8_t>& b, std::size_t at, std::size_t len)
{
    if (at + len > b.size()) len = at < b.size() ? b.size() - at : 0;
    return trimEnd(Ebcdic::toAscii(b.data() + at, len, Ebcdic::CodePage::Cp037));
}

}  // namespace sim36::storage
