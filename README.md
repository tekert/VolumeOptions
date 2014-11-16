#VolumeOptions
===

###Description:
  Module to replace windows audio ducking during comunications.

####Compatibility:
  Compatible with Windows 7+ and Windows Server R2+

Requieres boost and boost::system library to build. (version 1.57.0)

Built using Visual Studio 2013, Win32 and x64

Solution and projects provided to build on windows Visual Studio 2013.

===
####About:

  Its a plugin like interface for communication software or any application that needs to lower volume of other applications with customization. It uses WASAPI to work on sessions mostly (SndVol sessions), not individual streams (yet).
  
  
  Its configured on default to reduce volume of other sessions conserving default user volume on restore, on windows it supports auto detection of new sessions, delete expired sessions, detects duplicate audio processes and acts acordingly, also fixing some windows perks about same process's session default volume.
  
  
  Its built to not mess with user configuration in any way, if software forced to end before shutdown sesions will be left in non default state, in that case there is nothing we can do exept wait for user to manualy readjust.

Currently in development phase for later integrating it on Team Speak 3 and other communication software for windows, maybe linux in near future.

####Build:
Edit vo_paths.props with the path to boost includes, 32 and 64 path libraries.
Launch solution and simply build. (boost::system is statically linked)

####Use:
(TODO: complete interface)

Class VolumeOptions in *sound_plugin.h* is the plugin class interface, can be addapted to other software. Current header is an example for team speak 3 client plugin.

Options for:  (in progress)
Monitor:
* Exclude or Include process names to monitor.
* Exclude or include own process.
Sessions:
* Change volume only in active audio sessions or not.
* Change volume by % or by fixed level.
* Delay (ms) volume restores to default user vol.
* Volume change level (can be up or down)


#####Windows library status:
* *Library development, Core 95% done* (missing per app custom vol option)

#####TS3 Plugin status:
* *TS3 plugin interface, 70% done* (missing gui config window, .ini only for now)
* *Configuration file: 80% done.* (parsing complete, need some sintax for last option)