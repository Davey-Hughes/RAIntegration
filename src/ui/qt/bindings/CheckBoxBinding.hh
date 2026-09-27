#ifndef RA_UI_QT_BINDINGS_CHECKBOXBINDING_HH
#define RA_UI_QT_BINDINGS_CHECKBOXBINDING_HH
#pragma once

#include "ui/qt/bindings/ControlBinding.hh"

#include <QMetaObject>

#include <atomic>

class QCheckBox;

namespace ra {
namespace ui {
namespace qt {
namespace bindings {

/// <summary>
/// Binds a QCheckBox to a bool property: the Qt counterpart of ui/win32/bindings/CheckBoxBinding.
/// </summary>
class CheckBoxBinding : public ControlBinding
{
public:
    explicit CheckBoxBinding(ViewModelBase& vmViewModel) noexcept : ControlBinding(vmViewModel) {}
    ~CheckBoxBinding() noexcept override;

    CheckBoxBinding(const CheckBoxBinding&) noexcept = delete;
    CheckBoxBinding& operator=(const CheckBoxBinding&) noexcept = delete;
    CheckBoxBinding(CheckBoxBinding&&) noexcept = delete;
    CheckBoxBinding& operator=(CheckBoxBinding&&) noexcept = delete;

    /// <summary>Binds the checked state. Call before <see cref="SetControl" />.</summary>
    void BindCheck(const BoolModelProperty& pProperty) noexcept { m_pCheckedProperty = &pProperty; }

    /// <summary>Attaches the control: joins the notify targets, shows the value, connects clicks. Qt thread, once, last.</summary>
    void SetControl(QCheckBox& oCheckBox);

    void Detach() noexcept override;

protected:
    void OnViewModelBoolValueChanged(const BoolModelProperty::ChangeArgs& args) override;

private:
    std::atomic<QCheckBox*> m_pCheckBox{nullptr};       // written once, by SetControl
    const BoolModelProperty* m_pCheckedProperty = nullptr; // set before SetControl
    QMetaObject::Connection m_oClicked;                  // Qt thread only
};

} // namespace bindings
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !RA_UI_QT_BINDINGS_CHECKBOXBINDING_HH
