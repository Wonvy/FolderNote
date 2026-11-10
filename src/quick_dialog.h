#pragma once
#include "quick_menu_gate.h"
#include <string>

namespace quick::dialog {
enum class NavigateResult {
  Success, TargetChanged, ActivationFailed, ModifierPressed, InputBlocked,
  AddressUnavailable, AddressWriteFailed, AddressSubmitFailed
};
NavigateResult navigate(HWND target, const std::wstring &path, const detail::MenuRequest &request);
const wchar_t *failureMessage(NavigateResult result) noexcept;
}
