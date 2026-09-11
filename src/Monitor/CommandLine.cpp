#include "Monitor/CommandLine.h"

#include <cctype>

namespace sim36::monitor {

std::vector<std::string> CommandLine::tokenize(const std::string& line)
{
    std::size_t first = 0;
    while (first < line.size() && std::isspace(static_cast<unsigned char>(line[first]))) ++first;
    if (first == line.size() || line[first] == '#' || line[first] == ';') return {};

    std::vector<std::string> words;
    std::string word;
    bool quoted = false;
    char quote = '\0';
    bool escaped = false;
    // Length alone cannot distinguish no argument from an explicitly quoted
    // empty argument.
    bool wordStarted = false;

    for (char c : line) {
        if (escaped) {
            word.push_back(c);
            wordStarted = true;
            escaped = false;
            continue;
        }
        if (c == '\\') {
            wordStarted = true;
            escaped = true;
            continue;
        }
        if (quoted) {
            if (c == quote) quoted = false;
            else word.push_back(c);
            continue;
        }
        if (c == '\'' || c == '"') {
            wordStarted = true;
            quoted = true;
            quote = c;
            continue;
        }
        if (c == ';') break;
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (wordStarted) {
                words.push_back(word);
                word.clear();
                wordStarted = false;
            }
            continue;
        }
        word.push_back(c);
        wordStarted = true;
    }

    if (escaped) word.push_back('\\');
    if (quoted) throw FormatError("unterminated quoted string");
    if (wordStarted) words.push_back(word);
    return words;
}

}  // namespace sim36::monitor
