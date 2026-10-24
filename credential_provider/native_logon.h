#pragma once
#include <windows.h>
#include <credentialprovider.h>
#include <string>
namespace facelogin::credential_provider {
HRESULT PackNativeLogon(const std::wstring& domain, const std::wstring& username,
    const std::wstring& password, CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario,
    BYTE** output, DWORD* outputSize);
}
