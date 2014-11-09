/*
Copyright (c) 2014, Paul Dolcet
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

	* Redistributions of source code must retain the above copyright notice, this
	list of conditions and the following disclaimer.

	* Redistributions in binary form must reproduce the above copyright notice,
	this list of conditions and the following disclaimer in the documentation
	and/or other materials provided with the distribution.

	* Neither the name of [project] nor the names of its
	contributors may be used to endorse or promote products derived from
	this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef AUDIO_SESSIONS_H
#define AUDIO_SESSIONS_H

#ifdef _WIN32

#include <SDKDDKVer.h>

#include "Audiopolicy.h"
#include "Mmdeviceapi.h"
#include "Endpointvolume.h"

#endif

#include <iostream>
#include <unordered_map>
#include <stack>
#include <mutex>
#include <memory>

#ifdef _WIN32

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(x)	\
	if(x != NULL)		\
	{					\
	x->Release();	\
	x = NULL;		\
	}

#define CHECK_HR(hr) if (FAILED(hr)) { goto done; }
#endif


class AudioSession
{
	HRESULT m_hrStatus;
	IAudioSessionEvents *m_pAudioEvents;

public:
	AudioSession(IAudioSessionControl *pSessionControl);
	~AudioSession();
	HRESULT GetStatus() { return m_hrStatus; };
	IAudioSessionControl *m_pSessionControl;
};


// For windows. TODO linux
class AudioSessions
{
public:

	AudioSessions(const DWORD cpid);
	~AudioSessions();

	HRESULT RestoreSessions();
	HRESULT ProcessSessions(bool change_vol, float vol_reduction);

private:
	HRESULT CreateSessionManager();

	HRESULT SaveSession(IAudioSessionControl* pSessionControl, const float default_vol_of_same_sid = -1.0f);
	HRESULT ChangeVolume(IAudioSessionControl* pSessionControl, float vol_reduction);
	//HRESULT RestoreSession(ISimpleAudioVolume* pSimpleAudioVolume, const std::wstring& siid);

	// TODO: filter settings, PIDs, etc
	const DWORD m_skippid;

	IAudioSessionManager2* m_pSessionManager2;
	IAudioSessionNotification* m_pSessionEvents;
	friend class CSessionNotifications;

	float m_current_vol_reduction; // if 0, current sessions are default, if >0 indicates the level of reduction currently aplied.

	struct _AudioSessionSnap
	{
		std::shared_ptr<AudioSession> ae;
		float v; // fast get default volume
	} AudioSessionSnap;

	// Defaults sound volume map per aplication for current user. SIID (SessionInstanceIdentifier) -> volume
	std::unordered_map<std::wstring, _AudioSessionSnap> m_sound_defaults;

	std::recursive_mutex m_mutex;
};


#endif

// For team speak
class VolumeOptions
{
public:

	VolumeOptions(const float v);
	~VolumeOptions();

	int process_talk(const bool talk_status);

	void save_default_volume();
	void restore_default_volume();
	void set_volume_reduction(const float v);
	float get_volume_reduction();

	void reset_data(); // not used.

private:
	AudioSessions m_as;

	DWORD m_cpid; // current process id, to filter.

	// Default user setting to reduce volume
	float m_vol_reduction;

	std::stack<bool> m_calls; // to count concurrent users talking
	bool m_quiet; // if noone is talking, this is true

	std::mutex mutex;
};



#endif