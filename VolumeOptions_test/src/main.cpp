
#include <stdlib.h>

#include "../volumeoptions/sound_plugin.h"

// TODO AudioMonitor example

// Change main2 to main to compile
int main2(int argc, char* argv[])
{
    vo::volume_options_settings settings;
    
    // fill settings, you can change them anytime
    settings.monitor_settings.ses_global_settings.vol_reduction = 0.4f;

	// Example interface for ts3 talk software,
    VolumeOptions* vo = new VolumeOptions("."); // will load config file
    //VolumeOptions* vo = new VolumeOptions(settings); // will load supplied settings directly
    // settings is updated with the actual settings applied on creation.

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
        // settings is updated with the actual settings applied on return.

        // client 1 starts talking too
        channelID = 0; clientID = "1";
        vo->process_talk(true, uniqueServerID, channelID, clientID);

		system("PAUSE");

        // disable clients on the fly
        vo->set_client_status("0", VolumeOptions::DISABLED);

        // client 1 stops talking, vol reduction will be deactivated, because client 0 is the last one and
        //      its disabled
        channelID = 0; clientID = "1";
        vo->process_talk(false, uniqueServerID, channelID, clientID);

        // client 0 stops talking, nothing will happend because its disabled.
        channelID = 0; clientID = "0";
        vo->process_talk(false, uniqueServerID, channelID, clientID); // client 1 stops talking, now vol reduction will be deactivated.

        system("PAUSE");

        vo->set_client_status("0", VolumeOptions::ENABLED);
        // we should be at default level here


        // More examples

        system("PAUSE");
        vo->set_channel_status(uniqueServerID, 1, VolumeOptions::DISABLED);
        system("PAUSE");
        vo->process_talk(true, uniqueServerID, 1, "2");
        system("PAUSE");
        vo->set_channel_status(uniqueServerID, 1, VolumeOptions::ENABLED);
        system("PAUSE");
        vo->process_talk(false, uniqueServerID, 0, "2");
        system("PAUSE");
        // we should be at default level here

        vo->process_talk(true, uniqueServerID, 0, "1");
        vo->process_talk(true, uniqueServerID, 0, "2");
        vo->process_talk(true, uniqueServerID, 0, "3");
        vo->process_talk(true, uniqueServerID, 0, "4");
        system("PAUSE");
        vo->set_client_status("1", VolumeOptions::DISABLED);
        system("PAUSE");
        vo->process_talk(false, uniqueServerID, 0, "2");
        system("PAUSE");
        vo->set_channel_status(uniqueServerID, 0, VolumeOptions::DISABLED); // volume should be restored here
        system("PAUSE");
        vo->process_talk(false, uniqueServerID, 0, "1");    // no effect, 3 and 4 are talking but channel is disabled
        system("PAUSE");
        vo->process_talk(false, uniqueServerID, 0, "4");    // no effect, 3 is talking but channel is disabled
        system("PAUSE");
        vo->set_channel_status(uniqueServerID, 0, VolumeOptions::ENABLED); // audio monitor is running now. 3 is talking
        system("PAUSE");
        vo->process_talk(false, uniqueServerID, 0, "3"); // audio monitor y paused now, no one is talking
        system("PAUSE");
        vo->set_client_status("1", VolumeOptions::ENABLED);
        system("PAUSE");
        // we should be at default level here
	}


	system("PAUSE");
}