VolumeOptions
=============

Description:
  Module to replace windows audio ducking during comunications.

Compatibility:
  Compatible with Windows 7+ and Windows Server R2+

Requieres boost::asio and boost::system library to build.
Built using Visual Studio 2013, Win32 and x64
Solution and projects ready to build on windows.

Its a plugin like interface for communication software or any application that needs to lower volume of other applications with customization. It uses WASAPI to work on sessions mostly (SndVol sessions), not individual streams (yet).
Its configured on default to reduce volume of other sessions conserving default user volume on restore, on windows it supports auto detection of new sessions, delete expired sessions, detects duplicate audio processes and acts acordingly also fixing some windows perks about default volume of repeated sessions but always restores to correct user volume. Its built to not mess with user configuration in any way exept if main software where the plugin is used crashes (wich almost never happens exept on blackout), in that case there is nothing we can do exept wait for user to manualy readjust.

Currently in development phase for later integrating it on Team Speak 3 and other communication software for windows, maybe linux in near future.


Use:
(TODO: complete interface)

Class VolumeOptions in sound_plugin.h is the main class to use, adapt it to the app needs. Current header is an example for team speak 3 client plugin.


Status: Library development, Core done 95%. (missing some minor options)
Status: ts3 plugin interface, 50% done (missing gui screen).
Status: Configuration file: 60% done. (need to complete parsing from plugin gui)
