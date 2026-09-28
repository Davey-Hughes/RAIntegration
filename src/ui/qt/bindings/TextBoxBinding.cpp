#include "ui/qt/bindings/TextBoxBinding.hh"

#include "ui/qt/bindings/Post.hh"

#include <QLineEdit>
#include <QString>
#include <QTimer>

#include <cassert>
#include <chrono>

namespace ra {
namespace ui {
namespace qt {
namespace bindings {

TextBoxBinding::~TextBoxBinding() noexcept
{
    // First, while everything a handler reads is still alive (see ControlBinding).
    Detach();
}

void TextBoxBinding::BindText(const StringModelProperty& pProperty, UpdateMode nMode) noexcept
{
    assert(m_pLineEdit.load() == nullptr); // before SetControl: the handlers read it
    m_pTextProperty = &pProperty;
    m_nUpdateMode = nMode;
}

void TextBoxBinding::BindReadOnly(const BoolModelProperty& pProperty) noexcept
{
    assert(m_pLineEdit.load() == nullptr); // before SetControl: the handlers read it
    m_pReadOnlyProperty = &pProperty;
}

void TextBoxBinding::SetControl(QLineEdit& oLineEdit)
{
    assert(m_pLineEdit.load() == nullptr); // once

    // Joined first, then published, then read - WindowBinding::SetWidget's order: a change from here on either
    // reaches a handler or is read below.
    Attach();
    m_pLineEdit.store(&oLineEdit);

    if (m_pReadOnlyProperty != nullptr)
        oLineEdit.setReadOnly(GetValue(*m_pReadOnlyProperty));

    if (m_pTextProperty == nullptr)
        return;

    oLineEdit.setText(QString::fromStdWString(CopyValue(*m_pTextProperty)));

    // Only the user's edits: textEdited and editingFinished, never textChanged, which setText - a posted update from
    // the view model - emits too. The control is each connection's context, so none outlives it. LostFocus and Typing
    // write only a real edit (FlushPendingEdit): editingFinished fires on Return with nothing edited, and a view-model
    // change shown during the typing pause replaced the edit - an unconditional write would put back what the control
    // showed before over a change still queued for it.
    switch (m_nUpdateMode)
    {
        case UpdateMode::LostFocus:
            m_vConnections.push_back(QObject::connect(&oLineEdit, &QLineEdit::editingFinished, &oLineEdit,
                                                      [this]() { FlushPendingEdit(); }));
            break;

        case UpdateMode::KeyPress:
            m_vConnections.push_back(
                QObject::connect(&oLineEdit, &QLineEdit::textEdited, &oLineEdit, [this]() { UpdateSource(); }));
            break;

        case UpdateMode::Typing:
        {
            // Win32 uses a thread-pool timer and a version counter; a single-shot timer on the Qt thread, restarted
            // on each edit, needs neither.
            auto* pTimer = new QTimer(&oLineEdit);
            pTimer->setSingleShot(true);
            pTimer->setInterval(std::chrono::milliseconds(300));
            m_pTypingTimer = pTimer;
            m_vConnections.push_back(
                QObject::connect(pTimer, &QTimer::timeout, &oLineEdit, [this]() { FlushPendingEdit(); }));
            m_vConnections.push_back(
                QObject::connect(&oLineEdit, &QLineEdit::textEdited, pTimer, [pTimer]() { pTimer->start(); }));
            break;
        }

        default:
            break;
    }
}

void TextBoxBinding::UpdateSource()
{
    auto* pLineEdit = m_pLineEdit.load();
    if (pLineEdit == nullptr || m_pTextProperty == nullptr || IsDetached())
        return;

    if (!m_pTypingTimer.isNull())
        m_pTypingTimer->stop(); // written now: nothing left to delay

    SetValueFromControl(*m_pTextProperty, pLineEdit->text().toStdWString());
    pLineEdit->setModified(false); // written: no longer pending
}

void TextBoxBinding::FlushPendingEdit()
{
    const auto* pLineEdit = m_pLineEdit.load();
    if (pLineEdit != nullptr && m_nUpdateMode != UpdateMode::None && pLineEdit->isModified())
        UpdateSource();
}

void TextBoxBinding::Detach() noexcept
{
    for (const auto& oConnection : m_vConnections)
        QObject::disconnect(oConnection);
    m_vConnections.clear();

    if (!m_pTypingTimer.isNull())
        m_pTypingTimer->stop();

    ControlBinding::Detach();
}

void TextBoxBinding::OnViewModelStringValueChanged(const StringModelProperty::ChangeArgs& args)
{
    // Any thread, while attached.
    if (m_pTextProperty == nullptr || !(args.Property == *m_pTextProperty) || IsEchoOfOwnChange(args.Property))
        return;

    QLineEdit* pLineEdit = m_pLineEdit.load();
    if (pLineEdit == nullptr)
        return; // SetControl reads the value itself

    Post(*pLineEdit, [pLineEdit, sText = QString::fromStdWString(args.tNewValue)]() {
        // setText moves the cursor to the end: an equal text is left alone
        if (pLineEdit->text() != sText)
            pLineEdit->setText(sText);
    });
}

void TextBoxBinding::OnViewModelBoolValueChanged(const BoolModelProperty::ChangeArgs& args)
{
    if (m_pReadOnlyProperty == nullptr || !(args.Property == *m_pReadOnlyProperty))
        return;

    QLineEdit* pLineEdit = m_pLineEdit.load();
    if (pLineEdit != nullptr)
        Post(*pLineEdit, [pLineEdit, bReadOnly = args.tNewValue]() { pLineEdit->setReadOnly(bReadOnly); });
}

} // namespace bindings
} // namespace qt
} // namespace ui
} // namespace ra
