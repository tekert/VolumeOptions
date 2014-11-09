// Including SDKDDKVer.h defines the highest available Windows platform.

// If you wish to build your application for a previous Windows platform, include WinSDKVer.h and
// set the _WIN32_WINNT macro to the platform you wish to support before including SDKDDKVer.h.

#ifdef _WIN32
#include <SDKDDKVer.h>

#include "Audiopolicy.h"
#include "Mmdeviceapi.h"
#include "Endpointvolume.h"

#include "../volumeoptions/sounds_windows.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "Advapi32.lib")


////////////////////////////////////// Audio Session //////////////////////////////////////

class CAudioSessionEvents : public IAudioSessionEvents
{
	LONG _cRef;

	~CAudioSessionEvents() {}

public:
	CAudioSessionEvents() 
		: _cRef(1)
	{}

	// IUnknown methods -- AddRef, Release, and QueryInterface

	ULONG STDMETHODCALLTYPE AddRef()
	{
		return InterlockedIncrement(&_cRef);
	}

	ULONG STDMETHODCALLTYPE Release()
	{
		ULONG ulRef = InterlockedDecrement(&_cRef);
		if (0 == ulRef)
		{
			delete this;
		}
		return ulRef;
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(
		REFIID  riid,
		VOID  **ppvInterface)
	{
		if (IID_IUnknown == riid)
		{
			AddRef();
			*ppvInterface = (IUnknown*)this;
		}
		else if (__uuidof(IAudioSessionEvents) == riid)
		{
			AddRef();
			*ppvInterface = (IAudioSessionEvents*)this;
		}
		else
		{
			*ppvInterface = NULL;
			return E_NOINTERFACE;
		}
		return S_OK;
	}

	// Notification methods for audio session events

	HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(
		LPCWSTR NewDisplayName,
		LPCGUID EventContext)
	{
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnIconPathChanged(
		LPCWSTR NewIconPath,
		LPCGUID EventContext)
	{
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(
		float NewVolume,
		BOOL NewMute,
		LPCGUID EventContext)
	{
#ifdef _DEBUG
		if (NewMute)
		{
			printf("MUTE\n");
		}
		else
		{
			printf("Volume = %d percent\n",
				(UINT32)(100 * NewVolume + 0.5));
		}
#endif
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(
		DWORD ChannelCount,
		float NewChannelVolumeArray[],
		DWORD ChangedChannel,
		LPCGUID EventContext)
	{
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(
		LPCGUID NewGroupingParam,
		LPCGUID EventContext)
	{
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnStateChanged(
		AudioSessionState NewState)
	{
#ifdef _DEBUG
		char *pszState = "?????";

		switch (NewState)
		{
		case AudioSessionStateActive:
			pszState = "active";
			break;
		case AudioSessionStateInactive:
			pszState = "inactive";
			break;
		case AudioSessionStateExpired: // only shows if we dont retaing a pointer to the session
			pszState = "expired";
			break;
		}
		printf("New session state = %s\n", pszState);
#endif
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnSessionDisconnected(
		AudioSessionDisconnectReason DisconnectReason)
	{
#ifdef _DEBUG
		char *pszReason = "?????";

		switch (DisconnectReason)
		{
		case DisconnectReasonDeviceRemoval:
			pszReason = "device removed";
			break;
		case DisconnectReasonServerShutdown:
			pszReason = "server shut down";
			break;
		case DisconnectReasonFormatChanged:
			pszReason = "format changed";
			break;
		case DisconnectReasonSessionLogoff:
			pszReason = "user logged off";
			break;
		case DisconnectReasonSessionDisconnected:
			pszReason = "session disconnected";
			break;
		case DisconnectReasonExclusiveModeOverride:
			pszReason = "exclusive-mode override";
			break;
		}
		printf("Audio session disconnected (reason: %s)\n",
			pszReason);
#endif
		return S_OK;
	}
};

class CSessionNotifications : public IAudioSessionNotification
{
private:

	LONG m_cRefAll;
	AudioSessions* m_pas;

	~CSessionNotifications() {};

public:

	CSessionNotifications(AudioSessions* pas)
		: m_cRefAll(1)
		, m_pas(pas) // TODO: CUIDADO! rehaer esto, si una notificacion llega justo cuando destruimos AudioSessions en su destructor, paf!
	{}

	// IUnknown
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvInterface)
	{
		if (IID_IUnknown == riid)
		{
			AddRef();
			*ppvInterface = (IUnknown*)this;
		}
		else if (__uuidof(IAudioSessionNotification) == riid)
		{
			AddRef();
			*ppvInterface = (IAudioSessionNotification*)this;
		}
		else
		{
			*ppvInterface = NULL;
			return E_NOINTERFACE;
		}
		return S_OK;
	}

	ULONG STDMETHODCALLTYPE AddRef()
	{
		return InterlockedIncrement(&m_cRefAll);
	}

	ULONG STDMETHODCALLTYPE Release()
	{
		ULONG ulRef = InterlockedDecrement(&m_cRefAll);
		if (0 == ulRef)
		{
			delete this;
		}
		return ulRef;
	}

	HRESULT OnSessionCreated(IAudioSessionControl *pNewSessionControl)
	{
		if (pNewSessionControl)
		{
#ifdef _DEBUG
			printf("New Session Created\n");
#endif

			//TODO: ver como obtener una referencia al AudioSessions de VolumeOptions.
		
		}
		return S_OK;
	}
};

AudioSession::AudioSession(IAudioSessionControl *pSessionControl)
{
	if (pSessionControl == NULL)
	{
		m_hrStatus = E_POINTER;
		return;
	}

	m_hrStatus = S_OK;
	m_pSessionControl = pSessionControl;
	m_pAudioEvents = new CAudioSessionEvents; // AddRef() on constructor

	m_pSessionControl->AddRef(); // Important: retaining a copy to a WASAPI session interface so the session never expires until released.

	// RegisterAudioSessionNotification calls AddRef on m_pAudioEvents
	//m_hrStatus = m_pSessionControl->RegisterAudioSessionNotification(m_pAudioEvents);

	//NOTE: retaining a copy of IAudioSessionControl causes the session to never expire
}

AudioSession::~AudioSession()
{
	if (m_pSessionControl != NULL)
	{
		//m_pSessionControl->UnregisterAudioSessionNotification(m_pAudioEvents);
	}
	SAFE_RELEASE(m_pAudioEvents);
	SAFE_RELEASE(m_pSessionControl);
}



//////////////////////////////////////   Audio Sessions //////////////////////////////////////

AudioSessions::AudioSessions()
{
	HRESULT hr = CreateSessionManager();
}

AudioSessions::~AudioSessions()
{
	if (m_pSessionManager2 != NULL)
		m_pSessionManager2->UnregisterSessionNotification(m_pSessionEvents);
	SAFE_RELEASE(m_pSessionEvents);
	SAFE_RELEASE(m_pSessionManager2);
}

HRESULT AudioSessions::CreateSessionManager()
{
	HRESULT hr = S_OK;

	IMMDevice* pDevice = NULL;
	IMMDeviceEnumerator* pEnumerator = NULL;

	hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

	// Create the device enumerator.
	hr = CoCreateInstance(
		__uuidof(MMDeviceEnumerator),
		NULL, CLSCTX_ALL,
		__uuidof(IMMDeviceEnumerator),
		(void**)&pEnumerator);

	if (pEnumerator == NULL)
		return S_FALSE; // error creating instance

	// Get the default audio device.
	hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);

	// Get the session manager.
	hr = pDevice->Activate(
		__uuidof(IAudioSessionManager2), CLSCTX_ALL,
		NULL, (void**)&m_pSessionManager2);

	//TODO:
	// http://msdn.microsoft.com/en-us/library/dd370956%28v=vs.85%29.aspx
	// para bajar volumen a las nuevas sesiones mientras uno habla
	if (m_pSessionManager2 != NULL)
	{
		m_pSessionEvents = new CSessionNotifications(this); // AddRef() on constructor
		m_pSessionManager2->RegisterSessionNotification(m_pSessionEvents);
	}

	// Clean up.
	SAFE_RELEASE(pEnumerator);
	SAFE_RELEASE(pDevice);

	return hr;
}


inline
HRESULT AudioSessions::SaveSession(IAudioSessionControl* pSessionControl, ISimpleAudioVolume* pSimpleAudioVolume, const std::wstring& siid)
{
	HRESULT hr = S_OK;

	float v;
	hr = pSimpleAudioVolume->GetMasterVolume(&v);

	_AudioSessionSnap ass;
	ass.ae.reset(new AudioSession(pSessionControl));
	ass.v = v;

	m_sound_defaults[siid] = ass;

	return hr;
}

inline
HRESULT AudioSessions::ChangeVolume(ISimpleAudioVolume* pSimpleAudioVolume, float vol_reduction)
{
	HRESULT hr = S_OK;

	float v;
	hr = pSimpleAudioVolume->GetMasterVolume(&v);

	//LPCGUID my_guid;
	hr = pSimpleAudioVolume->SetMasterVolume(v * ((float)1.0 - vol_reduction), NULL);

	return hr;
}

// DEPRECATED
#if 0
inline
HRESULT AudioSessions::RestoreSession(ISimpleAudioVolume* pSimpleAudioVolume, const std::wstring& siid)
{
	HRESULT hr = S_OK;

	// dont touch non existant sessions (new session could be added when the client was talking)
	if (m_sound_defaults.find(siid) == m_sound_defaults.end())
		return hr;

	_AudioSessionSnap ass = m_sound_defaults[siid];
	hr = pSimpleAudioVolume->SetMasterVolume(ass.v, NULL);

	return hr;
}
#endif

HRESULT AudioSessions::RestoreSessions()
{
	HRESULT hr = S_OK;

	// Restore all our saved sessions
	for (std::unordered_map<std::wstring, _AudioSessionSnap>::iterator iter = m_sound_defaults.begin(); iter != m_sound_defaults.end(); ++iter)
	{
		_AudioSessionSnap ass = iter->second;

		ISimpleAudioVolume* pSimpleAudioVolume;
		hr = ass.ae->m_pSessionControl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&pSimpleAudioVolume);

		hr = pSimpleAudioVolume->SetMasterVolume(ass.v, NULL);

		SAFE_RELEASE(pSimpleAudioVolume);
	}

	return hr;
}

// TODO: move filter settings to class, and create a set method
HRESULT AudioSessions::ProcessSessions(const DWORD cpid, bool change_vol, float vol_reduction)
{
	if (!m_pSessionManager2)
		return E_INVALIDARG;

	HRESULT hr = S_OK;

	int cbSessionCount = 0;

	IAudioSessionEnumerator* pSessionList = NULL;
	IAudioSessionControl* pSessionControl = NULL;

	// Delete map before overwriting save
	m_sound_defaults.clear();

	// Get the current list of sessions.
	// IMPORTANT NOTE: DONT retain references to IAudioSessionControl before calling this function, it causes memory leaks
	//		thats why we delete m_sound_defaults before too.
	hr = m_pSessionManager2->GetSessionEnumerator(&pSessionList);

	// Get the session count.
	hr = pSessionList->GetCount(&cbSessionCount);

	for (int index = 0; index < cbSessionCount; index++)
	{
		// Get the <n>th session.
		hr = pSessionList->GetSession(index, &pSessionControl);

		// Get two important interfaces to query for info on current session.
		IAudioSessionControl2* pSessionControl2;
		hr = pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pSessionControl2);
		ISimpleAudioVolume* pSimpleAudioVolume;
		hr = pSessionControl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&pSimpleAudioVolume);

		//TODO: para detectar sistem sounds como parametro de crash
		// http://msdn.microsoft.com/en-us/library/windows/desktop/dd368257%28v=vs.85%29.aspx
		//hr = pSessionControl2->IsSystemSoundsSession();

		DWORD pid;
		hr = pSessionControl2->GetProcessId(&pid);
#ifdef _DEBUG		
		LPWSTR sid;
		hr = pSessionControl2->GetSessionIdentifier(&sid); // This one is NOT unique
		std::wstring cpp_sid(sid);
		CoTaskMemFree(sid);
#endif		
		LPWSTR siid;
		hr = pSessionControl2->GetSessionInstanceIdentifier(&siid); // This one is unique
		std::wstring cpp_siid(siid);
		CoTaskMemFree(siid);

		// If pid is not team speak
		if (pid != cpid)
		{
			hr = SaveSession(pSessionControl, pSimpleAudioVolume, cpp_siid);
			if (change_vol)
				hr = ChangeVolume(pSimpleAudioVolume, vol_reduction);
		}

		// DEBUG:
#ifdef _DEBUG
		LPWSTR pswSession = NULL;
		float v;
		hr = pSessionControl->GetDisplayName(&pswSession);
		pSimpleAudioVolume->GetMasterVolume(&v);
		wprintf_s(L"Session Name: %s [%d] ret:[%d]\n pid: %d\n sid: %s\n ssid: %s\n Volume: %f\n\n", pswSession, index, hr, pid, cpp_sid.c_str(), cpp_siid.c_str(), v);
		CoTaskMemFree(pswSession);
#endif

		SAFE_RELEASE(pSessionControl);
		SAFE_RELEASE(pSimpleAudioVolume);
		SAFE_RELEASE(pSessionControl2);
	}

	SAFE_RELEASE(pSessionList);

	return hr;
}


#endif