
#include <stdlib.h>

#include "../volumeoptions/sound_plugin.h"


int main2(int argc, char* argv[])
{

    volume_options_settings settings;
    
    // fill settings, you can change them anytime
    settings.monitor_settings.ses_global_settings.vol_reduction = 0.5f;

	// Example interface for talk software
    VolumeOptions* vo = new VolumeOptions(settings, ".");

	for (;;)
	{
		vo->process_talk(true); // someone is talking, push the stack if this is the first one, start vol reduction

        // change volume on the fly.
        settings.monitor_settings.ses_global_settings.vol_reduction = 0.7f;
        vo->change_settings(settings);

        vo->process_talk(true); // someone is talking, just push the stack

		system("PAUSE");

		vo->process_talk(false); // someone stopped talking, pop the stack = non empty, do nothing.
        vo->process_talk(false); // stack is empty, restores volume

		system("PAUSE");
	}


	system("PAUSE");
}