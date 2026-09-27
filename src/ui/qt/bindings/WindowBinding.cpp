#include "ui/qt/bindings/WindowBinding.hh"

#include "ui/qt/bindings/Post.hh"

#include "services/IWindowConfiguration.hh"
#include "services/ServiceLocator.hh"

#include "util/TypeCasts.hh"

#include <QLabel>
#include <QSize>
#include <QString>
#include <QWidget>

#include <algorithm>
#include <cassert>
#include <cstdint>

namespace ra {
namespace ui {
namespace qt {
namespace bindings {

std::vector<WindowBinding*> WindowBinding::s_vKnownBindings;

WindowBinding::WindowBinding(WindowViewModelBase& vmWindow) : BindingBase(vmWindow, AttachLater{}), m_vmWindow(vmWindow)
{
    // Not yet one of the view model's notify targets: see SetWidget.
    s_vKnownBindings.push_back(this);
}

WindowBinding::~WindowBinding() noexcept
{
    // First, while every member a change handler reads is still alive: once this
    // returns, no other thread is inside one of this binding's handlers or can
    // enter one (RemoveNotifyTargetAndWait waits for a call in progress), so none
    // can post to the window that is about to go.
    DetachFromViewModel();

    const auto pIter = std::find(s_vKnownBindings.begin(), s_vKnownBindings.end(), this);
    if (pIter != s_vKnownBindings.end())
        s_vKnownBindings.erase(pIter);
}

WindowBinding* WindowBinding::GetBindingFor(const WindowViewModelBase& vmWindow) noexcept
{
    for (auto* pBinding : s_vKnownBindings)
    {
        if (&pBinding->m_vmWindow == &vmWindow)
            return pBinding;
    }

    return nullptr;
}

std::vector<QWidget*> WindowBinding::GetBoundWidgets()
{
    std::vector<QWidget*> vWidgets;
    for (const auto* pBinding : s_vKnownBindings)
    {
        auto* pWidget = pBinding->GetWidget();
        if (pWidget != nullptr)
            vWidgets.push_back(pWidget);
    }

    return vWidgets;
}

void WindowBinding::BindLabel(QLabel& oLabel, const StringModelProperty& pSourceProperty)
{
    // the change handlers read m_vLabels without a lock, which is safe only while it cannot change
    assert(m_pWidget.load() == nullptr);
    m_vLabels.emplace_back(&pSourceProperty, &oLabel);
}

void WindowBinding::SetWidget(QWidget& oWidget)
{
    // Join the view model's notify targets only now, when everything a handler
    // reads is ready, and before the values are read: a change to the title or a
    // bound label made from here on either reaches a handler, or is read below.
    // A DialogResult set before the pointer below is published is dropped, as
    // Win32 drops one set before its dialog exists.
    AttachToViewModel();

    // Published before the values are read. A change stored on another thread
    // either loads this pointer and posts itself, or loaded nullptr - and then
    // its store came before these reads: the property container's lock orders
    // its SetValue against CopyValue, and had CopyValue gone first, the store
    // below would have happened before that thread's load.
    m_pWidget.store(&oWidget);

    oWidget.setWindowTitle(QString::fromStdWString(CopyValue(WindowViewModelBase::WindowTitleProperty)));

    for (const auto& pLabel : m_vLabels)
        pLabel.second->setText(QString::fromStdWString(CopyValue(*pLabel.first)));

    RestoreSize(oWidget);
}

void WindowBinding::OnViewModelStringValueChanged(const StringModelProperty::ChangeArgs& args)
{
    // Any thread. Handlers run only once SetWidget has attached the binding; a
    // null widget means the change came between attaching and publishing the
    // pointer, and SetWidget reads that value itself.
    QWidget* pWidget = m_pWidget.load();
    if (pWidget == nullptr)
        return;

    if (args.Property == WindowViewModelBase::WindowTitleProperty)
    {
        Post(*pWidget, [pWidget, sTitle = QString::fromStdWString(args.tNewValue)]() {
            pWidget->setWindowTitle(sTitle);
        });
        return;
    }

    for (const auto& pLabel : m_vLabels)
    {
        if (*pLabel.first == args.Property)
        {
            QLabel* pTarget = pLabel.second;
            Post(*pTarget, [pTarget, sText = QString::fromStdWString(args.tNewValue)]() { pTarget->setText(sText); });
        }
    }
}

void WindowBinding::OnViewModelIntValueChanged(const IntModelProperty::ChangeArgs& args)
{
    // Win32's rule (WindowViewModelBase::SetDialogResult): anything but None closes the window.
    if (args.Property != WindowViewModelBase::DialogResultProperty ||
        args.tNewValue == ra::etoi(DialogResult::None))
    {
        return;
    }

    QWidget* pWidget = m_pWidget.load();
    if (pWidget != nullptr)
        Post(*pWidget, [pWidget]() { pWidget->close(); });
}

void WindowBinding::OnShown()
{
    // Win32 sets it from WM_SHOWWINDOW. Set directly, not through
    // BindingBase::SetValue: nothing here handles IsVisible, so there is no echo
    // to suppress, and SetValue's remove/add of this notify target would race a
    // worker notifying the same view model.
    m_vmWindow.SetIsVisible(true);
}

void WindowBinding::OnClosed()
{
    m_vmWindow.SetIsVisible(false);
}

void WindowBinding::OnResized(int nWidth, int nHeight)
{
    // Saved on every resize, as Win32 does (DialogBase.cpp WM_SIZE):
    // DoShutdown saves the configuration before any window is closed, so a
    // size saved at close would be lost.
    if (m_sSizeKey.empty() || !ra::services::ServiceLocator::Exists<ra::services::IWindowConfiguration>())
        return;

    ra::services::ServiceLocator::GetMutable<ra::services::IWindowConfiguration>().SetWindowSize(
        m_sSizeKey, ra::ui::Size{nWidth, nHeight});
}

void WindowBinding::RestoreSize(QWidget& oWidget) const
{
    if (m_sSizeKey.empty() || !ra::services::ServiceLocator::Exists<ra::services::IWindowConfiguration>())
        return;

    const auto oSaved =
        ra::services::ServiceLocator::Get<ra::services::IWindowConfiguration>().GetWindowSize(m_sSizeKey);
    if (oSaved.Width == INT32_MIN && oSaved.Height == INT32_MIN)
        return; // never saved: keep the layout's size

    // Each dimension on its own, as Win32 does; resize() clamps to the minimum
    // size. The position is not restored: Wayland neither reports nor honours
    // it, and Win32 stores it relative to the emulator's window, which the
    // library cannot see here.
    QSize oSize = oWidget.sizeHint().expandedTo(oWidget.minimumSize());
    if (oSaved.Width != INT32_MIN)
        oSize.setWidth(oSaved.Width);
    if (oSaved.Height != INT32_MIN)
        oSize.setHeight(oSaved.Height);

    oWidget.resize(oSize);
}

} // namespace bindings
} // namespace qt
} // namespace ui
} // namespace ra
