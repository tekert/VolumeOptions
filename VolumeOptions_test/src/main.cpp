

#include "../volumeoptions/sound_plugin.h"


int main2(int argc, char* argv[])
{

	// Interface for talk software
	VolumeOptions* vo = new VolumeOptions(0.5f, ".");

	for (;;)
	{
		vo->process_talk(true); // someone is talking, reduce volume
		system("PAUSE");
		vo->process_talk(false); // all are quiet, restores volume
		system("PAUSE");
	}


	system("PAUSE");
}