// The one lexer used by interactive input and command files.
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace sim36::monitor {

// Raised for malformed input, such as an unterminated quoted string.
class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class CommandLine {
public:
    // Splits one monitor line into words.
    //
    //  * `#` at the start of the line is a whole-line comment.  Hash is
    //    deliberately NOT an inline comment: SSP member names such as #CPTC
    //    are ordinary arguments throughout the monitor.
    //  * `;` starts an inline comment.
    //  * Single and double quotes group words containing whitespace; a
    //    quoted empty argument yields an empty word, which configuration
    //    replay needs to distinguish from "no argument".
    //  * Backslash escapes the next character.
    static std::vector<std::string> tokenize(const std::string& line);
};

}  // namespace sim36::monitor
