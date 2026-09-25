#include "Host/PrinterPdf.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <sstream>
#include <vector>

#include <hpdf.h>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace sim36::host {
namespace {

struct PdfError {
    HPDF_STATUS code = HPDF_OK;
    HPDF_STATUS detail = HPDF_OK;
};

void errorHandler(HPDF_STATUS code, HPDF_STATUS detail, void* data)
{
    auto* error = static_cast<PdfError*>(data);
    if (error->code == HPDF_OK) {
        error->code = code;
        error->detail = detail;
    }
}

struct Color {
    int r;
    int g;
    int b;
};

struct Palette {
    Color dark;
    Color light;
};

Palette paletteFor(const std::string& paper)
{
    if (paper == "blue") return {{65, 182, 255}, {214, 239, 255}};
    if (paper == "gray") return {{200, 200, 200}, {230, 230, 230}};
    if (paper == "orange") return {{219, 182, 99}, {255, 221, 146}};
    if (paper == "white") return {{220, 220, 220}, {255, 255, 255}};
    return {{99, 182, 99}, {219, 250, 219}};
}

HPDF_REAL pdfColorChannel(int value)
{
    return static_cast<HPDF_REAL>(value) / static_cast<HPDF_REAL>(255);
}

void rgbFill(HPDF_Page page, Color color)
{
    HPDF_Page_SetRGBFill(page, pdfColorChannel(color.r), pdfColorChannel(color.g),
                         pdfColorChannel(color.b));
}

void rgbStroke(HPDF_Page page, Color color)
{
    HPDF_Page_SetRGBStroke(page, pdfColorChannel(color.r), pdfColorChannel(color.g),
                           pdfColorChannel(color.b));
}

std::vector<std::vector<std::string>> pagesOf(const std::string& text)
{
    std::vector<std::vector<std::string>> pages(1);
    std::string line;
    bool afterFormFeed = false;
    auto finishLine = [&] {
        pages.back().push_back(line);
        line.clear();
        if (pages.back().size() == 66) pages.emplace_back();
    };
    for (char ch : text) {
        if (ch == '\f') {
            if (!line.empty()) finishLine();
            if (!pages.back().empty()) pages.emplace_back();
            afterFormFeed = true;
        } else if (ch == '\n') {
            if (afterFormFeed) afterFormFeed = false;
            else finishLine();
        } else if (ch != '\r') {
            afterFormFeed = false;
            line += ch;
        }
    }
    if (!line.empty()) finishLine();
    if (pages.size() > 1 && pages.back().empty()) pages.pop_back();
    return pages;
}

std::filesystem::path executableDirectory()
{
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && static_cast<std::size_t>(length) < buffer.size())
        return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) == 0)
        return std::filesystem::weakly_canonical(buffer.data()).parent_path();
#else
    std::array<char, 4096> buffer{};
    const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length > 0)
        return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length))).parent_path();
#endif
    return {};
}

std::filesystem::path fontPath()
{
    const std::array<std::filesystem::path, 4> candidates = {
        executableDirectory() / "fonts" / "IBMPlexMono-Regular.ttf",
        std::filesystem::path("fonts") / "IBMPlexMono-Regular.ttf",
        std::filesystem::path("assets") / "fonts" / "IBMPlexMono-Regular.ttf",
        std::filesystem::path("..") / "assets" / "fonts" / "IBMPlexMono-Regular.ttf"
    };
    for (const auto& candidate : candidates)
        if (std::filesystem::exists(candidate)) return candidate;
    return candidates.front();
}

void drawForm(HPDF_Page page, const Palette& colors)
{
    constexpr float width = 1044.0F;
    constexpr float height = 792.0F;
    rgbFill(page, colors.light);
    HPDF_Page_Rectangle(page, 0, 0, width, height);
    HPDF_Page_Fill(page);

    rgbFill(page, colors.dark);
    for (int band = 0; band < 11; ++band) {
        HPDF_Page_Rectangle(page, 30, static_cast<float>(band * 72), width - 60, 36);
        HPDF_Page_Fill(page);
    }

    rgbStroke(page, colors.dark);
    HPDF_Page_SetLineWidth(page, 0.5F);
    for (int line = 0; line <= 66; line += 3) {
        const float y = 6.0F + static_cast<float>(line) * 12.0F;
        HPDF_Page_MoveTo(page, 40, y);
        HPDF_Page_LineTo(page, width - 40, y);
        HPDF_Page_Stroke(page);
    }

    rgbStroke(page, {200, 200, 200});
    HPDF_Page_SetLineWidth(page, 0.75F);
    for (int hole = 0; hole < 22; ++hole) {
        const float y = 18.0F + static_cast<float>(hole) * 36.0F;
        for (float x : {20.0F, width - 20.0F}) {
            HPDF_Page_Circle(page, x, y, 4.0F);
            HPDF_Page_Stroke(page);
        }
    }
}

}  // namespace

bool writePrinterPdf(const std::string& path, const std::string& text,
                     const std::string& paper, std::string& error)
{
    PdfError pdfError;
    HPDF_Doc document = HPDF_New(errorHandler, &pdfError);
    if (document == nullptr) {
        error = "cannot allocate PDF document";
        return false;
    }
    const std::filesystem::path font = fontPath();
    if (!std::filesystem::exists(font)) {
        error = "cannot find bundled font " + font.string();
        HPDF_Free(document);
        return false;
    }

    HPDF_UseUTFEncodings(document);
    HPDF_SetCurrentEncoder(document, "UTF-8");
    const char* loadedName = HPDF_LoadTTFontFromFile(document, font.string().c_str(), HPDF_TRUE);
    HPDF_Font pdfFont = loadedName == nullptr ? nullptr : HPDF_GetFont(document, loadedName, "UTF-8");
    if (pdfFont == nullptr) {
        error = "cannot load bundled font " + font.string();
        HPDF_Free(document);
        return false;
    }

    const Palette palette = paletteFor(paper);
    for (const auto& lines : pagesOf(text)) {
        HPDF_Page page = HPDF_AddPage(document);
        HPDF_Page_SetWidth(page, 1044);
        HPDF_Page_SetHeight(page, 792);
        drawForm(page, palette);
        HPDF_Page_SetRGBFill(page, 0, 0, 0);
        HPDF_Page_BeginText(page);
        HPDF_Page_SetFontAndSize(page, pdfFont, 10);
        for (std::size_t row = 0; row < lines.size() && row < 66; ++row) {
            std::string line = lines[row].substr(0, 132);
            HPDF_Page_TextOut(page, 43, 780.0F - static_cast<float>(row) * 12.0F, line.c_str());
        }
        HPDF_Page_EndText(page);
    }

    const HPDF_STATUS saved = HPDF_SaveToFile(document, path.c_str());
    if (saved != HPDF_OK || pdfError.code != HPDF_OK) {
        std::ostringstream message;
        message << "libharu error 0x" << std::hex
                << (pdfError.code == HPDF_OK ? saved : pdfError.code)
                << " detail 0x" << pdfError.detail;
        error = message.str();
        HPDF_Free(document);
        return false;
    }
    HPDF_Free(document);
    return true;
}

}  // namespace sim36::host
