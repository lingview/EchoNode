#pragma once
#include <string>
#include <windows.h>

namespace echonode::platform::win {

inline std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

inline std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

inline std::string codepageToUtf8(const std::string& s, UINT codepage) {
    if (s.empty()) return {};
    int wn = MultiByteToWideChar(codepage, 0, s.data(), static_cast<int>(s.size()),
                                 nullptr, 0);
    std::wstring w(wn, L'\0');
    MultiByteToWideChar(codepage, 0, s.data(), static_cast<int>(s.size()),
                        w.data(), wn);
    return wideToUtf8(w);
}

} // namespace echonode::platform::win
