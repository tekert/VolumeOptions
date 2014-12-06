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

#ifndef DEBUG_H
#define DEBUG_H

#include "../volumeoptions/config.h"
#include <cassert>

std::string demangle(char const* name);
void print_backtrace(char* out, int len, int max_depth = 0);


#if 0
#ifdef _WIN32
#define __PORTABLE_FUNCTION__ __FUNCTION__
#else
#define __PORTABLE_FUNCTION__ __PRETTY_FUNCTION__
#endif

#include <sstream>

void assert_fail(const char* expr, int line, char const* file
    , char const* function, char const* val, int kind = 0);

#define VO_ASSERT_PRECOND(x) \
	do { if (x) {} else assert_fail(#x, __LINE__, __FILE__, __PORTABLE_FUNCTION__, "", 1); } while (false)

#define VO_ASSERT(x) \
	do { if (x) {} else assert_fail(#x, __LINE__, __FILE__, __PORTABLE_FUNCTION__, "", 0); } while (false)

#define VO_ASSERT_VAL(x, y) \
	do { if (x) {} else { std::stringstream __s__; __s__ << #y ": " << y; \
	assert_fail(#x, __LINE__, __FILE__, __PORTABLE_FUNCTION__, __s__.str().c_str(), 0); } } while (false)
#endif



#endif