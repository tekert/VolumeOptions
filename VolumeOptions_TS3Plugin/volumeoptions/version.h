#ifndef AT_VERSION_H_INCLUDED
#define AT_VERSION_H_INCLUDED

/*-----------------------------------------------------------
VERSION CONTROL BUILD SYSTEM
This header file was modified by VERBUILD v1.0.1
-----------------------------------------------------------
help : verbuild -?
info : http://www.yeamaec.com
yeamaec@hanafos.com ,krkim@yeamaec.com
Customized by Paul Dolcet.
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

// START verbuild generated numbers
#define VERSION_FULL           0.8.334.28

#define VERSION_DATE           "2014-12-01"
#define VERSION_TIME           "23:19:08"
#define VERSION_BASEYEAR       2014

#define VERSION_MAJOR          0
#define VERSION_MINOR          8
#define VERSION_BUILDNO        334
#define VERSION_EXTEND         28

#define VERSION_FILE           0,8,334,28
#define VERSION_PRODUCT        0,8,334,28
#define VERSION_FILESTR        "0,8,334,28"
#define VERSION_PRODUCTSTR     "0,8,334,28"
// END verbuild generation


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


#endif
