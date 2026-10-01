#ifndef RA_UI_LOGINVIEWMODEL_H
#define RA_UI_LOGINVIEWMODEL_H
#pragma once

#include "ui/WindowViewModelBase.hh"

namespace ra {
namespace ui {
namespace viewmodels {

class LoginViewModel : public WindowViewModelBase
{
public:
    GSL_SUPPRESS_F6 LoginViewModel();

    /// <summary>
    /// The <see cref="ModelProperty" /> for the username.
    /// </summary>
    static const StringModelProperty UsernameProperty;

    /// <summary>
    /// Gets the username.
    /// </summary>
    const std::wstring& GetUsername() const { return GetValue(UsernameProperty); }

    /// <summary>
    /// Sets the username.
    /// </summary>
    void SetUsername(const std::wstring& sValue) { SetValue(UsernameProperty, sValue); }

    /// <summary>
    /// The <see cref="ModelProperty" /> for the password.
    /// </summary>
    static const StringModelProperty PasswordProperty;

    /// <summary>
    /// Gets the password.
    /// </summary>
    const std::wstring& GetPassword() const { return GetValue(PasswordProperty); }

    /// <summary>
    /// Sets the password.
    /// </summary>
    void SetPassword(const std::wstring& sValue) { SetValue(PasswordProperty, sValue); }

    /// <summary>
    /// The <see cref="ModelProperty" /> for the password.
    /// </summary>
    static const BoolModelProperty IsPasswordRememberedProperty;

    /// <summary>
    /// Gets whether the password should be remembered.
    /// </summary>
    bool IsPasswordRemembered() const { return GetValue(IsPasswordRememberedProperty); }

    /// <summary>
    /// Sets whether the password should be remembered.
    /// </summary>
    void SetPasswordRemembered(bool bValue) { SetValue(IsPasswordRememberedProperty, bValue); }
    
    /// <summary>
    /// The <see cref="ModelProperty" /> for the reason the last login attempt failed.
    /// </summary>
    static const StringModelProperty ErrorMessageProperty;

    /// <summary>
    /// Gets the reason the last login attempt failed; empty if it did not (or has not been tried).
    /// </summary>
    const std::wstring& GetErrorMessage() const { return GetValue(ErrorMessageProperty); }

    /// <summary>
    /// Sets the reason the last login attempt failed.
    /// </summary>
    void SetErrorMessage(const std::wstring& sValue) { SetValue(ErrorMessageProperty, sValue); }

    /// <summary>
    /// Sets whether failures are reported through <see cref="ErrorMessageProperty" /> (and a successful login shows
    /// no message). A view that can show the error itself turns it on; Windows' dialog does not, so it keeps its
    /// message boxes.
    /// </summary>
    void SetShowsErrorsInline(bool bValue) noexcept { m_bShowsErrorsInline = bValue; }

    /// <summary>
    /// Command handler for Login button.
    /// </summary>    
    /// <returns>
    /// <c>true</c> if the user was successfully logged in and the dialog should be closed, 
    /// <c>false</c> if not.
    /// </returns>
    bool Login() const;

protected:
    LoginViewModel(const std::wstring&& sUsername); // alternate costructor for unit tests

private:
    bool m_bShowsErrorsInline = false;
};

} // namespace viewmodels
} // namespace ui
} // namespace ra

#endif !RA_UI_LOGINVIEWMODEL_H
