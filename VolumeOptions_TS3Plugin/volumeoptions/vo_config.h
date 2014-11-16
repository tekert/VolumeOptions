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

* Neither the name of [project] nor the names of its
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

#ifndef VO_CONFIG_H
#define VO_CONFIG_H

#include <boost/config.hpp>
#ifndef BOOST_NO_CXX11_VARIADIC_TEMPLATES
// Workaround, boost disabled this define for mscv 12 as a workaround, but mscv 12 supports it.
#if _MSC_VER == 1800
#define COMPILER_SUPPORT_VARIADIC_TEMPLATES
#endif
#else
#define COMPILER_SUPPORT_VARIADIC_TEMPLATES
#endif

//TODO: do we really need this macro? was originaly for vista support but...
#define VO_ENABLE_EVENTS 

// TODO: i dont like boost::log, too many dependacies for a logger, ill do it myself maybe.
#ifdef _DEBUG
#define PRINT_LOG 1
#else
#define PRINT_LOG 0
#endif

#ifdef PRINT_LOG
#if PRINT_LOG
#define dprintf(...) printf(__VA_ARGS__)
#define dwprintf(...) wprintf(__VA_ARGS__)
#else
#define dprintf(...)
#define dwprintf(...)
#endif
#endif


#endif