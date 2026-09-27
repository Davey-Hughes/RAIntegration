#include "ui/qt/bindings/CheckBoxBinding.hh"

#include "ui/qt/bindings/Post.hh"

#include <QCheckBox>

#include <cassert>

namespace ra {
namespace ui {
namespace qt {
namespace bindings {

CheckBoxBinding::~CheckBoxBinding() noexcept
{
    Detach();
}

void CheckBoxBinding::BindCheck(const BoolModelProperty& pProperty) noexcept
{
    assert(m_pCheckBox.load() == nullptr); // before SetControl: the handlers read it
    m_pCheckedProperty = &pProperty;
}

void CheckBoxBinding::SetControl(QCheckBox& oCheckBox)
{
    assert(m_pCheckBox.load() == nullptr); // once
    Attach();
    m_pCheckBox.store(&oCheckBox);

    if (m_pCheckedProperty == nullptr)
        return;

    oCheckBox.setChecked(GetValue(*m_pCheckedProperty));

    // clicked, not toggled: setChecked - a posted update from the view model - emits toggled too
    m_oClicked = QObject::connect(&oCheckBox, &QCheckBox::clicked, &oCheckBox, [this](bool bChecked) {
        if (!IsDetached())
            SetValueFromControl(*m_pCheckedProperty, bChecked);
    });
}

void CheckBoxBinding::Detach() noexcept
{
    QObject::disconnect(m_oClicked);
    ControlBinding::Detach();
}

void CheckBoxBinding::OnViewModelBoolValueChanged(const BoolModelProperty::ChangeArgs& args)
{
    // Any thread, while attached.
    if (m_pCheckedProperty == nullptr || !(args.Property == *m_pCheckedProperty) || IsEchoOfOwnChange(args.Property))
        return;

    QCheckBox* pCheckBox = m_pCheckBox.load();
    if (pCheckBox != nullptr)
        Post(*pCheckBox, [pCheckBox, bChecked = args.tNewValue]() { pCheckBox->setChecked(bChecked); });
}

} // namespace bindings
} // namespace qt
} // namespace ui
} // namespace ra
