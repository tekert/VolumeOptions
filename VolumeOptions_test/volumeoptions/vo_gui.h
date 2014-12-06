
#ifndef WINDOWS_PLUGIN_GUI_H
#define WINDOWS_PLUGIN_GUI_H

#include "../volumeoptions/vo_ts3plugin.h"

// parent can be null
int DialogThread(vo::VolumeOptions* vo, void* parent = nullptr);
int DialogThread2();


#endif