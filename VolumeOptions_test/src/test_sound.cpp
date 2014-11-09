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

// Including SDKDDKVer.h defines the highest available Windows platform.

// If you wish to build your application for a previous Windows platform, include WinSDKVer.h and
// set the _WIN32_WINNT macro to the platform you wish to support before including SDKDDKVer.h.

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <boost/asio.hpp>

#if 0
#ifdef _DEBUG
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif
#endif

#include <SDKDDKVer.h>

#include <Audiopolicy.h>
#include <Mmdeviceapi.h>
#include <Endpointvolume.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <codecvt>
#include <fstream>

#include <iostream>
#include <unordered_map>
#include <stack>
#include <thread>
#include <random>

//#include "test_sound.h"
#include "../volumeoptions/sound_plugin.h"
#include "../volumeoptions/sounds_windows.h"
#include "../volumeoptions/async_queue.h" // testing, tiene memory leaks en algun lado

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "Advapi32.lib")

namespace {
	boost::asio::io_service io;
}

// used to sync calls from other threads usign io_service.
namespace {
	static std::recursive_mutex io_mutex;
	static std::condition_variable_any cond; // libtorrent cond is a semaphore.


	template <typename R>
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


template <typename ft, typename... pt>
inline
void ASYNC_CALL(ft&& f, pt&&... args)
{
	io.post(std::bind(std::forward<ft>(f), std::forward<pt>(args)...));
}

template <typename ft, typename... pt>
inline
void SYNC_CALL(ft&& f, pt&&... args)
{
	bool done = false;
	io.dispatch(std::bind(&sync_queue, &done, &cond, &io_mutex, std::function<void(void)>(std::bind(std::forward<ft>(f), std::forward<pt>(args)...))));
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
	io.dispatch(std::bind(&ret_sync_queue<rt>, &r, &done, &cond, &io_mutex, std::function<rt(void)>(std::bind(std::forward<ft>(f), std::forward<pt>(args)...))));
	std::unique_lock<std::recursive_mutex> l(io_mutex);
	while (!done) { cond.wait(l); };
	l.unlock();
	return r;
}



	// Note: When all are released only the ones with the local bool set to true return, the others wait again.
#define VO_SYNC_WAIT \
	std::unique_lock<std::recursive_mutex> l(io_mutex); \
	while (!done) { cond.wait(l); }; \
	l.unlock();

	// Simple Wrappers for async without return types
#define VO_ASYNC_CALL(x) \
	io.post(std::bind(& x))

#define VO_ASYNC_CALL1(x, p1) \
	io.post(std::bind(& x, p1))

#define VO_ASYNC_CALL2(x, p1, p2) \
	io.post(std::bind(& x, p1, p2))

#define VO_ASYNC_CALL3(x, p1, p2, p3) \
	io.post(std::bind(& x, p1, p2, p3))

	// Note: These use dispatch in case a sync call is requested from within the same io.run thread, the cond.wait will be ommited.
#define VO_SYNC_CALL(x) \
	bool done = false; \
	io.dispatch(std::bind(&sync_queue, &done, &cond, &io_mutex, std::function<void(void)>(std::bind(& x)))); \
	VO_SYNC_WAIT

#define VO_SYNC_CALL1(x, p1) \
	bool done = false; \
	io.dispatch(std::bind(&sync_queue, &done, &cond, &io_mutex, std::function<void(void)>(std::bind(& x, p1)))); \
	VO_SYNC_WAIT

#define VO_SYNC_CALL2(x, p1, p2) \
	bool done = false; \
	io.dispatch(std::bind(&sync_queue, &done, &cond, &io_mutex, std::function<void(void)>(std::bind(& x, p1, p2)))); \
	VO_SYNC_WAIT

#define VO_SYNC_CALL_RET(type, x) \
	bool done = false; \
	type r; \
	io.dispatch(std::bind(&ret_sync_queue<type>, &r, &done, &cond, &io_mutex, std::function<type(void)>(std::bind(& x)))); \
	VO_SYNC_WAIT

#define VO_SYNC_CALL_RET1(type, x, p1) \
	bool done = false; \
	type r; \
	io.dispatch(std::bind(&ret_sync_queue<type>, &r, &done, &cond, &io_mutex, std::function<type(void)>(std::bind(& x, p1)))); \
	VO_SYNC_WAIT

#define VO_SYNC_CALL_RET2(type, x, p1, p2) \
	bool done = false; \
	type r; \
	io.dispatch(std::bind(&ret_sync_queue<type>, &r, &done, &cond, &io_mutex, std::function<type(void)>(std::bind(& x, p1, p2)))); \
	VO_SYNC_WAIT

#define VO_SYNC_CALL_RET3(type, x, p1, p2, p3) \
	bool done = false; \
	type r; \
	io.dispatch(std::bind(&ret_sync_queue<type>, &r, &done, &cond, &io_mutex, std::function<type(void)>(std::bind(& x, p1, p2, p3)))); \
	VO_SYNC_WAIT

}

#if 0

// convert UTF-8 string to wstring
std::wstring utf8_to_wstring(const std::string& str)
{
	std::wstring_convert<std::codecvt_utf8<wchar_t>> myconv;
	return myconv.from_bytes(str);
}

// convert wstring to UTF-8 string
std::string wstring_to_utf8(const std::wstring& str)
{
	std::wstring_convert<std::codecvt_utf8<wchar_t>> myconv;
	return myconv.to_bytes(str);
}


class CAudioSessionEvents : public IAudioSessionEvents
{
	LONG _cRef;

	~CAudioSessionEvents()
	{}


public:
	CAudioSessionEvents() :
		_cRef(1)
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
	AudioSessions* m_pas = nullptr;

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

	HRESULT OnSessionCreated(IAudioSessionControl *pSessionControl)
	{
		HRESULT hr = S_OK;

		if (pSessionControl)
		{
#ifdef _DEBUG
			printf("\nNew Session Created:\n");
#endif
			// TODO, mejorar esto, hacer mejores funciones de agregado de sesiones por ej (casi hecho)
			// tambien una mejor deteccion, o hacer que siempre 0.0f vol signifique desactivado

			// Osea, en nueva sesion pueden pasar estas cosas:
			//
			// 1: Existe un fantasma de sesion anterior ya guardado, lo que ocurre con los fantasmas, que si le bajas volumen a un fantasma
			//		entonces los proximos procesos del mismo SID que se abran tienen el mismo volumen que el fantasma pero tienen SIID diferente
			//		esto es porque SndVol guarda los volumenes cada vez que una sesion desaparece (se vuelve fantasma) en el registro, es asi como
			//		las nuevas sesiones toman ese valor de registro si pertenece al mismo SID. [Status: inactive]
			//
			// 2: Existe una sesion ya abierta, entonces tiene que ser una instancia nueva, esa instancia va a tener mismo SID pero diferente SIID.
			//		en este caso el volumen no se guardo en el registro entonces esta 2nda instancia tiene volumen full. [Status: inactive o active]
			//
			// 3: No existe el SID en nuestro map, entonces tenemos que guardarlo solo si esta aplicado un vol_reduction actualmente (no en estado de quiet digamos)
			//		Esto seria lo mismo para todos los casos.
			//

			// Get two important interfaces to query for info on current session.
			IAudioSessionControl2* pSessionControl2;
			hr = pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pSessionControl2);
			ISimpleAudioVolume* pSimpleAudioVolume;
			hr = pSessionControl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&pSimpleAudioVolume);

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

#ifdef _DEBUG
			AudioSessionState State;
			hr = pSessionControl->GetState(&State);
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
			wprintf_s(L"\nNew SIID [PID]: %s [%d] State: %s\n", cpp_siid.c_str(), pid, pszState);
#endif

			//TODO
			// acabo de detectar que pueden llegar aca sesiones ya guardadas q estaban inactivas temporalmente
			// entonces termina bajando el vol dos veces, chekiar q no exista primero.

			bool found = false;
			float default_vol_of_same_sid = -1.0f;
			if (m_pas->m_sound_defaults.find(cpp_siid) == m_pas->m_sound_defaults.end())
			{
				// Find test
				for (std::unordered_map<std::wstring, AudioSessions::_AudioSessionSnap>::iterator iter = m_pas->m_sound_defaults.begin();
					iter != m_pas->m_sound_defaults.end(); ++iter)
				{
					AudioSessions::_AudioSessionSnap ass = iter->second;

					IAudioSessionControl2* pSessionControl2Snap;
					hr = ass.ae->m_pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pSessionControl2Snap);

					if (pSessionControl == ass.ae->m_pSessionControl)
						found = true;

					// If
					LPWSTR sidsnap;
					hr = pSessionControl2Snap->GetSessionIdentifier(&sidsnap); // This one is NOT unique
					std::wstring cpp_sidsnap(sidsnap);
					CoTaskMemFree(sidsnap);

					if (cpp_sidsnap == cpp_sid)
						default_vol_of_same_sid = ass.v;

					SAFE_RELEASE(pSessionControl2Snap);

					if (found || (default_vol_of_same_sid >= 0))
						break;
				}

				if (default_vol_of_same_sid >= 0)
					printf("\nSAME PROCESS FOUND!\n");

				// osea, si queda un fantasma de sesion, lo baja dos veces. habra q usar sid enves de siid.
				if (!found)
				{
					printf("\nNOT FOUND!\n");
					// TODO: maybe change this to a pretty way
					hr = m_pas->SaveSession(pSessionControl, default_vol_of_same_sid);
					if ((m_pas->m_current_vol_reduction > 0.0f) && (default_vol_of_same_sid < 0))
						hr = m_pas->ChangeVolume(pSessionControl, m_pas->m_current_vol_reduction);
				}
				else
					printf("\nFOUND!\n");
			}
			else
				printf("\nSIID YA ESTABA\n");

			SAFE_RELEASE(pSimpleAudioVolume);
			SAFE_RELEASE(pSessionControl2);
			// We dont need to release pSessionControl, msdn says it doesnt adds a reference.
		}
		return hr;
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

	ULONG n = m_pSessionControl->AddRef(); // Important: retaining a copy to a WASAPI session interface so the session never expires until released.
	if (n == 0)
		assert(false);

	m_hrStatus = m_pSessionControl->RegisterAudioSessionNotification(m_pAudioEvents);
	// registeraudiosession calls AddRef on m_pAudioEvents


	//TODO: probar poner SAFE_RELEASE(m_pSessionControl) aca, porq no me expiran las sesiones si retengo un puntero de sesion
	//NOTA: al final, es lo que quiero, siempre puedo restorar volumen en sesiones porque no expiran, aunq deberia reestablecer volumen en sesiones inactivas.
}

AudioSession::~AudioSession()
{
	if (m_pSessionControl != NULL)
	{
		m_pSessionControl->UnregisterAudioSessionNotification(m_pAudioEvents);
	}
	else
	{
		SAFE_RELEASE(m_pAudioEvents);
	}
	SAFE_RELEASE(m_pSessionControl);
}









AudioSessions::AudioSessions(const DWORD cpid)
	: m_current_vol_reduction(0.0f)
	, m_skippid(cpid)
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
HRESULT AudioSessions::SaveSession(IAudioSessionControl* pSessionControl, const float default_vol_of_same_sid)
{
	HRESULT hr = S_OK;

	std::lock_guard<std::recursive_mutex> guard(m_mutex);

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

	//TODO: detectar si se abre otro team speak, por nombre a lo sumo. analizar. un lio
	// If pid is not team speak
	if (pid != m_skippid)
	{
#ifdef _DEBUG
		AudioSessionState State;
		hr = pSessionControl->GetState(&State);
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
		wprintf_s(L"Saving SIID [PID]: %s [%d] State: %s\n", cpp_siid.c_str(), pid, pszState);
#endif

		float v;
		hr = pSimpleAudioVolume->GetMasterVolume(&v);

		_AudioSessionSnap ass;
		ass.ae.reset(new AudioSession(pSessionControl));
		if (default_vol_of_same_sid >= 0)
			ass.v = default_vol_of_same_sid;
		else
			ass.v = v;

		m_sound_defaults[cpp_siid] = ass;

		// DEBUG:
#ifdef _DEBUG
		//wprintf_s(L"Session Name: %s [%d] ret:[%d]\n pid: %d\n sid: %s\n ssid: %s\n Volume: %f\n\n", pswSession, index, hr, pid, cpp_sid.c_str(), cpp_siid.c_str(), v);
#endif
	}

	SAFE_RELEASE(pSimpleAudioVolume);
	SAFE_RELEASE(pSessionControl2);

	return hr;
}

inline
HRESULT AudioSessions::ChangeVolume(IAudioSessionControl* pSessionControl, float vol_reduction)
{
	HRESULT hr = S_OK;

	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	ISimpleAudioVolume* pSimpleAudioVolume;
	hr = pSessionControl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&pSimpleAudioVolume);

	float v;
	hr = pSimpleAudioVolume->GetMasterVolume(&v);

#ifdef _DEBUG
	printf("Changing Volume of %.2f by %.2f%%\n", v, vol_reduction);
#endif

	//TODO: cambiar volumen solamente si la sesion esta activa, y que el resto se encargue los eventos.

	//LPCGUID my_guid;
	hr = pSimpleAudioVolume->SetMasterVolume(v * ((float)1.0 - vol_reduction), NULL);

	SAFE_RELEASE(pSimpleAudioVolume);

	return hr;
}

#if 0
inline
HRESULT AudioSessions::RestoreSession(ISimpleAudioVolume* pSimpleAudioVolume, const std::wstring& cpp_siid)
{
	HRESULT hr = S_OK;

	std::lock_guard<std::mutex> guard(m_mutex);

	// dont touch non existant sessions (new session could be added when the client was talking)
	if (m_sound_defaults.find(cpp_siid) == m_sound_defaults.end()) {
		return hr;
	}

	_AudioSessionSnap ass = m_sound_defaults[siid];

	//LPCGUID my_guid;
	hr = pSimpleAudioVolume->SetMasterVolume(ass.v, NULL); // TODO: ver de usar LPCGUID si se usan events!

	return hr;
}
#endif

HRESULT AudioSessions::RestoreSessions()
{
	HRESULT hr = S_OK;

	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	// Restore all our saved sessions
	for (std::unordered_map<std::wstring, _AudioSessionSnap>::iterator iter = m_sound_defaults.begin(); iter != m_sound_defaults.end(); ++iter)
	{
		_AudioSessionSnap ass = iter->second;

		ISimpleAudioVolume* pSimpleAudioVolume;
		hr = ass.ae->m_pSessionControl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&pSimpleAudioVolume);

		hr = pSimpleAudioVolume->SetMasterVolume(ass.v, NULL); // TODO: ver de usar LPCGUID si se usan events!

		SAFE_RELEASE(pSimpleAudioVolume);
	}

	m_current_vol_reduction = 0.0f;

	return hr;
}

// TODO: move filter settings to class, and create a set method
HRESULT AudioSessions::ProcessSessions(bool change_vol = false, float vol_reduction = 0.0f)
{
	if (!m_pSessionManager2)
		return E_INVALIDARG;

	HRESULT hr = S_OK;

	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	int cbSessionCount = 0;
	//LPWSTR pswSession = NULL;

	IAudioSessionEnumerator* pSessionList = NULL;
	IAudioSessionControl* pSessionControl = NULL;

	if (change_vol && (vol_reduction == 0.0f))
		change_vol = false;

	if (change_vol)
		m_current_vol_reduction = vol_reduction;

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

		hr = SaveSession(pSessionControl);
		if (change_vol)
			hr = ChangeVolume(pSessionControl, vol_reduction);

		SAFE_RELEASE(pSessionControl);
	}

	SAFE_RELEASE(pSessionList);

	return hr;
}

// DEPRECATED
// Iterated over all audio sessions and aplies action on master volume.
// action = 0 save current volume
// action = 1 save current volume and change volume
// action = 2 restore volume
// cpid = process id of session to skip iteration
#if 0
HRESULT AudioSessions::ProcessAudioSessions(float vol_reduction, const DWORD cpid, const actions_t action)
{
	if (!m_pSessionManager2)
		return E_INVALIDARG;

	HRESULT hr = S_OK;

	int cbSessionCount = 0;
	LPWSTR pswSession = NULL;

	IAudioSessionEnumerator* pSessionList = NULL;
	IAudioSessionControl* pSessionControl = NULL;

	// Delete map before overwriting save
	if ((action == SAVE_CURRENT_AUDIO) || (action == CHANGE_AND_SAVE_AUDIO))
		m_sound_defaults.clear();

	//TODO: entonces.. rhacer esta funcion completamente
	// en el caso de restore, restorar lo que esta guardado.
	// crear una funcion para save y otra para change y otra para restore
	// poner enfasis en no llamar a GetSessionEnumerator sin antes liberar todas las referencias actuales.
	if (action == RESTORE_AUDIO)
	{
		hr = RestoreSessions();
		//SAFE_RELEASE(pSessionList);
		return hr;
	}

	// Get the current list of sessions.
	hr = m_pSessionManager2->GetSessionEnumerator(&pSessionList);

	// Get the session count.
	hr = pSessionList->GetCount(&cbSessionCount);

	for (int index = 0; index < cbSessionCount; index++)
	{

		// Get the <n>th session.
		hr = pSessionList->GetSession(index, &pSessionControl);

		hr = pSessionControl->GetDisplayName(&pswSession);

		// Get two important interfaces to query for info on current session.
		IAudioSessionControl2* pSessionControl2;
		hr = pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pSessionControl2);
		ISimpleAudioVolume* pSimpleAudioVolume;
		hr = pSessionControl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&pSimpleAudioVolume);

		// anda
		/*
		IAudioSessionControl* pSessionControl_test;
		hr = pSimpleAudioVolume->QueryInterface(__uuidof(IAudioSessionControl), (void**)&pSessionControl_test);
		if (hr != S_OK)
			printf("error\n");

		SAFE_RELEASE(pSessionControl_test);
		*/
		ULONG n = pSessionControl->AddRef();
		n = pSessionControl->Release();

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
			switch (action)
			{
				case SAVE_CURRENT_AUDIO:
					hr = SaveSession(pSessionControl, pSimpleAudioVolume, cpp_siid);
					break;
				case CHANGE_AND_SAVE_AUDIO:
					hr = SaveSession(pSessionControl, pSimpleAudioVolume, cpp_siid);
					hr = ChangeVolume(pSimpleAudioVolume, vol_reduction);
					break;
				//case RESTORE_AUDIO:
					//hr = RestoreSession(pSimpleAudioVolume, cpp_siid);
					//break;
			}
		}

		// DEBUG:
#ifdef _DEBUG
		float v;
		pSimpleAudioVolume->GetMasterVolume(&v);
		//wprintf_s(L"Session Name: %s [%d] ret:[%d]\n pid: %d\n sid: %s\n ssid: %s\n Volume: %f\n\n", pswSession, index, hr, pid, cpp_sid.c_str(), cpp_siid.c_str(), v);
#endif
		CoTaskMemFree(pswSession);
		SAFE_RELEASE(pSessionControl);
		SAFE_RELEASE(pSimpleAudioVolume);
		SAFE_RELEASE(pSessionControl2);
	}

	SAFE_RELEASE(pSessionList);

	return hr;
}
#endif



////////////////////////////////////////////////////////////////////////////////////////////////////////




VolumeOptions::VolumeOptions(const float v)
: m_as(GetCurrentProcessId()) //TODO: mejorar esto
{
	m_cpid = GetCurrentProcessId();
	m_quiet = true;
	m_vol_reduction = v;

	VolumeOptions::save_default_volume();
}

VolumeOptions::~VolumeOptions()
{
	VolumeOptions::restore_default_volume();
}

void VolumeOptions::set_volume_reduction(const float v)
{
	std::lock_guard<std::mutex> guard(mutex);

	m_vol_reduction = v;
}

float VolumeOptions::get_volume_reduction()
{
	return m_vol_reduction;
}

void VolumeOptions::restore_default_volume()
{
	std::lock_guard<std::mutex> guard(mutex);

	printf("VO: Restoring per app user default volume\n");

	m_as.RestoreSessions();
}

void VolumeOptions::save_default_volume()
{
	std::lock_guard<std::mutex> guard(mutex);

	printf("VO: Saving per app user default volume\n");

	m_as.ProcessSessions();
}

void VolumeOptions::reset_data()
{
	std::lock_guard<std::mutex> guard(mutex);

	printf("VO: Reseting call data\n");
	while (!m_calls.empty())
		m_calls.pop();

	VolumeOptions::restore_default_volume();
}


int VolumeOptions::process_talk(const bool talk_status)
{
	HRESULT hr = S_OK;

	std::lock_guard<std::mutex> guard(mutex);

	// Count the number os users currently talking using a stack.
	// NOTE: we asume TS3Client always sends events in logical order per client
	if (talk_status)
		m_calls.push(1);
	else
		m_calls.pop();

	printf("VO: Update Users currently talking: %d\n", m_calls.size());

	// if last client stoped talking, restore sounds.
	if (m_calls.empty())
	{
		printf("VO: Restoring saved audio volume\n");
		hr = m_as.RestoreSessions();
		m_quiet = true;
	}

	// if someone talked while the channel was quiet, redudce volume (else was already lowered)
	if (!m_calls.empty() && m_quiet)
	{
		printf("VO: Saving and changing audio volume\n");
		hr = m_as.ProcessSessions(true, m_vol_reduction);;
		m_quiet = false;
	}

	return 1; // TODO error codes
}






//-----------------------------------------------------------
// This function enumerates all active (plugged in) audio
// rendering endpoint devices. It prints the friendly name
// and endpoint ID string of each endpoint device.
//-----------------------------------------------------------
#define EXIT_ON_ERROR(hres)  \
	if (FAILED(hres)) { goto Exit; }

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);

#include "Functiondiscoverykeys_devpkey.h"

void PrintEndpointNames()
{
	HRESULT hr = S_OK;
	IMMDeviceEnumerator *pEnumerator = NULL;
	IMMDeviceCollection *pCollection = NULL;
	IMMDevice *pEndpoint = NULL;
	IPropertyStore *pProps = NULL;
	LPWSTR pwszID = NULL;

	hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

	hr = CoCreateInstance(
		CLSID_MMDeviceEnumerator, NULL,
		CLSCTX_ALL, IID_IMMDeviceEnumerator,
		(void**)&pEnumerator);
	EXIT_ON_ERROR(hr)

		hr = pEnumerator->EnumAudioEndpoints(
		eRender, DEVICE_STATE_ACTIVE,
		&pCollection);
	EXIT_ON_ERROR(hr)

		UINT  count;
	hr = pCollection->GetCount(&count);
	EXIT_ON_ERROR(hr)

		if (count == 0)
		{
			printf("No endpoints found.\n");
		}

	// Each loop prints the name of an endpoint device.
	for (ULONG i = 0; i < count; i++)
	{
		// Get pointer to endpoint number i.
		hr = pCollection->Item(i, &pEndpoint);
		EXIT_ON_ERROR(hr)

			// Get the endpoint ID string.
			hr = pEndpoint->GetId(&pwszID);
		EXIT_ON_ERROR(hr)

			hr = pEndpoint->OpenPropertyStore(
			STGM_READ, &pProps);
		EXIT_ON_ERROR(hr)

			PROPVARIANT varName;
		// Initialize container for property value.
		PropVariantInit(&varName);

		// Get the endpoint's friendly-name property.
		hr = pProps->GetValue(
			PKEY_Device_FriendlyName, &varName);
		EXIT_ON_ERROR(hr)

			// Print endpoint friendly name and endpoint ID.
			printf("Endpoint %d: \"%S\" (%S)\n",
			i, varName.pwszVal, pwszID);

		CoTaskMemFree(pwszID);
		pwszID = NULL;
		PropVariantClear(&varName);
		SAFE_RELEASE(pProps)
			SAFE_RELEASE(pEndpoint)
	}
	SAFE_RELEASE(pEnumerator)
		SAFE_RELEASE(pCollection)
		return;

Exit:
	printf("Error!\n");
	CoTaskMemFree(pwszID);
	SAFE_RELEASE(pEnumerator)
		SAFE_RELEASE(pCollection)
		SAFE_RELEASE(pEndpoint)
		SAFE_RELEASE(pProps)
}
#endif

#include <strsafe.h>
#include "Functiondiscoverykeys_devpkey.h"
void WinErrorLog(LPTSTR lpszFunction)
{
	// Retrieve the system error message for the last-error code

	LPVOID lpMsgBuf = NULL;
	LPVOID lpDisplayBuf = NULL;
	DWORD dw = GetLastError();

	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,
		dw,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf,
		0, NULL);

	// Display the error message and exit the process

	lpDisplayBuf = (LPVOID)LocalAlloc(LMEM_ZEROINIT,
		(lstrlen((LPCTSTR)lpMsgBuf) + lstrlen((LPCTSTR)lpszFunction) + 40) * sizeof(TCHAR));
	StringCchPrintf((LPTSTR)lpDisplayBuf,
		LocalSize(lpDisplayBuf) / sizeof(TCHAR),
		TEXT("%s failed with error %d: %s"),
		lpszFunction, dw, lpMsgBuf);
	MessageBox(NULL, (LPCTSTR)lpDisplayBuf, TEXT("Error"), MB_OK);

	LocalFree(lpMsgBuf);
	LocalFree(lpDisplayBuf);
}


static BOOL SetPrivilege(
	HANDLE hToken,          // access token handle
	LPCTSTR lpszPrivilege,  // name of privilege to enable/disable
	BOOL bEnablePrivilege   // to enable or disable privilege
	)
{
	TOKEN_PRIVILEGES tp;
	LUID luid;

	if (!LookupPrivilegeValue(
		NULL,            // lookup privilege on local system
		lpszPrivilege,   // privilege to lookup 
		&luid))        // receives LUID of privilege
	{
		return FALSE;
	}

	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = luid;
	if (bEnablePrivilege)
		tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	else
		tp.Privileges[0].Attributes = 0;

	// Enable the privilege or disable all privileges.

	if (!AdjustTokenPrivileges(
		hToken,
		FALSE,
		&tp,
		sizeof(TOKEN_PRIVILEGES),
		(PTOKEN_PRIVILEGES)NULL,
		(PDWORD)NULL))
	{
		return FALSE;
	}

	if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
	{
		return FALSE;
	}

	return TRUE;
}


void testreg()
{

	//test save registry
	LONG errorCode;
	HKEY MixerSettings;
	//std::string regFile(sconfigPath + "\\test.reg");
	std::string sconfigPath("C:\\SourceCodes\\Tekert\\VisualStudio\\Team Speak SDK\\test\\test_sound\\bin\\Debug_x64");
	std::wstring wregFile = utf8_to_wstring(sconfigPath + "\\test.reg"); // temp Rvalue reference

	HANDLE m_hToken;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &m_hToken))
		WinErrorLog(L"VO: OpenProcessToken");

	if (!SetPrivilege(m_hToken, SE_BACKUP_NAME, TRUE))
	{
		WinErrorLog(L"VO: SetPrivilege");
	}
	CloseHandle(m_hToken);


	errorCode = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\Microsoft\\Internet Explorer\\LowRegistry\\Audio\\PolicyConfig\\PropertyStore", 0, KEY_ALL_ACCESS, &MixerSettings);
	if (errorCode != ERROR_SUCCESS)
	{
		printf("VO: error opening key with  RegOpenKey\n");
		WinErrorLog(L"VO: error opening key with  RegOpenKey");
	}

	errorCode = RegSaveKeyEx(MixerSettings, wregFile.c_str(), NULL, REG_LATEST_FORMAT);
	if (errorCode != ERROR_SUCCESS)
	{
		printf("VO: error saving key with  RegSaveKeyEx %d\n", errorCode);
		//WinErrorLog(L"VO: error saving key with  RegSaveKeyEx");
	}

	// TODO, esto tengo que usar, ir una por una guardando todo
	std::wstring BaseKey(L"Software\\Microsoft\\Internet Explorer\\LowRegistry\\Audio\\PolicyConfig\\PropertyStore\\");
	std::wstring achKey(L"c63da24b_0");
	std::wstring NewKey;
	wchar_t value[8192];
	DWORD BufferSize = 8192;
	errorCode = RegGetValue(HKEY_CURRENT_USER, L"Software\\Microsoft\\Internet Explorer\\LowRegistry\\Audio\\PolicyConfig\\PropertyStore\\c63da24b_0", 0, RRF_RT_ANY, NULL, (PVOID)value, &BufferSize);
	if (errorCode != ERROR_SUCCESS)
	{
		printf("VO: error reading key with  RegGetValue %d\n", errorCode);
		//WinErrorLog(L"VO: error saving key with  RegSaveKeyEx");
	}

	RegCloseKey(MixerSettings);


	system("PAUSE");

}

BOOL IsProcessRunning2(DWORD pid)
{
	HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
	DWORD ret = WaitForSingleObject(process, 0);
	CloseHandle(process);
	return ret == WAIT_TIMEOUT;
}

BOOL IsProcessRunning(DWORD pid)
{
	HANDLE process =
		OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	const bool exists = (process != NULL);
	CloseHandle(process);
	return exists;
}


int fff(int i)
{
	return i+100;
}

void work(int i)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	printf("Aca va.. %d\n", i);
}

int work2(int i)
{
	printf("Aca va.. %d\n", i);

	if (i == 15)
	{
		int a = 2;
		a = 4;
	}

	return i;
}

#if 0
async_queue aq;
void producer()
{
	//std::function<void()> call;
	std::unique_ptr<Function> call;
	bool ok = true;
	while (ok)
	{
		ok = aq.wait_and_pop(call);
		if (!ok)
			break;

		std::function<int()> func = static_cast<GenericFunction<int()> &>(*call).function;

		func();

		//call();
	}
}


void async_calls(int i)
{
	std::default_random_engine generator;
	std::uniform_int_distribution<int> distribution(1, 3);
	int dice_roll = distribution(generator);  // generates number in the range 1..3

	std::this_thread::sleep_for(std::chrono::seconds(distribution(generator)));
	//aq.push(std::bind(work, i));

	std::unique_ptr<Function> func_ptr(new GenericFunction<void()>(std::bind(work, i)));
	//std::unique_ptr<Function> func_ptr(new GenericFunction<int()>(std::bind(work2, i)));

	aq.push(func_ptr);

}
#endif


bool g_abort = false;
void producer()
{
	bool stop_loop = false;
	while (!stop_loop)
	{
		boost::system::error_code ec;
		io.run(ec);
		if (ec)
		{
			std::cout << "[" << std::this_thread::get_id()
				<< "] Exception: " << ec << std::endl;
			g_abort = true;
			break;
		}
		io.reset();

		stop_loop = g_abort;
	}
}


void async_calls(int i)
{
	std::default_random_engine generator;
	std::uniform_int_distribution<int> distribution(1, 3);
	int dice_roll = distribution(generator);  // generates number in the range 1..3

	std::this_thread::sleep_for(std::chrono::seconds(distribution(generator)));

	//{VO_ASYNC_CALL1(work, i);}
	//VO_SYNC_CALL1(work, i);

	ASYNC_CALL(work, fff(i));
	//SYNC_CALL(work, i);

	if (i == 8)
	{
		{
			bool done = false;
			int r;
			io.dispatch(std::bind(&ret_sync_queue<int>, &r, &done, &cond, &io_mutex, std::function<int(void)>(std::bind(&work2, 15))));
			std::unique_lock<std::recursive_mutex> l(io_mutex); \
			while (!done) { cond.wait(l); };
			l.unlock();

			printf("T8 VO_SYNC_CALL_RET1 r: %d\n", r);
		}
	}

}

int testret14()
{
	return 14;
}

void async_test()
{

	std::thread th_producer(producer);

	std::thread threads[10];
	// spawn 10 threads:
	for (int i = 0; i < 10; ++i)
		threads[i] = std::thread(async_calls, i);

	{VO_SYNC_CALL1(work, 11); }
	{VO_SYNC_CALL1(work, 12); }
	{VO_SYNC_CALL1(work, 13); }

	{
		//VO_SYNC_CALL_RET1(int, work2, 14);
		//printf("VO_SYNC_CALL_RET1 r: %d\n", r);
		int aver = 14;
		int r = SYNC_CALL_RET<int>(work2, testret14());
		//VO_SYNC_CALL_RET1(int, work2, &aver);
		printf("VO_SYNC_CALL_RET1 r: %d\n", r);
	}

	for (auto& th : threads) th.join();

	std::this_thread::sleep_for(std::chrono::seconds(5));
	printf("...Pasaron los 5sec\n");
	g_abort = true;
	io.stop();
	th_producer.join();

	system("PAUSE");


}

void testtime()
{
	// Search for duplicates or last changed session to transfer default volume
	std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	std::this_thread::sleep_for(std::chrono::seconds(1));
	std::chrono::steady_clock::time_point later = std::chrono::steady_clock::now();

	std::chrono::steady_clock::duration d;

	d = (later - now);
	if (d < std::chrono::milliseconds(1900))
		printf("SI\n");
	else
		printf("NO\n");


	system("PAUSE");


	std::chrono::minutes twomin(2);
	std::cout << "Printing took "
		<< std::chrono::duration_cast<std::chrono::milliseconds>(later - now).count()
		<< " algo.\n";


	if (std::chrono::duration_cast<std::chrono::milliseconds>(later - now) > std::chrono::minutes(2))
		printf("SI\n");

	system("PAUSE");
}

struct a_class;


// como hacer para:
// 1.- transmitir un shared_ptr desde el constructor
// 2.- no permitir que se pueda copiar
struct a_class : std::enable_shared_from_this<a_class> 
{
private:

public:
	~a_class() {
		printf("%s\n", __FUNCTION__);
	}
	a_class() {
		printf("%s\n", __FUNCTION__);
		std::shared_ptr<a_class> ptr(this);
	}

	a_class(const a_class&) = delete;
	a_class& operator= (const a_class&) = delete;

	template<typename ... T>
	static std::shared_ptr<a_class> create(T&& ... all)
	{
		return std::shared_ptr<a_class>(new a_class(std::forward<T>(all)...));
	}
};



int main(int argc, char* argv[])
{
	//PrintEndpointNames();

	//char test[] = "aéボ";
	//wchar_t testw[] = L"aéボ";
	//test == 61 c3 a9 e3 83 9c 00
	//system("PAUSE");

	//async_test();

	//std::shared_ptr<a_class> p(a_class::create());

	//system("PAUSE");

	//VolumeOptions vo(0.5f, "");
	VolumeOptions* vo = new VolumeOptions(0.5f, "C:\\SourceCodes\\Tekert\\VisualStudio\\Team Speak SDK\\test\\test_sound\\bin\\Debug_x64");
	system("PAUSE");
#if 1
	for (int i = 0; i < 340; i++)
	{
		vo->process_talk(true);
		if (i % 5)
			vo->process_talk(true);
		Sleep(300);
		vo->process_talk(false);
		if (i % 5)
			vo->process_talk(false);
	}

	printf("\n\nFIN LOOP\n\n");
	system("PAUSE");
#endif
	
	//vo.process_talk(true);
	vo->process_talk(true);
	system("PAUSE");
	vo->process_talk(false);
	//vo.process_talk(false);

	system("PAUSE");

	delete vo;

	system("PAUSE");

#ifdef _DEBUG
	_CrtDumpMemoryLeaks();
#endif

	return 0;

}

