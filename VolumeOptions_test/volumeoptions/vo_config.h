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

#ifndef VO_CONFIG_H
#define VO_CONFIG_H

#include <boost/config.hpp>

// Unique ID of this software, randomly generated DONT TOUCH IT unless necessary
#define VO_GUID_STRING "{D2C1BB1F-47D8-48BF-AC69-7E4E7B2DB6BF}"




//TODO: do we really need this macro? was originaly for vista support but...
#define VO_ENABLE_EVENTS


#define VO_USE_SYSTEM_ASSERT 0 // 1 = use standard system asserts, 0 = use our asserts
#define VO_VERSION "test" // TODO remove this

#ifdef _DEBUG
#define PRINT_LOG 1

#if !VO_USE_SYSTEM_ASSERT
#define VO_WRITE_TO_FILE_ASSERTS 0 // 1 = writes asserts to file and continues normal execution. 0 = prints and aborts
#endif

#else
#define PRINT_LOG 0

#define VO_RELEASE_ASSERTS  // forces asserts on release mode
#ifdef VO_RELEASE_ASSERTS
#define VO_WRITE_TO_FILE_ASSERTS 1 
#endif

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