
#ifndef AP_VERSION_WIN32_HPP_INCLUDED
#define AP_VERSION_WIN32_HPP_INCLUDED

// file versioning for windows
#ifdef _WIN32

#include "windows.h"




// ------------------------------------------------------------------------------

/*-----------------------------------------------------------
VERSION CONTROL BUILD SYSTEM
This header file was modified by VERBUILD v1.0.1
-----------------------------------------------------------
help : verbuild -?
info : http://www.yeamaec.com
yeamaec@hanafos.com ,krkim@yeamaec.com
Customized by Pablo Dolcet.
-----------------------------------------------------------*/

#define STRINGIZE2(s) #s
#define STRINGIZE(s) STRINGIZE2(s)

// VO format of versioning is: MMmmbb
// M = Major version, m = minor version, t = custom build version
// extend is times built or source control version

// VERSION_ are auto generated
// "PROJECTNAME"_ are for internal use
// VER_ are for resource use.

//TODO: when using source control, replace EXTEND with revision from repository.
//for now i use a incremental number from tool verbuild.

// Start verbuild generated numbers
#define VERSION_FULL           0.6.20.0

#define VERSION_DATE           "2014-04-01"
#define VERSION_TIME           "22:03:58"
#define VERSION_BASEYEAR       2014

#define VERSION_MAJOR          0
#define VERSION_MINOR          6
#define VERSION_BUILDNO        20
#define VERSION_EXTEND         0

#define VERSION_FILE           0,6,20,0
#define VERSION_PRODUCT        0,6,20,0
#define VERSION_FILESTR        "0,6,20,0"
#define VERSION_PRODUCTSTR     "0,6,20,0"
// End verbuild generation

#define VO_VERSION VERSION_FULL
#define VO_VERSION_STR	STRINGIZE(VERSION_MAJOR)        \
	"." STRINGIZE(VERSION_MINOR)    \
	"." STRINGIZE(VERSION_BUILDNO) \
	"." STRINGIZE(VERSION_EXTEND)    \

#define VO_VERSION_RELEASE_STR	STRINGIZE(VERSION_MAJOR)        \
	"." STRINGIZE(VERSION_MINOR)    \
	"." STRINGIZE(VERSION_BUILDNO) \

//#define VERSION_MAJOR               VERSION_MAJOR
//#define VERSION_MINOR               VERSION_MINOR
#define VERSION_REVISION            VERSION_BUILDNO  // source control or manual revision.
#define VERSION_BUILD               VERSION_EXTEND  //times built

#ifdef _WIN64
#define BITSFLAG "(x64)"
#else
#define BITSFLAG "(x86)"
#endif
// ------------------------------------------------------------------------------

// Mappings for final rc  use.


#define VER_FILE_DESCRIPTION_STR    "VO " BITSFLAG
#define VER_FILE_VERSION            VERSION_FILE
#define VER_FILE_VERSION_STR        VERSION_FILESTR

#define VER_PRODUCTNAME_STR         "VO"  BITSFLAG
#define VER_PRODUCT_VERSION         VERSION_PRODUCT
#define VER_PRODUCT_VERSION_STR     VERSION_PRODUCTSTR
#define VER_ORIGINAL_FILENAME_STR   VER_PRODUCTNAME_STR ".dll"
#define VER_INTERNAL_NAME_STR       VER_ORIGINAL_FILENAME_STR
#define VER_COPYRIGHT_STR           "2014 Paul Dolcet"

#ifdef _DEBUG
#define VER_VER_DEBUG             VS_FF_DEBUG
#else
#define VER_VER_DEBUG             0
#endif

#define VER_FILEOS                  VOS_NT_WINDOWS32
#define VER_FILEFLAGS               VER_VER_DEBUG
#define VER_FILETYPE                VFT_APP

#define VS_VERSION_INFO				1



#endif

#endif
