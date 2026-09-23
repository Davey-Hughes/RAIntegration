#ifndef RA_TESTS_COMPAT_CPPUNITTEST_H
#define RA_TESTS_COMPAT_CPPUNITTEST_H
#pragma once

#include <cstddef>
#include <cstring>
#include <cwchar>
#include <string>
#include <type_traits>
#include <vector>

namespace ratest {

struct TestCase
{
    const char* sSuite;
    const char* sName;
    void (*pRun)();
};

std::vector<TestCase>& Registry();

[[noreturn]] void Fail(const std::wstring& sMessage);

template<class T, void (T::*M)()>
void Invoke()
{
    T oInstance;
    (oInstance.*M)();
}

template<class T, const char* Name>
class TestClass
{
protected:
    using SelfType = T;
    static constexpr const char* SuiteName() noexcept { return Name; }
};

} // namespace ratest

// TEST_CLASS(X) must expand to exactly a class-head, because the test file
// supplies the `{ ... };` that follows it. The suite name reaches TEST_METHOD
// through the base class rather than through a member, for the same reason.
#define TEST_CLASS(className)                                                    \
    inline constexpr char className##_TestSuiteName[] = #className;              \
    class className : public ::ratest::TestClass<className, className##_TestSuiteName>

// The registration happens inside the constructor body of a nested class
// rather than in the initialiser of a static data member. Declaring the method
// and then defining it in the same class body is ill-formed ("class member
// cannot be redeclared"), so it cannot be forward-declared here; and a static
// data member's initialiser is not a complete-class context, so
// &SelfType::methodName would not resolve there. A nested member function body
// *is* a complete-class context of the enclosing class, so its parsing is
// delayed until the test class is complete and the method - declared further
// down the member-specification - is visible. Invoke<> is instantiated from
// there, where SelfType is complete.
#define TEST_METHOD(methodName)                                                  \
    struct methodName##_RegistrarType                                            \
    {                                                                            \
        methodName##_RegistrarType()                                             \
        {                                                                        \
            ::ratest::Registry().push_back(                                      \
                {SuiteName(), #methodName,                                       \
                 &::ratest::Invoke<SelfType, &SelfType::methodName>});           \
        }                                                                        \
    };                                                                           \
    inline static const methodName##_RegistrarType s_##methodName##_Registrar{}; \
    void methodName()

namespace Microsoft {
namespace VisualStudio {
namespace CppUnitTestFramework {

// Primary template. The suite provides 41 explicit specialisations of this,
// all of the form: template<> std::wstring ToString<T>(const T&).
template<typename T>
std::wstring ToString(const T& t)
{
    if constexpr (std::is_enum_v<T>)
        return std::to_wstring(static_cast<long long>(t));
    else if constexpr (std::is_arithmetic_v<T>)
        return std::to_wstring(t);
    else
        return L"<object>";
}

// Non-template overloads win over the primary and keep failure messages
// readable for the types the suite actually compares most often.
std::wstring ToString(const std::string& s);
std::wstring ToString(const std::wstring& s);
std::wstring ToString(const char* s);
std::wstring ToString(const wchar_t* s);
std::wstring ToString(bool b);

std::wstring BuildMessage(const wchar_t* sAssertion, const std::wstring& sExpected,
                          const std::wstring& sActual, const wchar_t* sUserMessage);

class Assert
{
public:
    template<typename T>
    static void AreEqual(const T& tExpected, const T& tActual, const wchar_t* sMessage = nullptr)
    {
        static_assert(!std::is_array_v<T>,
                      "Assert::AreEqual on two arrays of the same extent would bind here and compare "
                      "addresses; add/use a char or wchar_t array overload, or decay to a pointer.");
        if (!(tExpected == tActual))
            ::ratest::Fail(BuildMessage(L"AreEqual", ToString(tExpected), ToString(tActual), sMessage));
    }

    static void AreEqual(const char* sExpected, const char* sActual, const wchar_t* sMessage = nullptr);
    static void AreEqual(const wchar_t* sExpected, const wchar_t* sActual, const wchar_t* sMessage = nullptr);

    // Equal-extent `char`/`wchar_t` arrays (e.g. two `char[8]` fields, or a
    // string literal compared against one) would otherwise deduce the generic
    // template above with T = the array type and compare addresses instead of
    // contents. N is a single template parameter, not one per side: that
    // makes this overload strictly more specialized than the generic one
    // (partial ordering can go only one way, since the generic template can
    // be deduced from this overload's synthesized arguments but not the
    // reverse), so it wins outright instead of tying with it and making the
    // call ambiguous. A mismatched-extent pair (N != M) simply doesn't match
    // this overload and falls back to array-to-pointer decay into the
    // pointer overload below, which is exactly what should happen. This
    // forwards to that pointer overload so the comparison is strcmp/wcscmp.
    template<size_t N>
    static void AreEqual(const char (&sExpected)[N], const char (&sActual)[N], const wchar_t* sMessage = nullptr)
    {
        AreEqual(static_cast<const char*>(sExpected), static_cast<const char*>(sActual), sMessage);
    }

    template<size_t N>
    static void AreEqual(const wchar_t (&sExpected)[N], const wchar_t (&sActual)[N], const wchar_t* sMessage = nullptr)
    {
        AreEqual(static_cast<const wchar_t*>(sExpected), static_cast<const wchar_t*>(sActual), sMessage);
    }

    template<typename T>
    static void AreNotEqual(const T& tNotExpected, const T& tActual, const wchar_t* sMessage = nullptr)
    {
        static_assert(!std::is_array_v<T>,
                      "Assert::AreNotEqual on two arrays of the same extent would bind here and compare "
                      "addresses; add/use a char or wchar_t array overload, or decay to a pointer.");
        if (tNotExpected == tActual)
            ::ratest::Fail(BuildMessage(L"AreNotEqual", ToString(tNotExpected), ToString(tActual), sMessage));
    }

    // Same reasoning as the AreEqual array overloads above: a single N (not
    // one per side) keeps this strictly more specialized than the generic
    // AreNotEqual template so the call isn't ambiguous, and lets a
    // mismatched-extent pair fall back to array-to-pointer decay instead.
    template<size_t N>
    static void AreNotEqual(const char (&sNotExpected)[N], const char (&sActual)[N], const wchar_t* sMessage = nullptr)
    {
        if (std::strcmp(sNotExpected, sActual) == 0)
            ::ratest::Fail(BuildMessage(L"AreNotEqual", ToString(static_cast<const char*>(sNotExpected)),
                                        ToString(static_cast<const char*>(sActual)), sMessage));
    }

    template<size_t N>
    static void AreNotEqual(const wchar_t (&sNotExpected)[N], const wchar_t (&sActual)[N], const wchar_t* sMessage = nullptr)
    {
        if (std::wcscmp(sNotExpected, sActual) == 0)
            ::ratest::Fail(BuildMessage(L"AreNotEqual", ToString(static_cast<const wchar_t*>(sNotExpected)),
                                        ToString(static_cast<const wchar_t*>(sActual)), sMessage));
    }

    static void IsTrue(bool bCondition, const wchar_t* sMessage = nullptr);
    static void IsFalse(bool bCondition, const wchar_t* sMessage = nullptr);

    template<typename T>
    static void IsNull(const T* pValue, const wchar_t* sMessage = nullptr)
    {
        if (pValue != nullptr)
            ::ratest::Fail(BuildMessage(L"IsNull", L"nullptr", L"non-null", sMessage));
    }

    template<typename T>
    static void IsNotNull(const T* pValue, const wchar_t* sMessage = nullptr)
    {
        if (pValue == nullptr)
            ::ratest::Fail(BuildMessage(L"IsNotNull", L"non-null", L"nullptr", sMessage));
    }

    [[noreturn]] static void Fail(const wchar_t* sMessage = nullptr);
};

} // namespace CppUnitTestFramework
} // namespace VisualStudio
} // namespace Microsoft

#endif // !RA_TESTS_COMPAT_CPPUNITTEST_H
