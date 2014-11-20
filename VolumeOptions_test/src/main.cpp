
#include <stdlib.h>

#include "../volumeoptions/sound_plugin.h"

// TODO AudioMonitor example

// Change main2 to main to compile
int main2(int argc, char* argv[])
{
    vo::volume_options_settings settings;
    
    // fill settings, you can change them anytime
    settings.monitor_settings.ses_global_settings.vol_reduction = 0.5f;

	// Example interface for ts3 talk software
    VolumeOptions* vo = new VolumeOptions(settings, ".");

    VolumeOptions::channelID_t channelID;
    VolumeOptions::uniqueClientID_t clientID;
    VolumeOptions::uniqueServerID_t uniqueServerID = "etcetc...";
    // NOTE: this assumes every individual client talks and then stops talking always.
	for (;;)
	{
        // client 0 starts talking
        channelID = 0; clientID = "0";
        vo->process_talk(true, uniqueServerID, channelID, clientID);

        // change volume on the fly.
        settings.monitor_settings.ses_global_settings.vol_reduction = 0.7f;
        vo->set_settings(settings);

        // client 1 starts talking too
        channelID = 0; clientID = "1";
        vo->process_talk(true, uniqueServerID, channelID, clientID);

		system("PAUSE");

        // disable clients on the fly
        vo->set_client_status(0, VolumeOptions::DISABLED);

        // client 1 stops talking, vol reduction will be deactivated, because client 0 is the last one and
        //      its disabled
        channelID = 0; clientID = "1";
        vo->process_talk(false, uniqueServerID, channelID, clientID);

        // client 0 stops talking, nothing will happend because its disabled.
        channelID = 0; clientID = "0";
        vo->process_talk(false, uniqueServerID, channelID, clientID); // client 1 stops talking, now vol reduction will be deactivated.

		system("PAUSE");
	}


	system("PAUSE");
}