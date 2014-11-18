#ifndef VO_VERSION_WIN_H_INCLUDED
#define VO_VERSION_WIN_H_INCLUDED

// file versioning for windows
#ifdef _WIN32

#include "windows.h"
#include "../volumeoptions/version.h"


// VERSION_ are auto generated
// "PROJECTNAME"_ are for internal use
// VER_ are for resource use.

#if defined( _WIN64 )
#define VO_BITSFLAG "(x64)"
#else
#define VO_BITSFLAG "(x86)"
#endif

// ------------------------------------------------------------------------------

// Mappings for final rc  use.

#define VER_FILE_DESCRIPTION_STR    "VolumeOptions Plugin for TS3 " VO_BITSFLAG
#define VER_FILE_VERSION            VERSION_FILE
#define VER_FILE_VERSION_STR        VERSION_FILESTR

#define VER_PRODUCTNAME_STR         "VolumeOptions Plugin "  VO_BITSFLAG
#define VER_PRODUCT_VERSION         VERSION_PRODUCT
#define VER_PRODUCT_VERSION_STR     VERSION_PRODUCTSTR
#define VER_ORIGINAL_FILENAME_STR   "volume_options_ts3plugin" VO_BITSFLAG ".dll"
#define VER_INTERNAL_NAME_STR       VER_ORIGINAL_FILENAME_STR
#define VER_COPYRIGHT_STR           "2014 Paul Dolcet tekert@gmail.com"

#ifdef _DEBUG
#define VER_VER_DEBUG             VS_FF_DEBUG
#else
#define VER_VER_DEBUG             0
#endif

#define VER_FILEOS                  VOS_NT_WINDOWS32
#define VER_FILEFLAGS               VER_VER_DEBUG
#define VER_FILETYPE                VFT_DLL

#endif

#endif
