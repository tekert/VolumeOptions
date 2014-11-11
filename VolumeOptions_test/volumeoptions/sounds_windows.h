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
/*

	WINDOWS 7+ or Server 2008 R2+ only

	SndVol.exe auto volume manager
*/

#ifndef SOUND_WINDOWS_H
#define SOUND_WINDOWS_H

#ifdef _WIN32

#include <WinSDKVer.h>
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 // Minimum Win7 or Windows Server 2008 R2
#endif

#define VO_ENABLE_EVENTS //TODO: do we really need this macro? was originaly for vista support but...

#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>

#include <SDKDDKVer.h>
#include <Audiopolicy.h>

#include <unordered_map>
#include <utility>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>

#include "../volumeoptions/vo_settings.h"

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(x)	\
	if(x != NULL)		\
	{					\
		ULONG _n = x->Release();	\
		/* n ? x : */ x = NULL ;		\
	}
#endif


#ifdef _DEBUG

#ifndef CHECK_HR
#define CHECK_HR(hr) if (FAILED(hr)) { assert(false);}
#endif
inline ULONG CHECK_REFS(IUnknown *p)
{
	ULONG n = p->AddRef();
	n = p->Release();
	return n;
}
#else
#define CHECK_HR
#define CHECK_REFS(x) false
#endif


class AudioMonitor;
/*
	Represents a Single windows Audio Session
*/
class AudioSession : public std::enable_shared_from_this<AudioSession>
{
public:
	AudioSession(IAudioSessionControl *pSessionControl,
		AudioMonitor& m_AudioMonitor, float default_volume = -1.0f);
	AudioSession(const AudioSession &) = delete; // non copyable
	AudioSession& operator= (const AudioSession&) = delete; // non copyassignable
	~AudioSession();

	/* public methods here must be const, thread safe and non bloking */
	/* this class will be called from external callbacks sometimes through shared_ptr */
	/* and require non bloking actions, so.. no mutex alowed inside public methods */
	HRESULT GetStatus() const { return m_hrStatus; };

	std::wstring getSID() const;
	std::wstring getSIID() const;
	DWORD getPID() const;

private:

	void InitEvents();
	void StopEvents();

	float GetCurrentVolume();
	HRESULT ApplyVolumeSettings();	//TODO: mejor partir esta funcion, una en monitor y otra aca. ApplySessionVolumeSettings
	void UpdateDefaultVolume(float new_def);
	enum resume_t { NORMAL = false, NO_DELAY = true};
	HRESULT RestoreVolume(const resume_t callback_no_delay = NORMAL); //TODO: mejor partir esta funcion, una en monitor y otra aca. RestoreSesisonVolume
	void RestoreHolderCallback(const std::shared_ptr<AudioSession> spAudioSession, boost::system::error_code const& e = boost::system::error_code());
	void ChangeVolume(const float v);
	void touch(); // sets m_last_modified_on now().
	void set_time_active_since();
	void set_time_inactive_since();

	float m_default_volume; // always marks user default volume of this SID session
	bool is_volume_at_default;  // if true, session volume is at user default volume

	mutable std::atomic<HRESULT> m_hrStatus;

	std::chrono::steady_clock::time_point m_last_modified_on;
	std::chrono::steady_clock::time_point m_last_active_state;

	IAudioSessionControl* m_pSessionControl;
	IAudioSessionEvents *m_pAudioEvents;
	IAudioSessionControl2* m_pSessionControl2;
	ISimpleAudioVolume* m_pSimpleAudioVolume;
	AudioMonitor& m_AudioMonitor;  // To witch monitor it blongs

	/* Only this class can manage this object in thread safe way */
	friend class AudioMonitor;
	friend class AudioCallbackProxy;
};


/*
	Represents a windows Audio Manager, that containts current audio sessions

	With VO_ENABLE_EVENTS callbacks from events will post calls (async) to the thread polling
		from this class.
	Without VO_ENABLE_EVENTS, events will be disabled (no callbacks from windows audio), thats means it wont
		detect new sessions while monitor is active, and will lower volume of all applications
		instead of only the active ones if configured to do so.
*/
class AudioMonitor : public std::enable_shared_from_this<AudioMonitor>
{
public:
	/* Created with STOPPED status */
	// static std::shared_ptr<AudioMonitor> create(vo::monitor_settings&& settings) // if no variadic template support
	template<typename ...T>
	static std::shared_ptr<AudioMonitor> create(T&&... all)
	{
		return std::shared_ptr<AudioMonitor>(new AudioMonitor(std::forward<T>(all)...));
	}
	AudioMonitor(const AudioMonitor &) = delete; // non copyable
	AudioMonitor& operator= (const AudioMonitor&) = delete; // non copyassignable
	~AudioMonitor();

	float GetVolumeReductionLevel();
	void SetSettings(vo::monitor_settings& settings);

	/* If Resume is used shile Stopped will also Starts all events and refresh sessions. */
	long Stop(); // Stops all events and deletes all saved sessions.
	long Pause(); // Restores volume on all sessions and freezes volume change
	long Start(); // Resumes/Starts volume change and reaplies settings.
	long Refresh(); // Gets all current sessions in SndVol
#ifdef VO_ENABLE_EVENTS 
	long InitEvents();
	long StopEvents();
#endif
	enum monitor_status_t { STOPPED, RUNNING, PAUSED };
	monitor_status_t GetStatus();
	std::shared_ptr<boost::asio::io_service> get_io();

private:

	AudioMonitor(vo::monitor_settings& settings);

	void poll(); /* AudioMonitor thread loop */
	HRESULT CreateSessionManager();

	HRESULT RefreshSessions();
	void DeleteSessions();

	HRESULT SaveSession(IAudioSessionControl* pNewSessionControl, bool unref);
	void DeleteSession(std::shared_ptr<AudioSession> spAudioSession); // TODO: not used yet, sessions not expiring...
	void DeleteExpiredSessions(boost::system::error_code const& e, 
		std::shared_ptr<boost::asio::steady_timer> timer);
	void ApplyCurrentSettings();

	IAudioSessionManager2* m_pSessionManager2;
	IAudioSessionNotification* m_pSessionEvents;

	bool isSessionExcluded(DWORD pid, std::wstring sid = L"");

	// Settings
	DWORD m_processid;
	vo::monitor_settings m_settings;
	const std::chrono::seconds m_inactive_timeout;
	const std::chrono::seconds m_delete_expired_interval;

	// Used to delay or cancel all volume restores session_this_pointer -> timer
	std::unordered_map<const AudioSession*, std::shared_ptr<boost::asio::steady_timer>> m_pending_restores;

	bool auto_change_volume_flag;
	monitor_status_t m_current_status;

	// Sessions currently Monitored, map of SID -> list of AudioSession pointers with unique SIID (SessionInstanceIdentifier) 
	// You could look at it as group of different SIID sessions with the same SID.
	typedef std::unordered_multimap<std::wstring, std::shared_ptr<AudioSession>> t_saved_sessions;
	t_saved_sessions m_saved_sessions;
	typedef std::pair<std::wstring, std::shared_ptr<AudioSession>> t_session_pair;

	friend class AudioSession;
	friend class AudioCallbackProxy; /* To select wich private methods others classes can acess */

	/* To sync Events with main class without "blocking" (async) 
		or we cause mem leaks on simultaneous callbacks (confirmed) */
	std::shared_ptr<boost::asio::io_service> m_io;
	bool m_abort;
	std::thread m_thread_monitor; /* poll thread */

	std::recursive_mutex m_mutex;
};



#endif

#endif