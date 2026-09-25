#pragma once

#include <string>

namespace sim36::host {

// Render decoded line-printer text on classic fanfold paper. The output is
// written directly to path, which should be a private .part file that the
// caller publishes atomically after this returns.
bool writePrinterPdf(const std::string& path, const std::string& text,
                     const std::string& paper, std::string& error);

}  // namespace sim36::host
