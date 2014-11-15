
#include <stdlib.h>

#include "../volumeoptions/sound_plugin.h"


int main2(int argc, char* argv[])
{

    volume_options_settings settings;
    
    // fill settings, you can change them anytime
    settings.monitor_settings.ses_global_settings.vol_reduction = 0.5f;

	// Example interface for talk software
    VolumeOptions* vo = new VolumeOptions(settings, ".");

    unsigned __int64 channelID = 0;
    unsigned __int64 clientID = 0;
	for (;;)
	{
        vo->process_talk(true, channelID, clientID); // someone is talking, push the stack if this is the first one, start vol reduction

        // change volume on the fly.
        settings.monitor_settings.ses_global_settings.vol_reduction = 0.7f;
        vo->set_settings(settings);

        vo->process_talk(true, channelID, clientID); // someone is talking, just push the stack

		system("PAUSE");

        vo->process_talk(false, channelID, clientID); // someone stopped talking, pop the stack = non empty, do nothing.
        vo->process_talk(false, channelID, clientID); // stack is empty, restores volume

		system("PAUSE");
	}


	system("PAUSE");
}