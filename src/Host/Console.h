// Operator line input.  Interactive terminals get line editing, in-session
// history and tab completion through replxx; redirected input is read
// verbatim so that a scripted session produces exactly the bytes a diff
// expects.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace sim36::host {

class Console {
public:
    // Vocabulary for the word under the cursor given the whitespace-separated
    // words before it: keyword candidates, and whether host file names are
    // also acceptable there (the console then adds matching directory
    // entries).  Candidates need not be filtered by the partial word.
    struct Completion {
        std::vector<std::string> words;
        bool paths = false;
    };
    using Completer = std::function<Completion(const std::vector<std::string>& preceding)>;

    Console();
    ~Console();
    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    // Installs the tab-completion vocabulary; no-op for redirected input.
    void setCompleter(Completer completer);

    // Prints the prompt and reads one line; nullopt at end of input.
    std::optional<std::string> readLine(const std::string& prompt);

    // True when standard input is a terminal.
    static bool isInteractive();

    // Candidate replacements for the partial last word of `input`: the
    // completer's keywords and, when it allows paths, directory entries,
    // both filtered by that word (keywords case-insensitively).  Exposed for
    // tests; readLine feeds it to replxx.
    static std::vector<std::string> candidates(const std::string& input, const Completer& completer);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace sim36::host
