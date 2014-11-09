#ifndef SOUND_WINDOWS_H
#define SOUND_WINDOWS_H

#ifdef _WIN32

#include <SDKDDKVer.h>

#include "Audiopolicy.h"
#include "Mmdeviceapi.h"
#include "Endpointvolume.h"

#include <unordered_map>
#include <memory>
#include <mutex>


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

	AudioSessions();
	~AudioSessions();

	HRESULT RestoreSessions();
	// if changevol is false it only saves sessions
	HRESULT ProcessSessions(const DWORD cpid, bool change_vol = false, float vol_reduction = 0.0f);

private:
	HRESULT CreateSessionManager();

	HRESULT SaveSession(IAudioSessionControl* pSessionControl, ISimpleAudioVolume* pSimpleAudioVolume, const std::wstring& siid);
	HRESULT ChangeVolume(ISimpleAudioVolume* pSimpleAudioVolume, float vol_reduction);
	//HRESULT RestoreSession(ISimpleAudioVolume* pSimpleAudioVolume, const std::wstring& siid); // DEPRECATED

	// TODO: filter settings, PIDs, etc

	IAudioSessionManager2* m_pSessionManager2;
	IAudioSessionNotification* m_pSessionEvents;
	friend class CSessionNotifications;

	struct _AudioSessionSnap
	{
		std::shared_ptr<AudioSession> ae; // containst a ref. to seesioncontrol pointer and registers events
		float v; // user volume
	} AudioSessionSnap;
	// Defaults sound volume map per aplication for current user. SIID (SessionInstanceIdentifier) -> volume, sessionPointer
	std::unordered_map<std::wstring, _AudioSessionSnap> m_sound_defaults;

	std::recursive_mutex m_mutex;
};


#endif

#endif