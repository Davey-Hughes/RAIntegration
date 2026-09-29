#include "MessageBoxViewModel.hh"

namespace ra {
namespace ui {
namespace viewmodels {

const StringModelProperty MessageBoxViewModel::MessageProperty("MessageBoxViewModel", "Message", L"");
const StringModelProperty MessageBoxViewModel::HeaderProperty("MessageBoxViewModel", "Header", L"");
const IntModelProperty MessageBoxViewModel::IconProperty("MessageBoxViewModel", "Icon", static_cast<int>(MessageBoxViewModel::Icon::None));
const IntModelProperty MessageBoxViewModel::ButtonsProperty("MessageBoxViewModel", "Buttons", static_cast<int>(MessageBoxViewModel::Buttons::OK));
const StringModelProperty MessageBoxViewModel::CheckBoxTextProperty("MessageBoxViewModel", "CheckBoxText", L"");
const BoolModelProperty MessageBoxViewModel::CheckBoxCheckedProperty("MessageBoxViewModel", "CheckBoxChecked", false);

} // namespace viewmodels
} // namespace ui
} // namespace ra
