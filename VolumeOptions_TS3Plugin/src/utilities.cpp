

#include "../volumeoptions/utilities.h"

#ifdef _WIN32
#include <windows.h>

// TODO: move it and tag it fix for version prior or equal to VS2013
namespace
{
    const long long g_Frequency = []() -> long long
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return frequency.QuadPart;
    }();
}

namespace stdwinfixed {
namespace chrono {

high_resolution_clock::time_point high_resolution_clock::now()
{
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    return time_point(duration(count.QuadPart * static_cast<rep>(period::den) / g_Frequency));
}

} // namespace stdfixed {
} // namespace chrono {
#endif