#include "CppUnitTest.h"

#include <cstdio>
#include <cstring>
#include <exception>

namespace ratest {

std::vector<TestCase>& Registry()
{
    static std::vector<TestCase> s_vRegistry;
    return s_vRegistry;
}

struct AssertionFailure
{
    std::wstring sMessage;
};

void Fail(const std::wstring& sMessage) { throw AssertionFailure{sMessage}; }

// Intentionally independent of StringBuilder: this must work while the code
// under test does not.
static std::string ToUtf8(const std::wstring& sInput)
{
    std::string sResult;
    for (const wchar_t wc : sInput)
    {
        const unsigned long c = static_cast<unsigned long>(wc);
        if (c < 0x80)
        {
            sResult.push_back(static_cast<char>(c));
        }
        else if (c < 0x800)
        {
            sResult.push_back(static_cast<char>(0xC0 | (c >> 6)));
            sResult.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
        else if (c < 0x10000)
        {
            sResult.push_back(static_cast<char>(0xE0 | (c >> 12)));
            sResult.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            sResult.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
        else
        {
            sResult.push_back(static_cast<char>(0xF0 | (c >> 18)));
            sResult.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            sResult.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            sResult.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return sResult;
}

} // namespace ratest

namespace Microsoft {
namespace VisualStudio {
namespace CppUnitTestFramework {

std::wstring ToString(const std::wstring& s) { return s; }
std::wstring ToString(const wchar_t* s) { return s ? std::wstring(s) : std::wstring(L"nullptr"); }
std::wstring ToString(bool b) { return b ? L"true" : L"false"; }

std::wstring ToString(const char* s)
{
    if (!s)
        return L"nullptr";
    std::wstring sResult;
    for (const char* p = s; *p; ++p)
        sResult.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
    return sResult;
}

std::wstring ToString(const std::string& s) { return ToString(s.c_str()); }

std::wstring BuildMessage(const wchar_t* sAssertion, const std::wstring& sExpected,
                          const std::wstring& sActual, const wchar_t* sUserMessage)
{
    std::wstring sResult(sAssertion);
    sResult += L" failed. Expected: <";
    sResult += sExpected;
    sResult += L"> Actual: <";
    sResult += sActual;
    sResult += L'>';
    if (sUserMessage)
    {
        sResult += L" - ";
        sResult += sUserMessage;
    }
    return sResult;
}

void Assert::AreEqual(const char* sExpected, const char* sActual, const wchar_t* sMessage)
{
    const bool bEqual = (sExpected == sActual) ||
                        (sExpected && sActual && std::strcmp(sExpected, sActual) == 0);
    if (!bEqual)
        ::ratest::Fail(BuildMessage(L"AreEqual", ToString(sExpected), ToString(sActual), sMessage));
}

void Assert::AreEqual(const wchar_t* sExpected, const wchar_t* sActual, const wchar_t* sMessage)
{
    const bool bEqual = (sExpected == sActual) ||
                        (sExpected && sActual && std::wcscmp(sExpected, sActual) == 0);
    if (!bEqual)
        ::ratest::Fail(BuildMessage(L"AreEqual", ToString(sExpected), ToString(sActual), sMessage));
}

void Assert::IsTrue(bool bCondition, const wchar_t* sMessage)
{
    if (!bCondition)
        ::ratest::Fail(BuildMessage(L"IsTrue", L"true", L"false", sMessage));
}

void Assert::IsFalse(bool bCondition, const wchar_t* sMessage)
{
    if (bCondition)
        ::ratest::Fail(BuildMessage(L"IsFalse", L"false", L"true", sMessage));
}

void Assert::Fail(const wchar_t* sMessage)
{
    ::ratest::Fail(sMessage ? std::wstring(sMessage) : std::wstring(L"Assert::Fail"));
}

} // namespace CppUnitTestFramework
} // namespace VisualStudio
} // namespace Microsoft

int main(int argc, char* argv[])
{
    const char* sFilter = nullptr;
    bool bList = false;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strncmp(argv[i], "--filter=", 9) == 0)
            sFilter = argv[i] + 9;
        else if (std::strcmp(argv[i], "--list") == 0)
            bList = true;
        else
        {
            std::fprintf(stderr, "unrecognised argument: %s\n", argv[i]);
            std::fprintf(stderr, "usage: ra_tests [--filter=ClassName] [--list]\n");
            return 2;
        }
    }

    auto& vTests = ::ratest::Registry();

    if (bList)
    {
        for (const auto& pTest : vTests)
            std::printf("%s::%s\n", pTest.sSuite, pTest.sName);
        return 0;
    }

    size_t nRun = 0;
    size_t nFailed = 0;

    for (const auto& pTest : vTests)
    {
        if (sFilter && std::strcmp(sFilter, pTest.sSuite) != 0)
            continue;

        ++nRun;
        try
        {
            pTest.pRun();
        }
        catch (const ::ratest::AssertionFailure& oFailure)
        {
            ++nFailed;
            std::printf("FAIL %s::%s\n     %s\n", pTest.sSuite, pTest.sName,
                        ::ratest::ToUtf8(oFailure.sMessage).c_str());
        }
        catch (const std::exception& oError)
        {
            ++nFailed;
            std::printf("FAIL %s::%s\n     unexpected exception: %s\n",
                        pTest.sSuite, pTest.sName, oError.what());
        }

        // A test can take the process down without unwinding: an assertion that
        // fails inside an rc_client callback throws across a noexcept boundary
        // and calls std::terminate. Block-buffered stdout would lose every
        // failure reported since the last flush, which would make the reported
        // failure set depend on where the 4KB buffer happened to end.
        std::fflush(stdout);
    }

    std::printf("%zu run, %zu failed\n", nRun, nFailed);

    if (sFilter && nRun == 0)
    {
        std::fprintf(stderr, "no tests matched --filter=%s\n", sFilter);
        return 2;
    }

    return nFailed == 0 ? 0 : 1;
}
