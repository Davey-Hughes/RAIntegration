#ifndef RA_UI_QT_BINDINGS_WINDOWBINDING_HH
#define RA_UI_QT_BINDINGS_WINDOWBINDING_HH
#pragma once

#include "ui/BindingBase.hh"
#include "ui/WindowViewModelBase.hh"

#include <atomic>
#include <string>
#include <utility>
#include <vector>

class QLabel;
class QWidget;

namespace ra {
namespace ui {
namespace qt {
namespace bindings {

/// <summary>
/// Connects a window's view model to its Qt widgets: the Qt counterpart of ui/win32/bindings/WindowBinding.
/// </summary>
/// <remarks>
/// Created, used and destroyed on the Qt thread - except the change handlers, which run on whatever thread set the
/// property, and so only read what <see cref="SetWidget" /> fixed, and only ever <see cref="Post" />.
/// The binding joins the view model's notify targets in <see cref="SetWidget" />, once everything its handlers read is
/// ready, and leaves them first thing in its destructor, which returns only when no other thread is inside one of its
/// handlers. So another thread may change the view model at any time. The window and every bound label must still
/// outlive the binding - a DialogBase guarantees that by owning it.
/// </remarks>
class WindowBinding : protected BindingBase
{
public:
    explicit WindowBinding(WindowViewModelBase& vmWindow);
    ~WindowBinding() noexcept;

    WindowBinding(const WindowBinding&) noexcept = delete;
    WindowBinding& operator=(const WindowBinding&) noexcept = delete;
    WindowBinding(WindowBinding&&) noexcept = delete;
    WindowBinding& operator=(WindowBinding&&) noexcept = delete;

    /// <summary>Finds the binding of a view model's window. Compares addresses only. Qt thread only.</summary>
    static WindowBinding* GetBindingFor(const WindowViewModelBase& vmWindow) noexcept;

    /// <summary>Every attached window, in no particular order. Qt thread only.</summary>
    static std::vector<QWidget*> GetBoundWidgets();

    WindowViewModelBase& GetWindowViewModel() const noexcept { return m_vmWindow; }

    /// <summary>The attached window, or <c>nullptr</c> before <see cref="SetWidget" />.</summary>
    QWidget* GetWidget() const noexcept { return m_pWidget.load(); }

    /// <summary>
    /// Saves the window's size under <paramref name="sKey" /> on every resize, and restores it in
    /// <see cref="SetWidget" />. Use the key Win32 uses for the same window. Call before <see cref="SetWidget" />.
    /// </summary>
    void SetSizeKey(std::string sKey) { m_sSizeKey = std::move(sKey); }

    /// <summary>
    /// Shows <paramref name="pSourceProperty" /> in <paramref name="oLabel" />. Call before <see cref="SetWidget" />.
    /// </summary>
    void BindLabel(QLabel& oLabel, const StringModelProperty& pSourceProperty);

    /// <summary>
    /// Attaches the window: pushes the current title and bound values into it and restores its saved size. From here
    /// on, changes reach it. Call once, last, on the Qt thread.
    /// </summary>
    void SetWidget(QWidget& oWidget);

    /// <summary>The window was shown. Qt thread.</summary>
    void OnShown();

    /// <summary>The window was closed. Qt thread.</summary>
    void OnClosed();

    /// <summary>The window's size changed. Qt thread.</summary>
    void OnResized(int nWidth, int nHeight);

protected:
    void OnViewModelStringValueChanged(const StringModelProperty::ChangeArgs& args) override;
    void OnViewModelIntValueChanged(const IntModelProperty::ChangeArgs& args) override;

private:
    void RestoreSize(QWidget& oWidget) const;

    WindowViewModelBase& m_vmWindow;

    // Written once, by SetWidget; read by the change handlers on any thread. See SetWidget for the ordering.
    std::atomic<QWidget*> m_pWidget{nullptr};

    // Filled before SetWidget and never changed after it; the handlers read it only once m_pWidget is set.
    std::vector<std::pair<const StringModelProperty*, QLabel*>> m_vLabels;

    std::string m_sSizeKey;

    static std::vector<WindowBinding*> s_vKnownBindings;
};

} // namespace bindings
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !RA_UI_QT_BINDINGS_WINDOWBINDING_HH
