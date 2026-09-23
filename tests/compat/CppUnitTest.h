#ifndef RA_TESTS_COMPAT_CPPUNITTEST_H
#define RA_TESTS_COMPAT_CPPUNITTEST_H
#pragma once

#include <cstddef>
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

struct Registrar
{
    Registrar(const char* sSuite, const char* sName, void (*pRun)());
};

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
        if (!(tExpected == tActual))
            ::ratest::Fail(BuildMessage(L"AreEqual", ToString(tExpected), ToString(tActual), sMessage));
    }

    static void AreEqual(const char* sExpected, const char* sActual, const wchar_t* sMessage = nullptr);
    static void AreEqual(const wchar_t* sExpected, const wchar_t* sActual, const wchar_t* sMessage = nullptr);

    template<typename T>
    static void AreNotEqual(const T& tNotExpected, const T& tActual, const wchar_t* sMessage = nullptr)
    {
        if (tNotExpected == tActual)
            ::ratest::Fail(BuildMessage(L"AreNotEqual", ToString(tNotExpected), ToString(tActual), sMessage));
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
