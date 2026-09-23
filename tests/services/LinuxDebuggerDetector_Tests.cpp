#ifndef _WIN32

#include "services/impl/LinuxDebuggerDetector.hh"

#include "tests/RA_UnitTestHelpers.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace services {
namespace impl {
namespace tests {

TEST_CLASS(LinuxDebuggerDetector_Tests)
{
public:
    TEST_METHOD(TestNoDebuggerWhenRunningNormally)
    {
        // ctest does not attach a debugger. Under gdb this test is expected to
        // fail - that is the positive control, run by hand.
        const LinuxDebuggerDetector oDetector;
        Assert::IsFalse(oDetector.IsDebuggerPresent());
    }
};

} // namespace tests
} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32
