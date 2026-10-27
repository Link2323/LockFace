#pragma once

#include <windows.h>
#include <string>
#include <string_view>

namespace facelogin::ui {

// Machine preference: LogonUI runs as SYSTEM and cannot read the enrolled
// user's HKCU. Only presentation uses this value; it never controls auth.
inline std::wstring LanguageMode() {
    wchar_t value[16] = {};
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\LockFace", L"UiLanguage",
                     RRF_RT_REG_SZ, nullptr, value, &bytes) == ERROR_SUCCESS &&
        (wcscmp(value, L"en") == 0 || wcscmp(value, L"zh-CN") == 0)) return value;
    return L"system";
}

inline bool IsEnglish() {
    const auto mode = LanguageMode();
    if (mode != L"system") return mode == L"en";
    return PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_CHINESE;
}

inline bool SetLanguageMode(const std::wstring& mode) {
    if (mode != L"system" && mode != L"zh-CN" && mode != L"en") return false;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\LockFace", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const auto result = RegSetValueExW(key, L"UiLanguage", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(mode.c_str()),
        static_cast<DWORD>((mode.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

inline const wchar_t* English(const wchar_t* source) {
    struct Message { const wchar_t* zh; const wchar_t* en; };
    static constexpr Message messages[] = {
#include "ui_messages.inc"
    };
    for (const auto& message : messages) {
        if (std::wstring_view(source) == message.zh) return message.en;
    }
    return source;
}

inline const wchar_t* Text(const wchar_t* source) {
    return IsEnglish() ? English(source) : source;
}

inline std::wstring Text(const std::wstring& source) {
    return Text(source.c_str());
}

} // namespace facelogin::ui
