#include "Host/Console.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <system_error>

#include <replxx.hxx>

#ifdef _WIN32
#include <io.h>
#define SIM36_ISATTY _isatty
#define SIM36_FILENO _fileno
#else
#include <unistd.h>
#define SIM36_ISATTY isatty
#define SIM36_FILENO fileno
#endif

namespace sim36::host {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t'; }

std::filesystem::path environmentPath(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::filesystem::path(value) : std::filesystem::path();
}

// Follow each platform's per-user state convention.  History is deliberately
// not stored beside the executable or in the current directory: both may be
// read-only, and separate checkout directories should share operator history.
std::filesystem::path historyPath()
{
#ifdef _WIN32
    std::filesystem::path root = environmentPath("LOCALAPPDATA");
    if (root.empty()) {
        root = environmentPath("USERPROFILE");
        if (!root.empty()) root /= "AppData/Local";
    }
#elif defined(__APPLE__)
    std::filesystem::path root = environmentPath("HOME");
    if (!root.empty()) root /= "Library/Application Support";
#else
    std::filesystem::path root = environmentPath("XDG_STATE_HOME");
    if (root.empty()) {
        root = environmentPath("HOME");
        if (!root.empty()) root /= ".local/state";
    }
#endif
    return root.empty() ? root : root / "sim36" / "history";
}

bool startsWithIgnoreCase(const std::string& word, const std::string& prefix)
{
    if (prefix.size() > word.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(word[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    return true;
}

// Directory entries whose path begins with `prefix`; directories get a
// trailing separator so a second tab descends into them.
void pathCandidates(const std::string& prefix, std::vector<std::string>& out)
{
    namespace fs = std::filesystem;
    const std::size_t slash = prefix.find_last_of("/\\");
    const std::string dirPart = slash == std::string::npos ? "" : prefix.substr(0, slash + 1);
    const std::string namePart = slash == std::string::npos ? prefix : prefix.substr(slash + 1);
    std::error_code ec;
    fs::directory_iterator it(dirPart.empty() ? fs::path(".") : fs::path(dirPart), ec);
    if (ec) return;
    std::vector<std::string> found;
    for (const fs::directory_entry& entry : it) {
        const std::string name = entry.path().filename().string();
        if (name.compare(0, namePart.size(), namePart) != 0) continue;
        if (namePart.empty() && !name.empty() && name[0] == '.') continue;
        std::string candidate = dirPart + name;
        if (entry.is_directory(ec)) candidate += '/';
        found.push_back(std::move(candidate));
    }
    std::sort(found.begin(), found.end());
    out.insert(out.end(), found.begin(), found.end());
}

}  // namespace

struct Console::Impl {
    replxx::Replxx rx;
    Completer completer;
    std::filesystem::path historyFile;
};

Console::Console() : impl_(isInteractive() ? new Impl() : nullptr)
{
    if (impl_ == nullptr) return;
    // Only blanks split words: hyphenated commands (listener-auto-signon),
    // dotted station ids and file paths must complete as one word.
    impl_->rx.set_word_break_characters(" \t");
    impl_->rx.set_max_history_size(1000);
    impl_->rx.set_unique_history(true);
    impl_->historyFile = historyPath();
    if (!impl_->historyFile.empty())
        impl_->rx.history_load(impl_->historyFile.string());
}

Console::~Console()
{
    if (impl_ != nullptr && !impl_->historyFile.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(impl_->historyFile.parent_path(), ec);
        if (!ec) impl_->rx.history_save(impl_->historyFile.string());
    }
    delete impl_;
}

bool Console::isInteractive()
{
    return SIM36_ISATTY(SIM36_FILENO(stdin)) != 0;
}

void Console::setCompleter(Completer completer)
{
    if (impl_ == nullptr) return;
    impl_->completer = std::move(completer);
    impl_->rx.set_completion_callback(
        [this](const std::string& input, int& contextLen) {
            replxx::Replxx::completions_t out;
            const std::vector<std::string> found = candidates(input, impl_->completer);
            std::size_t start = input.size();
            while (start > 0 && !isSpace(input[start - 1])) --start;
            contextLen = static_cast<int>(input.size() - start);
            for (const std::string& c : found) out.emplace_back(c);
            return out;
        });
}

std::vector<std::string> Console::candidates(const std::string& input, const Completer& completer)
{
    std::vector<std::string> preceding;
    std::string word;
    for (char c : input) {
        if (isSpace(c)) {
            if (!word.empty()) preceding.push_back(word);
            word.clear();
        } else {
            word += c;
        }
    }
    std::vector<std::string> out;
    if (!completer) return out;
    const Completion vocabulary = completer(preceding);
    for (const std::string& w : vocabulary.words)
        if (startsWithIgnoreCase(w, word)) out.push_back(w);
    if (vocabulary.paths) pathCandidates(word, out);
    return out;
}

std::optional<std::string> Console::readLine(const std::string& prompt)
{
    if (impl_ != nullptr) {
        const char* line = impl_->rx.input(prompt);
        if (line == nullptr) return std::nullopt;
        std::string result(line);
        if (!result.empty()) impl_->rx.history_add(result);
        return result;
    }
    std::fputs(prompt.c_str(), stdout);
    std::fflush(stdout);
    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

}  // namespace sim36::host
