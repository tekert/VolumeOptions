/*
Copyright (c) 2014, Paul Dolcet
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this
    list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

    * Neither the name of VolumeOptions nor the names of its
    contributors may be used to endorse or promote products derived from
    this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef VO_UTILITIES_H
#define VO_UTILITIES_H


/*  Utilities	*/
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif 
#include <codecvt>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif 

#include <chrono>

///////////////////////////////////////////////////////


#ifdef _WIN32
// to fix visual studio bug https://connect.microsoft.com/VisualStudio/feedback/details/719443/
namespace stdwinfixed {
    namespace chrono {

        struct high_resolution_clock
        {
            typedef long long                                               rep;
            typedef std::nano                                               period;
            typedef std::chrono::duration<rep, period>                      duration;
            typedef std::chrono::time_point<high_resolution_clock>          time_point;
            static const bool is_steady = true;

            static time_point now();
        };

    } //namespace stdfixed
} //namespace chrono
#endif

namespace vo {

#ifdef _WIN32
    typedef stdwinfixed::chrono::high_resolution_clock  high_resolution_clock;
#else
    typedef std::chrono::high_resolution_clock  high_resolution_clock;
#endif


// C++11 Standard conversions

// convert UTF-8 string to wstring
inline std::wstring utf8_to_wstring(const std::string& str)
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>> myconv;
    return myconv.from_bytes(str);
}

// convert wstring to UTF-8 string
inline std::string wstring_to_utf8(const std::wstring& str)
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>> myconv;
    return myconv.to_bytes(str);
}

} // end namespace vo


#endif