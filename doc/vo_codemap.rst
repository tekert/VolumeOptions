=======================================
volumeoptions internal code explanation
=======================================

:Author: Paul Dolcet, tekert@gmail.com
:Version: 0.7.x

.. contents:: Table of contents
  :terminology
  :structure
  :threads

terminology
===========

Windows:
========
	Audio Endpoints:
		http://msdn.microsoft.com/en-us/library/dd370793%28v=vs.85%29.aspx
		
	Audio Sessions:
		http://msdn.microsoft.com/en-us/library/dd370796%28v=vs.85%29.aspx
		
From the APIS provided bt microsoft
	http://msdn.microsoft.com/en-us/library/dd370805%28v=vs.85%29.aspx
	
We only use WASAPI apis to control audio.

Basicaly endpoints are "hardware" audio devices and sessions are an abstraction of groups of
individual audio streams associated with a process.

A session manager interface provides info about current audio sessions.
The "2" on WASAPI interfaces class names is made to separate different OS support.
for example IAudioSessionManager2 only works on windows7+ 2008R2+ and IAudioSessionManager vista+

In this project we need events, events are interfaces that provide callbacks, we can get this tipe of events
for sessions: http://msdn.microsoft.com/en-us/library/dd368289%28v=vs.85%29.aspx
for new sessions: http://msdn.microsoft.com/en-us/library/dd370969%28v=vs.85%29.aspx

The reason these events are important is because it lets us add or change volume, based on user actions on
current registered events on audio sessions.
There are important considerations when using events as explained in MSDN and in code. specially with threads.

Thats the reason we use async calls from these callbacks, more info below.


structure
=========

high level map:

	+---------------------------+                +----------------+
	|SessionNotificationCallback|...             |  VolumeOptions |    <----------Talk software callbacks
	+---------------------------+  .             +----------------+ 
	+---------------------+        .                   |
	|SessionEventsCallback|        .                   |
	+---------------------+ ........                   |
           .                     .                   |
           .                +--------------+         |
           .                | AudioMonitor |<--------+
           .                +--------------+  Only one instance.
           .                       |
	+--------------+                 |
	| AudioSession |                 |
	+--------------+                 |
	| AudioSession |                 |
	+--------------+ m_saved_sessions|
	| AudioSession | <---------------+
	+--------------+


AudioMonitor  (thread safe)
------------
  Runs in its own thread and uses asio io_service to execute ready handlers.
we use asio io_service to queue calls from windows callbacks due to wasapi rules (insted of creating a mutex
for every case and maintain new code to adapt).
 
  Here we store sessions added, sessions are added automaticaly acording to current settings 
(of audio monitor). Sessions are added via callbacks or enumeration (refresh gets all SndVol sessions)
we also store relative info to settings like delayed volume change etc, more info in code.

  Only one instance of this class can be created for each audio endpoint since we are controling a single
interface OS wide.

  The class is thread safe, as said, it runs in its own thread and all calls are sync'd with its own thread.
in the case of windows callbacks, calls are async, to private methods (callbacks interfaces are friend)
its important to always async calls from these callbacks.
  A small proxy class with static methods was created to limit wich private methods the callback interfaces
can access. (class AudioCallbackProxy)

  
SessionNotificationCallback  (thread safe)
---------------------------

  Has a reference to wich monitor it belongs to report any new session detected.

SessionEventsCallback  (thread safe)
---------------------

  Has both audio monitor reference and audio sessions weak_ptr to wich it belongs to report any new status.
   (audio monitor reference is needed to get its asio::io_service and async calls with his thread)

AudioSessions  (thread safe only when AudioMonitor manages it)
-------------

  This class cant be created by anyone exept audio monitor, this is because it need a reference to belonging
audio monitor, so it can fetch its settings and register events with audio monitor's reference, it also needs
current settings from audio monitor, and most importantly, wasapi wont report expired sessions as long as we
retaing a reference to any wasapi object (whats the use right?) so we cant notify the user if the session is
no longer valid, we have to use it internaly.

	Thats why audiomonitor has expire timers to delete old inactive saved sessions, the session has time points
of when it was last modified and when was the last time it was active, etc. With that info we can mimic SndVol
expires, on testing i saw that it takes 2min of a closed process audio session to delete itself from the
AudioManager enumerator, we do a similar thing here, inactive sesesions are deleted, if they come active again
and we dont have any reference to that session we will receive a new session notification.


VolumeOptions  (thread safe)
-------------

  Plugin interface adapted for talk software, it uses audio monitor public methods and settings.



threads
=======

* AudioMonitor own thread
  it locks the entire class on creation and no thread can access it exepto its own.


* Windows session events callback thread
  pops when an events arrives and cant be stopped per microsoft rules.
  
  
* Windows session notification callback thread
  pops when an events arrives and cant be stopped per microsoft rules.


* main user thread/s, handles VolumeOptions