
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

#ifndef _DEBUG
#define dprintf(...)
#define dwprintf(...)
#else
#define dprintf(...) printf(__VA_ARGS__)
#define dwprintf(...) wprintf(__VA_ARGS__)
#endif


#endif