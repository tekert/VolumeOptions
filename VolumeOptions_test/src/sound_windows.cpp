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
#ifdef _WIN32

#include <WinSDKVer.h>
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <boost/config.hpp>
#ifndef BOOST_NO_CXX11_VARIADIC_TEMPLATES
// Workaround, boost disabled this define for mscv 12 as a workaround, but mscv 12 supports it.
#if _MSC_VER == 1800
#define COMPILER_SUPPORT_VARIADIC_TEMPLATES
#endif
#else
#define COMPILER_SUPPORT_VARIADIC_TEMPLATES
#endif

// Only for local async calls, io_service
#include <boost/asio.hpp> // include Asio before including windows headers

#include <SDKDDKVer.h>
#include <Audiopolicy.h>
#include <Mmdeviceapi.h>
#include <Endpointvolume.h>

// Visual C++ pragmas, or add these libs manualy
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "Advapi32.lib")

#include <algorithm> // for string conversion
#include <thread> // for main monitor thread
#include <condition_variable>
#include <assert.h>

#include "../volumeoptions/sounds_windows.h"

#include <initguid.h> // for macro DEFINE_GUID definition http://support2.microsoft.com/kb/130869/en-us
/* zero init global GUID for events, for later on AudioMonitor constructor */
DEFINE_GUID(GUID_VO_CONTEXT_EVENT_ZERO, 0x00000000L, 0x0000, 0x0000,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
// {D2C1BB1F-47D8-48BF-AC69-7E4E7B2DB6BF}  For event distinction
static const GUID GUID_VO_CONTEXT_EVENT =
{ 0xd2c1bb1f, 0x47d8, 0x48bf, { 0xac, 0x69, 0x7e, 0x4e, 0x7b, 0x2d, 0xb6, 0xbf } };


/*
	C++ idiom : Friendship and the Attorney-Client
	To fine tune access to private members from callback classes for security
*/
class AudioCallbackProxy
{
private:
	static HRESULT SaveSession(std::shared_ptr<AudioMonitor> pam, IAudioSessionControl* pNewSessionControl, bool unref)
	{
		return pam->SaveSession(pNewSessionControl, unref);
	}
	static void DeleteSession(std::shared_ptr<AudioMonitor> pam, std::wstring ssid) // NOT USED, sessions not expiring
	{
		pam->DeleteSession(ssid);
	}

	static HRESULT ApplyVolumeSettings(std::shared_ptr<AudioSession> pas)
	{
		return pas->ApplyVolumeSettings();
	}
	static HRESULT RestoreVolume(std::shared_ptr<AudioSession> pas)
	{
		return pas->RestoreVolume();
	}
	static void UpdateDefaultVolume(std::shared_ptr<AudioSession> pas, float new_def)
	{
		pas->UpdateDefaultVolume(new_def);
	}
	static void set_time_inactive_since(std::shared_ptr<AudioSession> pas)
	{
		pas->set_time_inactive_since();
	}
	static void set_time_active_since(std::shared_ptr<AudioSession> pas)
	{
		pas->set_time_active_since();
	}

	/* Classes with access */
	friend class CAudioSessionEvents; /* needed for callbacks to access this class using async calls */
	friend class CSessionNotifications; /* needed for callbacks to access this class using async calls */
};


///////////// Some exported helpers for async calls from other proyects of mine /////////////////////

// Used to sync calls from other threads usign io_service. Used locally
namespace 
{
	std::recursive_mutex io_mutex;
	std::condition_variable_any cond; // semaphore.
	std::shared_ptr<boost::asio::io_service> g_pio; // pointer to AudioMonitor member only to use these handy macros


	template <class R>
	void ret_sync_queue(R* ret, bool* done, std::condition_variable_any* e, std::recursive_mutex* m, std::function<R(void)> f)
	{
		*ret = f();
		std::lock_guard<std::recursive_mutex> l(*m);
		*done = true;
		e->notify_all();
	}

	void sync_queue(bool* done, std::condition_variable_any* e, std::recursive_mutex* m, std::function<void(void)> f)
	{
		f();
		std::lock_guard<std::recursive_mutex> l(*m);
		*done = true;
		e->notify_all();
	}

	/* If compiler supports variadic templates use this, its nicer */
	/* perfect forwarding,  rvalue references */
#ifdef COMPILER_SUPPORT_VARIADIC_TEMPLATES

	template <typename ft, typename... pt>
	inline
	void ASYNC_CALL(ft&& f, pt&&... args)
	{
		g_pio->post(std::bind(std::forward<ft>(f), std::forward<pt>(args)...));
	}

	template <typename ft, typename... pt>
	inline
	void SYNC_CALL(ft&& f, pt&&... args)
	{
			bool done = false;
			g_pio->dispatch(std::bind(&sync_queue, &done, &cond, &io_mutex,
				std::function<void(void)>(std::bind(std::forward<ft>(f), std::forward<pt>(args)...))));

			std::unique_lock<std::recursive_mutex> l(io_mutex);
			while (!done) { cond.wait(l); };
			l.unlock();
	}

	template <typename rt, typename ft, typename... pt>
	inline
	rt SYNC_CALL_RET(ft&& f, pt&&... args)
	{
			bool done = false;
			rt r;
			g_pio->dispatch(std::bind(&ret_sync_queue<rt>, &r, &done, &cond, &io_mutex,
				std::function<rt(void)>(std::bind(std::forward<ft>(f), std::forward<pt>(args)...))));

			std::unique_lock<std::recursive_mutex> l(io_mutex);
			while (!done) { cond.wait(l); };
			l.unlock();
			return r;
	}

#else

	// Note: When all are released only the ones with the local bool set to true return, the others wait again.
#define VO_SYNC_WAIT \
	std::unique_lock<std::recursive_mutex> l(io_mutex); \
	while (!done) { cond.wait(l); }; \
	l.unlock();

	// Simple Wrappers for async without return types
#define VO_ASYNC_CALL(x) \
	g_pio->post(std::bind(& x))

#define VO_ASYNC_CALL1(x, p1) \
	g_pio->post(std::bind(& x, p1))

#define VO_ASYNC_CALL2(x, p1, p2) \
	g_pio->post(std::bind(& x, p1, p2))

#define VO_ASYNC_CALL3(x, p1, p2, p3) \
	g_pio->post(std::bind(& x, p1, p2, p3))

	// Note: These use dispatch in case a sync call is requested from within the same g_pio->run thread, the cond.wait will be ommited.
#define VO_SYNC_CALL(x) \
	bool done = false; \
	g_pio->dispatch(std::bind(&sync_queue, &done, &cond, &io_mutex, std::function<void(void)>(std::bind(& x)))); \
	VO_SYNC_WAIT

#define VO_SYNC_CALL1(x, p1) \
	bool done = false; \
	g_pio->dispatch(std::bind(&sync_queue, &done, &cond, &io_mutex, std::function<void(void)>(std::bind(& x, p1)))); \
	VO_SYNC_WAIT

#define VO_SYNC_CALL2(x, p1, p2) \
	bool done = false; \
	g_pio->dispatch(std::bind(&sync_queue, &done, &cond, &io_mutex, std::function<void(void)>(std::bind(& x, p1, p2)))); \
	VO_SYNC_WAIT

#define VO_SYNC_CALL_RET(type, x) \
	bool done = false; \
	type r; \
	g_pio->dispatch(std::bind(&ret_sync_queue<type>, &r, &done, &cond, &io_mutex, std::function<type(void)>(std::bind(& x)))); \
	VO_SYNC_WAIT

#define VO_SYNC_CALL_RET1(type, x, p1) \
	bool done = false; \
	type r; \
	g_pio->dispatch(std::bind(&ret_sync_queue<type>, &r, &done, &cond, &io_mutex, std::function<type(void)>(std::bind(& x, p1)))); \
	VO_SYNC_WAIT

#define VO_SYNC_CALL_RET2(type, x, p1, p2) \
	bool done = false; \
	type r; \
	g_pio->dispatch(std::bind(&ret_sync_queue<type>, &r, &done, &cond, &io_mutex, std::function<type(void)>(std::bind(& x, p1, p2)))); \
	VO_SYNC_WAIT

#define VO_SYNC_CALL_RET3(type, x, p1, p2, p3) \
	bool done = false; \
	type r; \
	g_pio->dispatch(std::bind(&ret_sync_queue<type>, &r, &done, &cond, &io_mutex, std::function<type(void)>(std::bind(& x, p1, p2, p3)))); \
	VO_SYNC_WAIT

#endif
}

///////////////////////////////// Windows Audio Callbacks //////////////////////////////////////


/*
	Callback class for current session events, -Audio Events Thread

	We use async call here, i decided to use asio because.. well i've been using it for longs years
		we could implement a thread safe queue for async calls but i already had this tested and done

	MSDN:
	1 The methods in the interface must be nonblocking. The client should never wait on a synchronization object during an event callback.
	2 The client should never call the IAudioSessionControl::UnregisterAudioSessionNotification method during an event callback.
	3 The client should never release the final reference on a WASAPI object during an event callback.

	NOTES:
	*1 To be non blocking and thread safe using our AudioMonitor class the only safe way is to use async calls.
	*2 Easy enough.
	*3 See AudioSession class destructor for more info about that.

*/

class CAudioSessionEvents : public IAudioSessionEvents
{
	LONG _cRef;
	std::weak_ptr<AudioSession> m_pAudioSession;
	std::weak_ptr<AudioMonitor> m_pAudioMonitor;

protected:

	~CAudioSessionEvents()
	{}

	CAudioSessionEvents()
		: _cRef(1)
	{}

public:

	CAudioSessionEvents(std::weak_ptr<AudioSession> pAudioSession,
		std::weak_ptr<AudioMonitor> pAudioMonitor)
		: _cRef(1)
		, m_pAudioSession(pAudioSession)
		, m_pAudioMonitor(pAudioMonitor)
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
#ifdef _DEBUG
		printf("OnDisplayNameChanged\n");
#endif
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnIconPathChanged(
		LPCWSTR NewIconPath,
		LPCGUID EventContext)
	{
#ifdef _DEBUG
		printf("OnIconPathChanged\n");
#endif
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(
		float NewVolume,
		BOOL NewMute,
		LPCGUID EventContext)
	{
		std::shared_ptr<AudioSession> spAudioSession;
		// If we didnt generate this event
		if (!IsEqualGUID(*EventContext, GUID_VO_CONTEXT_EVENT))
		{
			spAudioSession = m_pAudioSession.lock();
			if (!spAudioSession)
				return S_OK;

			ASYNC_CALL(&AudioCallbackProxy::UpdateDefaultVolume, spAudioSession, NewVolume);
		}

#ifdef _DEBUG
		if (!spAudioSession)
			spAudioSession = m_pAudioSession.lock();
		DWORD pid = 0;
		if (spAudioSession)
			pid = spAudioSession->getPID();
		if (NewMute)
		{
			printf("MUTE [%d]\n", pid);
		}
		else
		{
			printf("Callback: Volume = %d%% [%d]\n",
				(UINT32)(100 * NewVolume + 0.5), pid);
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
#ifdef _DEBUG
		printf("OnChannelVolumeChanged\n");
#endif
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(
		LPCGUID NewGroupingParam,
		LPCGUID EventContext)
	{
#ifdef _DEBUG
		printf("OnGroupingParamChanged\n");
#endif
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE OnStateChanged(
		AudioSessionState NewState)
	{
#ifdef _DEBUG
		printf("OnStateChanged CALLBACK: ");
#endif

		//std::shared_ptr<AudioMonitor> spAudioMonitor(m_pAudioMonitor.lock());
		std::shared_ptr<AudioSession> spAudioSession(m_pAudioSession.lock());
		if (!spAudioSession)
		{
#ifdef _DEBUG
			printf("spAudioSession == NULL!! on OnStateChanged()\n");
#endif
			return S_OK;
		}

		char *pszState = "?????";
		switch (NewState)
		{
		case AudioSessionStateActive:
			pszState = "active";
			ASYNC_CALL(&AudioCallbackProxy::ApplyVolumeSettings, spAudioSession);
			ASYNC_CALL(&AudioCallbackProxy::set_time_active_since, spAudioSession);
			break;
		case AudioSessionStateInactive:
			pszState = "inactive";
			ASYNC_CALL(&AudioCallbackProxy::RestoreVolume, spAudioSession);
			ASYNC_CALL(&AudioCallbackProxy::set_time_inactive_since, spAudioSession);
			break;
		case AudioSessionStateExpired: // only pops if we dont retaing a reference to the session
			pszState = "expired";
			//ASYNC_CALL(&AudioCallbackProxy::DeleteSession, spAudioMonitor, m_pAudioSession->getSIID());
			break;
		}
#ifdef _DEBUG
		printf("New session state = %s  PID[%d]\n", pszState, spAudioSession->getPID());
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

// we dont need instances of real base, this derived class its just a fix for event.
class CAudioSessionEvents_fix : public CAudioSessionEvents
{
	~CAudioSessionEvents_fix()
	{}

	CAudioSessionEvents_fix(std::weak_ptr<AudioSession> pAudioSession,
		std::weak_ptr<AudioMonitor> pAudioMonitor) :
		CAudioSessionEvents(pAudioSession, pAudioMonitor)
	{}

public:

	CAudioSessionEvents_fix() :
		CAudioSessionEvents()
	{}

};

/*
	Class for New Session Events Callbacks -Manager Events Thread

	MSDN:
	The application must not register or unregister notification callbacks during an event callback. 
*/
class CSessionNotifications : public IAudioSessionNotification
{
	LONG m_cRefAll;
	std::weak_ptr<AudioMonitor> m_pAudioMonitor;

	~CSessionNotifications() {};

public:

	CSessionNotifications(std::weak_ptr<AudioMonitor> pAudioMonitor)
		: m_cRefAll(1)
		, m_pAudioMonitor(pAudioMonitor)
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

	HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl *pNewSessionControl)
	{
		// Important, get a ref before an async call, windows deletes the object after return
		pNewSessionControl->AddRef();

		{
			// Fix, dont know why yet, but the first callback never arrives if we dont do this now.
			CAudioSessionEvents_fix* pAudioEvents = new CAudioSessionEvents_fix();
			HRESULT hr = S_OK;
			CHECK_HR(hr = pNewSessionControl->RegisterAudioSessionNotification(pAudioEvents));
			CHECK_HR(hr = pNewSessionControl->UnregisterAudioSessionNotification(pAudioEvents));
			SAFE_RELEASE(pAudioEvents);
		}

		if (pNewSessionControl)
		{
#ifdef _DEBUG
			printf("\t\tCALLBACK: New Session Incoming:\n");
#endif
			std::shared_ptr<AudioMonitor> spAudioMonitor(m_pAudioMonitor.lock());
			if (spAudioMonitor)
			{
				ASYNC_CALL(&AudioCallbackProxy::SaveSession, spAudioMonitor, pNewSessionControl, true);
			}
		}
		return S_OK;
	}
};


//////////////////////////////////////   Audio Session //////////////////////////////////////


AudioSession::AudioSession(IAudioSessionControl *pSessionControl, 
	AudioMonitor& a_AudioMonitor, float default_volume) //TODO: ver de hacer shared en AudioMonitor
	: m_default_volume(default_volume)
	, m_AudioMonitor(a_AudioMonitor)
	, m_hrStatus(S_OK)
	, m_pSessionControl(pSessionControl)
	, m_pAudioEvents(NULL)
	, m_pSimpleAudioVolume(NULL)
	, m_pSessionControl2(NULL)
	, is_volume_at_default(true)
{
	if (pSessionControl == NULL)
	{
		m_hrStatus = E_POINTER;
#ifdef _DEBUG
		printf("ERROR: AudioSession::AudioSession pSessionControl == NULL\n");
#endif
		return;
	}

	if (m_default_volume > 1.0f)
		m_default_volume = 1.0f;

	assert(m_pSessionControl);
	// NOTE: retaining a copy to a WASAPI session interface IAudioSessionControl  causes the session to never expire.
	m_pSessionControl->AddRef();
#if 0
	CHECK_HR(m_hrStatus = m_pSessionControl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&m_pSimpleAudioVolume));
	assert(m_pSimpleAudioVolume);

	CHECK_HR(m_hrStatus = m_pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&m_pSessionControl2));
	assert(m_pSimpleAudioVolume);
#endif
	
	// if default not set (negative) set it.
	float cur_v = GetCurrentVolume();
	if (m_default_volume < 0.0f)
		UpdateDefaultVolume(cur_v);

	/* Fix for windows, if a new session is detected when the process was just
		opened, volume controls won work. wait for a bit */
	Sleep(20);

	// Apply current monitor volume settings con creation.
#ifdef VO_ENABLE_EVENTS
	AudioSessionState State;
	CHECK_HR(m_hrStatus = pSessionControl->GetState(&State));
	if (State == AudioSessionStateActive)
	{
		set_time_active_since();
		ApplyVolumeSettings();
	}
	else
		set_time_inactive_since();
#else
	ApplyVolumeSettings();
	set_time_active_since();
#endif

	// Windows SndVol fix.
	/* if param default_volume >= 0 it means that is the real SID default, as explained in
	AudioSession::ChangeAudio, Sessions with the same SID take their default volume
	from the last SID changed 5sec ago from the registry, see ChangeAudio for more doc. */
	if (default_volume >= 0.0f)
	{
		// TODO: chekear que las sesiones nuevas corrigan bien el volumen
		if ((m_default_volume != cur_v) && is_volume_at_default)
		{
			ChangeVolume(m_default_volume);	// fix correct windows volume before aplying settings
		}
	}

#ifdef _DEBUG
#ifndef VO_ENABLE_EVENTS
	AudioSessionState State;
	m_hrStatus = pSessionControl->GetState(&State);
#endif
	wchar_t *pszState = L"?????";
	switch (State)
	{
	case AudioSessionStateActive:
		pszState = L"active";
		break;
	case AudioSessionStateInactive:
		pszState = L"inactive";
		break;
	case AudioSessionStateExpired: // only shows if we dont retaing a pointer to the session
		pszState = L"expired";
		break;
	}
	wprintf_s(L"\nAudioMonitor::SaveSession\t Saved SIID [PID]: %s [%d] State: %s\n\n", getSIID().c_str(), getPID(), pszState);
#endif
}

AudioSession::~AudioSession()
{
#ifdef _DEBUG
	wprintf_s(L"\n~AudioSession:: Deleting Session %s\n", getSID().c_str());
#endif

	StopEvents();

	RestoreVolume();

	assert(CHECK_REFS(m_pSessionControl) == 1);
	//assert(CHECK_REFS(m_pSimpleAudioVolume) == 1);
	//assert(CHECK_REFS(m_pSessionControl2) == 1);

	//SAFE_RELEASE(m_pSessionControl2);
	//SAFE_RELEASE(m_pSimpleAudioVolume);
	SAFE_RELEASE(m_pSessionControl);
}

/*
	Enables Session notification callbacks

	Registers for new notifications on this session.
	Refer to Stop/Unregister to more comments on this.
*/
void AudioSession::InitEvents()
{
	if (m_pAudioEvents == NULL)
	{
		// CAudioSessionEvents constructor sets Refs on 1 so remember to release
		m_pAudioEvents = new CAudioSessionEvents(this->shared_from_this(), m_AudioMonitor.shared_from_this());

		// RegisterAudioSessionNotification calls another AddRef on m_pAudioEvents so Refs = 2 by now
		CHECK_HR(m_hrStatus = m_pSessionControl->RegisterAudioSessionNotification(m_pAudioEvents));
#ifdef _DEBUG
		printf("AudioSession::InitEvents() Init Session Events on PID[%d]\n", getPID());
#endif
	}

	assert(m_pAudioEvents);
}

/*
	Stop Session notification callbacks

	Its important to follow what MSDN says about this or we could leak memory.
	http://msdn.microsoft.com/en-us/library/dd368289%28v=vs.85%29.aspx
	AudioSession, callbacks and general flow is carefuly written so this never occurs.
*/
void AudioSession::StopEvents()
{
	if ((m_pSessionControl != NULL) && (m_pAudioEvents != NULL))
	{
		// MSDN: "The client should never release the final reference on a WASAPI object during an event callback."
		//	So we need to unregister and wait for current events to finish before releasing the our last ISessionControl
		// NOTE: using async calls we garantize we can comply to this.
		CHECK_HR(m_hrStatus = m_pSessionControl->UnregisterAudioSessionNotification(m_pAudioEvents));
#ifdef _DEBUG
		printf("AudioSession::StopEvents() Stopped Session Events on PID[%d]\n", getPID());
#endif
	}

	// if called too fast it can be > 1, maybe a callback was in place.
	if (m_pAudioEvents != NULL)
		assert(CHECK_REFS(m_pAudioEvents) == 1);

	SAFE_RELEASE(m_pAudioEvents);
}

std::wstring AudioSession::getSID() const
{
	LPWSTR sid;

	IAudioSessionControl2* pSessionControl2 = NULL;
	CHECK_HR(m_hrStatus = m_pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pSessionControl2));
	assert(pSessionControl2);

	CHECK_HR(m_hrStatus = pSessionControl2->GetSessionIdentifier(&sid)); // This one is NOT unique
	std::wstring ws_sid(sid);
	CoTaskMemFree(sid);

	SAFE_RELEASE(pSessionControl2);

	return ws_sid;
}

std::wstring AudioSession::getSIID() const
{
	LPWSTR siid;

	IAudioSessionControl2* pSessionControl2 = NULL;
	CHECK_HR(m_hrStatus = m_pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pSessionControl2));
	assert(pSessionControl2);

	CHECK_HR(m_hrStatus = pSessionControl2->GetSessionInstanceIdentifier(&siid)); // This one is unique
	std::wstring ws_siid(siid);
	CoTaskMemFree(siid);

	SAFE_RELEASE(pSessionControl2);

	return ws_siid;
}

DWORD AudioSession::getPID() const
{
	DWORD pid;

	IAudioSessionControl2* pSessionControl2 = NULL;
	CHECK_HR(m_hrStatus = m_pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pSessionControl2));
	assert(pSessionControl2);

	CHECK_HR(m_hrStatus = pSessionControl2->GetProcessId(&pid));

	SAFE_RELEASE(pSessionControl2);

	return pid;
}

float AudioSession::GetCurrentVolume()
{
	float current_volume;

	ISimpleAudioVolume* pSimpleAudioVolume = NULL;
	CHECK_HR(m_hrStatus = m_pSessionControl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&pSimpleAudioVolume));
	assert(pSimpleAudioVolume);

	CHECK_HR(m_hrStatus = pSimpleAudioVolume->GetMasterVolume(&current_volume));
#ifdef _DEBUG
	printf("AudioSession::GetCurrentVolume() = %.2f\n", current_volume);
#endif

	SAFE_RELEASE(pSimpleAudioVolume);

	return current_volume;
}

/*
	Changes Session Volume based on current config

	It uses current global config to change the session volume.

	Note: SndVol uses the last changed session's volume on new instances of the same process as default vol.
	It takes ~5 sec to register a new volume level back at the registry, key:
	"HKEY_CURRENT_USER\Software\Microsoft\Internet Explorer\LowRegistry\Audio\PolicyConfig\PropertyStore"
	it overwrites sessions with the same SID.
*/
HRESULT AudioSession::ApplyVolumeSettings()
{
	HRESULT hr = S_OK;

	float current_vol_reduction = m_AudioMonitor.m_settings.vol_reduction;

	// if AudioSession::is_volume_at_default is true, the session is intact, not changed, else not
	// if AudioMonitor::auto_change_volume_flag is true, volume reduction is in place, else disabled.
	if ((is_volume_at_default) && (m_AudioMonitor.auto_change_volume_flag))
	{
		float set_vol;
		if (m_AudioMonitor.m_settings.treat_vol_as_percentage)
			set_vol = m_default_volume * (1.0f - current_vol_reduction);
		else
			set_vol = current_vol_reduction;

		ChangeVolume(set_vol);
		is_volume_at_default = false; // to signal the session that is NOT at default state.

#ifdef _DEBUG
		printf("AudioSession::ApplyVolumeSettings() Changed Volume of PID[%d] to %.2f\n",
			getPID(), set_vol);
#endif
	}
	else
	{
#ifdef _DEBUG
		printf("AudioSession::ApplyVolumeSettings() skiped, flag=%d global_vol_reduction = %.2f, "
			"is_volume_at_default = %d \n", m_AudioMonitor.auto_change_volume_flag, current_vol_reduction,
			is_volume_at_default);
#endif
	}

	return hr;
}

/*
	Sets new volume level as default for restore.

	Used when user changed volume manually while monitoring, 
		updates his preference as new default, this should be
		called from callbacks when contextGUID is different of ours.
		that means we didnt change the volume.
		if not using events, defaults will be updated only when 
		AudioMonitor is refreshing.
*/
void AudioSession::UpdateDefaultVolume(float new_def)
{
	m_default_volume = new_def;
	touch();

#ifdef _DEBUG
	printf("AudioSession::UpdateDefaultVolume(%.2f)\n", new_def);
#endif
}

/*
	Restores Default session volume

	We saved the default volume on the session so we can restore it here
	m_default_volume always have the user default volume.
	If is_volume_at_default flag is true it means the session is already at default level.
	If is false or positive it means we have to restore it.
*/
HRESULT AudioSession::RestoreVolume()
{
	HRESULT hr = S_OK;
	assert(m_pSessionControl);

	if (!is_volume_at_default)
	{
		ChangeVolume(m_default_volume);
#ifdef _DEBUG
		printf("AudioSession:: Restoring Volume of Session PID[%d] to %.2f\n", getPID(), m_default_volume);
#endif
		is_volume_at_default = true; // to signal the session is at default state.
	}
	else
	{
#ifdef _DEBUG
		printf("AudioSession:: Restoring Volume already at default state = %.2f\n", m_default_volume);
#endif
	}

	return hr;
}

/*
	Directly Aplies volume on session
*/
void AudioSession::ChangeVolume(const float v)
{
	ISimpleAudioVolume* pSimpleAudioVolume = NULL;
	CHECK_HR(m_hrStatus = m_pSessionControl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&pSimpleAudioVolume));
	assert(pSimpleAudioVolume);

	CHECK_HR(m_hrStatus = pSimpleAudioVolume->SetMasterVolume(v, &GUID_VO_CONTEXT_EVENT));
	touch();

#ifdef _DEBUG
	printf("AudioSession::ChangeVolume Forcing volume level = %.2f\n", v);
#endif

	SAFE_RELEASE(pSimpleAudioVolume);
}


void AudioSession::touch()
{
	m_last_modified_on = std::chrono::steady_clock::now();
}

void AudioSession::set_time_inactive_since()
{
	m_last_active_state = std::chrono::steady_clock::now();
}
void AudioSession::set_time_active_since()
{
	m_last_active_state = std::chrono::steady_clock::time_point::max();
}

//////////////////////////////////////   Audio Monitor //////////////////////////////////////


AudioMonitor::AudioMonitor(vo::monitor_settings& settings)
		: auto_change_volume_flag(false)
		, m_settings(settings)
		, m_pSessionEvents(NULL)
		, m_pSessionManager2(NULL)
		, m_abort(false)
#ifdef VO_ENABLE_EVENTS
		, m_delete_expired_interval(30) // when to call delete_expired_sessions
		, m_inactive_timeout(120) // sessions older than this are deleted
#endif
{
	HRESULT hr = S_OK;

	m_processid = GetCurrentProcessId();

	SetSettings(m_settings);

	// Initialize unique GUID for own events if not already created.
	//if (IsEqualGUID(GUID_VO_CONTEXT_EVENT_ZERO, GUID_VO_CONTEXT_EVENT))
		//CHECK_HR(hr = CoCreateGuid(&GUID_VO_CONTEXT_EVENT));
	m_io.reset(new boost::asio::io_service);
	g_pio = m_io;

	hr = CreateSessionManager();

	m_thread_monitor = std::thread(&AudioMonitor::poll, this);

	m_current_status = AudioMonitor::monitor_status_t::STOPPED;
}

AudioMonitor::~AudioMonitor()
{
	// Exit MonitorThread
	m_abort = true;
	m_io->stop();
	m_thread_monitor.join(); // wait to finish
	g_pio.reset();

#ifdef VO_ENABLE_EVENTS
	// Unregister Event callbacks before destruction
	//	if we dont want memory leaks.
	StopEvents();
#endif

	// Delete and Restore saved Sessions
	Stop();

	SAFE_RELEASE(m_pSessionEvents);
	SAFE_RELEASE(m_pSessionManager2);

#ifdef _DEBUG
	printf("\n\t...AudioMonitor Destroyed.\n");
#endif
}

/*
	Simple get for asio io_service of AudioMonitor

	In case we need to queue calls from other places.
*/
std::weak_ptr<boost::asio::io_service> AudioMonitor::get_io()
{
	return m_io;
}

/*
	Poll for new calls on current thread

	When events are enabled this is the only way to garantize what msdn says
	We do this using async calls from callbacks threads to this one.
	It locks the mutex so no other thread can access the class while polling.
*/
void AudioMonitor::poll()
{
	// No other thread can use this class while pooling
	std::lock_guard<std::recursive_mutex> guard(m_mutex);

#ifdef _DEBUG
	printf("\n\t...AudioMonitor Thread Init\n");
#endif

	boost::asio::io_service::work work(*m_io);

#ifdef VO_ENABLE_EVENTS
	std::shared_ptr<boost::asio::steady_timer> expired_session_removal_timer =
		std::make_shared<boost::asio::steady_timer>(*m_io);
	expired_session_removal_timer->expires_from_now(m_delete_expired_interval);
	expired_session_removal_timer->async_wait(std::bind(&AudioMonitor::DeleteExpiredSessions,
		this, std::placeholders::_1, expired_session_removal_timer));
#endif

	// Call Dispatcher
	bool stop_loop = false;
	while (!stop_loop)
	{
		boost::system::error_code ec;
		m_io->run(ec);
		if (ec)
		{
			printf("\t\t\tAsio Error: %s\n", ec.message().c_str());
		}
		m_io->reset();

		stop_loop = m_abort;
	}

}

/*
	Creates the IAudioSessionManager instance on default output device
*/
HRESULT AudioMonitor::CreateSessionManager()
{
	HRESULT hr = S_OK;

	IMMDevice* pDevice = NULL;
	IMMDeviceEnumerator* pEnumerator = NULL;

	CHECK_HR(hr = CoInitializeEx(NULL, COINIT_MULTITHREADED));

	// Create the device enumerator.
	CHECK_HR(hr = CoCreateInstance(
		__uuidof(MMDeviceEnumerator),
		NULL, CLSCTX_ALL,
		__uuidof(IMMDeviceEnumerator),
		(void**)&pEnumerator));

	if (pEnumerator == NULL)
		return S_FALSE; // error creating instance

	// Get the default audio device.
	CHECK_HR(hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice));

	// Get the session manager.
	CHECK_HR(hr = pDevice->Activate(
		__uuidof(IAudioSessionManager2), CLSCTX_ALL,
		NULL, (void**)&m_pSessionManager2));

	SAFE_RELEASE(pEnumerator);
	SAFE_RELEASE(pDevice);

	return hr;
}

/*
	Deletes all sessions and adds current ones

	Uses windows7+ enumerator to list all SndVol sessions
		then saves each session found.

	TODO: move filter settings to class, and create a set method
*/
HRESULT AudioMonitor::RefreshSessions()
{
	if (!m_pSessionManager2)
		return E_INVALIDARG;

	HRESULT hr = S_OK;

	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	int cbSessionCount = 0;

	IAudioSessionEnumerator* pSessionList = NULL;

	// Delete saved sessions before overwriting
	DeleteSessions();

	// Get the current list of sessions.
	// IMPORTANT NOTE: DONT retain references to IAudioSessionControl before calling this function, it causes memory leaks
	CHECK_HR(hr = m_pSessionManager2->GetSessionEnumerator(&pSessionList));

	// Get the session count.
	CHECK_HR(hr = pSessionList->GetCount(&cbSessionCount));

#ifdef _DEBUG
	printf("\n\n------ Refresing Sessions...\n\n");
#endif

	static const bool unref_there = false;
	for (int index = 0; index < cbSessionCount; index++)
	{
		IAudioSessionControl* pSessionControl = NULL;

		// Get the <n>th session.
		CHECK_HR(hr = pSessionList->GetSession(index, &pSessionControl));

		CHECK_HR(hr = SaveSession(pSessionControl, unref_there));

		SAFE_RELEASE(pSessionControl);
	}

	SAFE_RELEASE(pSessionList);

	return hr;
}

/*
	Deletes all sessions from multimap
*/
void AudioMonitor::DeleteSessions()
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	// Deleting map to call AudioSession destructors
	m_saved_sessions.clear();

#ifdef _DEBUG
	wprintf_s(L"\n\tAudioMonitor::DeleteSessions() Saved Sessions cleared\n");
#endif
}

/*
	Auto called in fixes time intervals or manually to delete
		expired sessions.

	We have to implement this because when wont get expired status
		callbacks when we retain WASAPI references.
*/
void AudioMonitor::DeleteExpiredSessions(boost::system::error_code const& e, 
	std::shared_ptr<boost::asio::steady_timer> timer)
{
	if (e == boost::asio::error::operation_aborted) return;

	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	if (e)
	{
#ifdef _DEBUG
		wprintf_s(L"ASIO Timer cancelled: %s\n", e.message().c_str());
#endif
	}

	timer->expires_from_now(m_delete_expired_interval);
	timer->async_wait(std::bind(&AudioMonitor::DeleteExpiredSessions,
		this, std::placeholders::_1, timer));

	HRESULT hr = S_OK;

	std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	for (auto it = m_saved_sessions.begin(); it != m_saved_sessions.end();)
	{
		// if inactive for more than (inactive_timeout), remove it
		std::chrono::steady_clock::duration oldness(now - it->second->m_last_active_state);
		if (oldness > m_inactive_timeout)
		{
#ifdef _DEBUG
			wprintf_s(L"Session PID[%d] Too old, removing...\n", it->second->getPID());
#endif
			it = m_saved_sessions.erase(it);
		}
		else
			it++;
	}

#ifdef _DEBUG // TODO test
	wprintf_s(L". DeleteExpired tick\n");
#endif
}

void AudioMonitor::ApplyCurrentSettings()
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	if (auto_change_volume_flag)
	{
		// Only change volume of active saved sessions
		AudioSessionState State;
		for (auto it = m_saved_sessions.begin(); it != m_saved_sessions.end(); ++it)
		{
			assert(it->second->m_pSessionControl);
#ifdef VO_ENABLE_EVENTS
			it->second->m_pSessionControl->GetState(&State);
			if (State == AudioSessionStateActive)
				it->second->ApplyVolumeSettings();
#else
			it->second->ApplyVolumeSettings();
#endif
		}
	}
}

bool AudioMonitor::isSessionExcluded(DWORD pid, std::wstring sid)
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	if (m_settings.exclude_own_process)
	{
		if (m_processid == pid)
			return true;
	}

	std::transform(sid.begin(), sid.end(), sid.begin(), ::tolower);

	if (!m_settings.use_included_filter)
	{
		for (auto n : m_settings.excluded_pids)
		{
			if (n == pid)
				return true;
		}
		// search for name inside sid
		for (auto n : m_settings.excluded_process)
		{
			std::size_t found = sid.find(n);
			if (found != std::string::npos)
				return true;
		}
	}
	else
	{
		for (auto n : m_settings.included_pids)
		{
			if (n == pid)
				return false;
		}
		// search for name inside sid
		for (auto n : m_settings.included_process)
		{
			std::size_t found = sid.find(n);
			if (found != std::string::npos)
				return false;
		}

		return true;
	}

	return false;
}

/*
	Saves a New session in multimap

	Group of session with different SIID (SessionInstanceIdentifier) but with the same 
		SID (SessionIdentifier) are grouped togheter on the same key (SID).
	See more notes inside.
	Before adding the session it detects whanever this session is another instance 
		of the same process (same SID) and copies the default volume of the last changed
		session.

	IAudioSessionControl* pSessionControl  -> pointer to the new session
	bool unref  -> do we Release() inside this function or not? useful for async calls
*/
HRESULT AudioMonitor::SaveSession(IAudioSessionControl* pSessionControl, bool unref)
{
	HRESULT hr = S_OK;

	if (!pSessionControl)
		return E_INVALIDARG;

	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	assert(pSessionControl);
	IAudioSessionControl2* pSessionControl2 = NULL;
	CHECK_HR(hr = pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pSessionControl2));
	assert(pSessionControl2);

	//TODO: We could detect crashes from this. is generaly full volume.
	// http://msdn.microsoft.com/en-us/library/windows/desktop/dd368257%28v=vs.85%29.aspx
	//hr = pSessionControl2->IsSystemSoundsSession();

	DWORD pid;
	CHECK_HR(hr = pSessionControl2->GetProcessId(&pid));

	LPWSTR _sid;
	CHECK_HR(hr = pSessionControl2->GetSessionIdentifier(&_sid)); // This one is NOT unique
	std::wstring ws_sid(_sid);
	CoTaskMemFree(_sid);

	bool excluded = isSessionExcluded(pid, ws_sid);

	if ((pSessionControl2->IsSystemSoundsSession() == S_FALSE) && !excluded)
	{
#ifdef _DEBUG
		wprintf_s(L"\n---Saving New Session: PID [%d]\n", pid);
#endif

		LPWSTR _siid;
		CHECK_HR(hr = pSessionControl2->GetSessionInstanceIdentifier(&_siid)); // This one is unique
		std::wstring ws_siid(_siid);
		CoTaskMemFree(_siid);

		bool duplicate = false;

		//	If a SID already exists, copy the default volume of the last changed session .
		//	As SndVol does, always copies the sound volume of the last changed session, try opening the same music player 3 times
		//		before the 3rd change volume to the second instance and then to the first, wait >5sec and open the 3rd,
		//		SndVol will take the 1rst one volume. NOTE: Takes 5sec for SndVol to register last volume of SID on the registry.
		//		registry saves only SIDs so they overwrite each other, the last one takes precedence.
		float last_sid_volume_fix = -1.0;
		if (m_saved_sessions.count(ws_sid))
		{
			auto range = m_saved_sessions.equal_range(ws_sid);
#ifdef _DEBUG
			wprintf_s(L"AudioMonitor::SaveSession - Equal SID detected bucket_size=%d\n", m_saved_sessions.count(ws_sid));
#endif
			// Search for duplicates or last changed session to fix windows default volume
			std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
			std::chrono::steady_clock::duration minduration = std::chrono::steady_clock::duration::max();
			t_saved_sessions::iterator last_changed = range.first;
			for (auto it = range.first; it != range.second; ++it)
			{
				// Take an iterator to the last session modified
				std::chrono::steady_clock::duration duration(now - it->second->m_last_modified_on);
				if (duration < minduration)
				{
					minduration = duration;
					last_changed = it;
				}
				if (it->second->getSIID() == ws_siid)
				{
#ifdef _DEBUG
					wprintf_s(L"AudioMonitor::SaveSession - Equal SIID detected, DUPLICATE discarting...\n");
#endif
					duplicate = true;
					break;
				}
			}

			last_sid_volume_fix = last_changed->second->m_default_volume;
#ifdef _DEBUG
			wprintf_s(L"AudioMonitor::SaveSession - Copying default volume of last session %.2f\n", last_sid_volume_fix);
#endif
		}

		if (!duplicate)
		{
			// Initialize the new AudioSession and store it.
			std::shared_ptr<AudioSession> pAudioSession = std::make_shared<AudioSession>(pSessionControl,
				*this, last_sid_volume_fix); // TODO: doesnt work with private constructor
#ifdef VO_ENABLE_EVENTS
			pAudioSession->InitEvents();
#endif
			// Save session
			m_saved_sessions.insert(t_session_pair(ws_sid, pAudioSession));

			// TODO: testear esto q puse
			/*  Its possible that between apply current volume settings and enable Events
					the session became active, so force a refresh again. */
			// Force volume refresh again
#ifdef VO_ENABLE_EVENTS
			/*
			AudioSessionState State;
			CHECK_HR(hr = pSessionControl->GetState(&State));
			if (State == AudioSessionStateActive)
				pAudioSession->ApplyVolumeSettings();
				*/
#else
			pAudioSession->ApplyVolumeSettings();
#endif
		}
	}
	else
	{
#ifdef _DEBUG
		wprintf_s(L"--AudioMonitor::SaveSession PID [%d] skipped...\n", pid);
#endif
	}

	SAFE_RELEASE(pSessionControl2);
	if (unref)
		SAFE_RELEASE(pSessionControl);

	return hr;
}

/*
	Not used yet TODO: research  why sessions never expire.
*/
void AudioMonitor::DeleteSession(std::wstring siid)
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);
	// NOT USED, sessions never expire when retaing pointer
}

/*
	Stops monitoring current sessions

	First we stop events so new sessions cant come in
	Then we set volume change flag to false
	Finaly we delete all sessions restoring default volume.
	Now the class is at clean state ready for shutdown or new start.
	
	TODO: error codes
*/

long AudioMonitor::Stop()
{
	HRESULT ret = S_OK;

	//if (std::this_thread::get_id() != m_thread_monitor.get_id())
	std::unique_lock<std::recursive_mutex> l(m_mutex, std::try_to_lock);

	if (!l.owns_lock())
	{
		ret = SYNC_CALL_RET<long>(&AudioMonitor::Stop, this);
	}
	else
	{
		if (m_current_status == AudioMonitor::monitor_status_t::STOPPED)
			return 0;

		// Global class flag , volume reduction inactive
		auto_change_volume_flag = false;

#ifdef VO_ENABLE_EVENTS
		// First we stop new sessions from coming
		ret = StopEvents();
#endif

		// Deletes all sessions restoring volume
		DeleteSessions();

#ifdef _DEBUG
		wprintf_s(L"\n\t ---- AudioMonitor::Stop() STOPPED .... \n\n");
#endif
		m_current_status = AudioMonitor::monitor_status_t::STOPPED;
	}

	return static_cast<long>(ret);
}

#ifdef VO_ENABLE_EVENTS 
/*
	Enables ISessionManager2 Events for new sessions

	Registers the class for callbacks to receive notifications on new sessions.

	TODO: do error codes.
*/
long AudioMonitor::InitEvents()
{
	HRESULT ret = S_OK;

	//if (std::this_thread::get_id() != m_thread_monitor.get_id())
	std::unique_lock<std::recursive_mutex> l(m_mutex, std::try_to_lock);

	if (!l.owns_lock())
	{
		ret = SYNC_CALL_RET<long>(&AudioMonitor::InitEvents, this);
	}
	else
	{
		// Events for adding new sessions
		if ((m_pSessionManager2 != NULL) && (m_pSessionEvents == NULL))
		{
			m_pSessionEvents = new CSessionNotifications(this->shared_from_this()); // AddRef() on constructor
			CHECK_HR(ret = m_pSessionManager2->RegisterSessionNotification(m_pSessionEvents));
			assert(m_pSessionEvents);
		}
	}

	return static_cast<long>(ret);
}

/*
	Disables ISessionManager2 Events for new sessions

	Unregisters the class for callbacks to stop new session notifications.

	TODO: do error codes.
*/
long AudioMonitor::StopEvents()
{
	HRESULT ret = S_OK;

	//if (std::this_thread::get_id() != m_thread_monitor.get_id())
	std::unique_lock<std::recursive_mutex> l(m_mutex, std::try_to_lock);

	if (!l.owns_lock())
	{
		ret = SYNC_CALL_RET<long>(&AudioMonitor::StopEvents, this);
	}
	else
	{
		// Stop new session notifications
		if ((m_pSessionManager2 != NULL) && (m_pSessionEvents != NULL))
			CHECK_HR(ret = m_pSessionManager2->UnregisterSessionNotification(m_pSessionEvents));

		if (m_pSessionEvents != NULL)
			assert(CHECK_REFS(m_pSessionEvents) == 1);

		SAFE_RELEASE(m_pSessionEvents);
	}

	return static_cast<long>(ret);
}
#endif

long AudioMonitor::Pause()
{
	HRESULT ret = S_OK;

#ifndef VO_ENABLE_EVENTS
	ret = Stop();
#else
	//if (std::this_thread::get_id() != m_thread_monitor.get_id())
	std::unique_lock<std::recursive_mutex> l(m_mutex, std::try_to_lock);

	if (!l.owns_lock())
	{
		ret = SYNC_CALL_RET<long>(&AudioMonitor::Pause, this);
	}
	else
	{
		if (m_current_status == AudioMonitor::monitor_status_t::STOPPED)
			return 0;
		if (m_current_status == AudioMonitor::monitor_status_t::PAUSED)
			return 0;

		// Global class flag , volume reduction inactive
		auto_change_volume_flag = false;

		// Restore Volume of all sessions currently monitored
		for (auto it = m_saved_sessions.begin(); it != m_saved_sessions.end(); ++it)
		{
			assert(it->second->m_pSessionControl);
			it->second->RestoreVolume();
		}

#ifdef _DEBUG
		wprintf_s(L"\n\t ---- AudioMonitor::Pause  PAUSED .... \n\n");
#endif
		m_current_status = AudioMonitor::monitor_status_t::PAUSED;
	}
#endif

	return static_cast<long>(ret);
}

/*
	Starts monitoring sessions to change volume on

	Each time we start the monitor we refresh all current sessions
	(useful when not using events too)
	auto_change_volume_flag indicates on class level if we are currently
	changing volume or not, if its false it means AudioSession::ApplyVolumeSettings
	will have no effect and all sessions will be at default level.
	If not, it means we are changing volume based on session activity.
*/
long AudioMonitor::Resume()
{
	HRESULT ret = S_OK;

	//if (std::this_thread::get_id() != m_thread_monitor.get_id())
	std::unique_lock<std::recursive_mutex> l(m_mutex, std::try_to_lock);
	if (!l.owns_lock())
	{
		ret = SYNC_CALL_RET<long>(&AudioMonitor::Resume, this);
	}
	else
	{
		if (m_current_status == AudioMonitor::monitor_status_t::RUNNING)
			return 0;

		if (m_current_status == AudioMonitor::monitor_status_t::STOPPED)
		{
#ifdef _DEBUG
			wprintf_s(L"\n\t .... AudioMonitor::Start() STARTED ----\n\n");
#endif
			// Delete and re add new sessions
			ret = RefreshSessions();

#ifdef VO_ENABLE_EVENTS
			/* Next we enable new incoming sessions. */
			ret = InitEvents();
#endif
		}
		// Activate reduce volume flag and update vol reduction
		auto_change_volume_flag = true;

		// Forces config on all sessions.
		ApplyCurrentSettings();

		m_current_status = AudioMonitor::monitor_status_t::RUNNING;	
	}

	return static_cast<long>(ret); // TODO: error codes
}

long AudioMonitor::Refresh()
{
	HRESULT ret = S_OK;

	//if (std::this_thread::get_id() != m_thread_monitor.get_id())
	std::unique_lock<std::recursive_mutex> l(m_mutex, std::try_to_lock);

	if (!l.owns_lock())
	{
		ret = SYNC_CALL_RET<long>(&AudioMonitor::Refresh, this);
	}
	else
	{
		ret = RefreshSessions();
	}

	return static_cast<long>(ret); // TODO: error codes
}

void AudioMonitor::SetSettings(vo::monitor_settings& settings)
{
	std::unique_lock<std::recursive_mutex> l(m_mutex, std::try_to_lock);

	if (!l.owns_lock())
	{
		SYNC_CALL(&AudioMonitor::SetSettings, this, std::ref(settings));
	}
	else
	{
		m_settings = settings;

		if (!m_settings.use_included_filter)
		{
			// search for name inside sid
			for (auto n : m_settings.excluded_process)
			{
				std::transform(n.begin(), n.end(), n.begin(), ::tolower);
			}
		}
		else
		{
			// search for name inside sid
			for (auto n : m_settings.included_process)
			{
				std::transform(n.begin(), n.end(), n.begin(), ::tolower);
			}
		}

		if (m_settings.vol_reduction < 0)
			m_settings.vol_reduction = 0.0f;

		if (!m_settings.treat_vol_as_percentage)
		{
			if (m_settings.vol_reduction > 1.0f)
				m_settings.vol_reduction = 1.0f;
		}




		ApplyCurrentSettings();
	}

}

float AudioMonitor::GetVolumeReductionLevel()
{
	float ret;

	std::unique_lock<std::recursive_mutex> l(m_mutex, std::try_to_lock);

	if (!l.owns_lock())
	{
		ret = SYNC_CALL_RET<float>(&AudioMonitor::GetVolumeReductionLevel, this);
	}
	else
	{
		ret = m_settings.vol_reduction;
	}

	return ret;
}

AudioMonitor::monitor_status_t AudioMonitor::GetStatus()
{
	monitor_status_t ret;

	std::unique_lock<std::recursive_mutex> l(m_mutex, std::try_to_lock);

	if (!l.owns_lock())
	{
		ret = SYNC_CALL_RET<monitor_status_t>(&AudioMonitor::GetStatus, this);
	}
	else
	{
		ret = m_current_status;
	}

	return ret;
}

#endif
