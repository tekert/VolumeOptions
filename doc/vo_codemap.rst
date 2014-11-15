==============================
volumeoptions code explanation
==============================

:Author: Paul Dolcet, tekert@gmail.com
:Version: 0.6.x

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
Number 2 on WASAPI interfaces after the name of the class is made to separate different OS support.
for example IAudioSessionManager2 only works on windows7+ 2008R2+ and IAudioSessionManager vista+

In this code we need events, events are interfaces that provide callbacks, we can get this tipe of events
for sessions: http://msdn.microsoft.com/en-us/library/dd368289%28v=vs.85%29.aspx
for new sessions: http://msdn.microsoft.com/en-us/library/dd370969%28v=vs.85%29.aspx

The reason these events are important is because it lets us add or change volume based on user actions on
current registered events for audio sessions.
There are important considerations when using events as explained in MSDN and in code. specially with threads.

Thats the reason we use async calls from these callbacks, more info below.


structure
=========

high level map:

	+---------------------------+                +----------------+
	|SessionNotificationCallback|                |  VolumeOptions |
	+---------------------------+                +----------------+ 
	+---------------------+                            |
	|SessionEventsCallback|                            |
	+---------------------+                            |
                                                     |
                            +--------------+         | Only on instance.
                            | AudioMonitor |<--------+
                            +--------------+  
                                   |
	+--------------+                 |
	| AudioSession |                 |
	+--------------+                 |
	| AudioSession |                 |
	+--------------+ m_saved_sessions|
	| AudioSession | <---------------+
	+--------------+


AudioMonitor
------------
  Runs in its own thread and uses asio io_service to execute ready handlers.
 we use asio io_service to queue calls from windows callbacks due to wasapi rules and our code flow.
 
  Here we store sessions added, sessions are added automaticaly acording to current settings 
of audio monitor. sessions are added via callbacks or enumeration (refresh gets all SndVol sessions)
we also store relative info to settings like delayed volume change etc, more info in code.

  Only one instance of this class can be created for each audio endpoint since we are controling a single
interface OS wide. (currently not done)

  The class is thread safe, as said, it runs in its own thread and all calls are sync'd with its own thread.
in the case of windows callbacks, calls are async, to private methods (callbacks interfaces are friend)
its important to always async calls from these callbacks.
  A small proxy class with static methods was created to limit wich private methods the callback interfaces
can access. (class AudioCallbackProxy)

  
SessionNotificationCallback
---------------------------

  Contains a reference to wich monitor it belongs to report any new session detected.

SessionEventsCallback
---------------------

  Contaings both audio monitor and audio sessions it belongs to to report any new status.
   (audio monitor reference is needed to get its asio::io_service and async calls)

AudioSessions
-------------

  This class cant be created by anyone exept audio monitor, this is because it need a reference to wich
audio monitor it belongs, so it can fetch its settings and register events with monitor's reference.

TODO: continue explaining. this is the most important.

VolumeOptions
-------------

  Plugin like interface to adapt it on different software, it uses audio monitor public methods and settings.



threads
=======

* AudioMonitor own thread
  it locks the entire class on creation and no thread can access it exepto its own.


* Windows session events callback thread
  pops when an events arrives and cant be stopped per microsoft rules.
  
  
* Windows session notification callback thread
  pops when an events arrives and cant be stopped per microsoft rules.


* main user thread/s, handles VolumeOptions