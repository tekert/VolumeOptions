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

    * Neither the name of VolumeOptions nor the names of its
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

#ifdef _WIN32

/*
    WINDOWS 7+ or Server 2008 R2+ only

    SndVol.exe auto volume manager
*/

#include <WinSDKVer.h>
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

// Only for local async calls, io_service
#include <boost/asio.hpp> // include Asio before including windows headers
#include <boost/asio/steady_timer.hpp>

#include <SDKDDKVer.h>
#include <Audiopolicy.h>
#include <Mmdeviceapi.h>
#include <Endpointvolume.h>
#include <Functiondiscoverykeys_devpkey.h>  // MMDeviceAPI.h must come first

// Visual C++ pragmas, or add these libs manualy
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "Advapi32.lib")

#include <algorithm> // for string conversion
#include <thread> // for main monitor thread
#include <condition_variable>
#include <cassert>
#include <iostream>

#include "../volumeoptions/vo_config.h"
#include "../volumeoptions/sounds_windows.h"

#include <initguid.h> // for macro DEFINE_GUID definition http://support2.microsoft.com/kb/130869/en-us
/* zero init global GUID for events, for later on AudioMonitor constructor */
DEFINE_GUID(GUID_VO_CONTEXT_EVENT_ZERO, 0x00000000L, 0x0000, 0x0000,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
// {D2C1BB1F-47D8-48BF-AC69-7E4E7B2DB6BF}  For event distinction
static const GUID GUID_VO_CONTEXT_EVENT =
{ 0xd2c1bb1f, 0x47d8, 0x48bf, { 0xac, 0x69, 0x7e, 0x4e, 0x7b, 0x2d, 0xb6, 0xbf } };

std::set<std::wstring> AudioMonitor::m_current_moniting_deviceids;
std::mutex AudioMonitor::m_static_set_access;

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
    // NOT USED, sessions not expiring
    static void DeleteSession(std::shared_ptr<AudioMonitor> pam, std::shared_ptr<AudioSession> spAudioSession)
    {
        pam->DeleteSession(spAudioSession);
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

    template <class R>
    void ret_sync_queue(R* ret, bool* done, std::condition_variable_any* e, std::recursive_mutex* m,
        std::function<R(void)> f)
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

    template <typename ft, typename... pt>
    void ASYNC_CALL(const std::shared_ptr<boost::asio::io_service>& io, ft&& f, pt&&... args)
    {
        io->post(std::bind(std::forward<ft>(f), std::forward<pt>(args)...));
    }

    template <typename ft, typename... pt>
    void SYNC_CALL(const std::shared_ptr<boost::asio::io_service>& io, ft&& f, pt&&... args)
    {
        bool done = false;
        io->dispatch(std::bind(&sync_queue, &done, &cond, &io_mutex,
            std::function<void(void)>(std::bind(std::forward<ft>(f), std::forward<pt>(args)...))));

        std::unique_lock<std::recursive_mutex> l(io_mutex);
        while (!done) { cond.wait(l); };
        l.unlock();
    }

    template <typename rt, typename ft, typename... pt>
    rt SYNC_CALL_RET(const std::shared_ptr<boost::asio::io_service>& io, ft&& f, pt&&... args)
    {
        bool done = false;
        rt r;
        io->dispatch(std::bind(&ret_sync_queue<rt>, &r, &done, &cond, &io_mutex,
            std::function<rt(void)>(std::bind(std::forward<ft>(f), std::forward<pt>(args)...))));

        std::unique_lock<std::recursive_mutex> l(io_mutex);
        while (!done) { cond.wait(l); };
        l.unlock();
        return r;
    }

    template <typename dt, typename ft, typename... pt>
        std::unique_ptr<boost::asio::steady_timer>
        ASYNC_CALL_DELAY(std::shared_ptr<boost::asio::io_service>& io,
        dt&& tdelay, ft&& f, pt&&... args)
    {
        std::unique_ptr<boost::asio::steady_timer> delay_timer =
            std::make_unique<boost::asio::steady_timer>(*io, std::forward<dt>(tdelay));
        delay_timer->async_wait(std::bind(std::forward<ft>(f), std::forward<pt>(args)...));
        return std::move(delay_timer);
    }

#if 0 // TODO in case we dont support variadic templates. DEPRECATED.

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
    1 The methods in the interface must be nonblocking. The client should never wait on a synchronization
    object during an event callback.
    2 The client should never call the IAudioSessionControl::UnregisterAudioSessionNotification method during
    an event callback.
    3 The client should never release the final reference on a WASAPI object during an event callback.

    NOTES:
    *1 To be non blocking and thread safe using our AudioMonitor class the only safe easy way is to use async calls.
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
        dprintf("OnDisplayNameChanged\n");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnIconPathChanged(
        LPCWSTR NewIconPath,
        LPCGUID EventContext)
    {
        dprintf("OnIconPathChanged\n");
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

            std::shared_ptr<AudioMonitor> spAudioMonitor(m_pAudioMonitor.lock());
            if (!spAudioMonitor)
                return S_OK;

            ASYNC_CALL(spAudioMonitor->get_io(), &AudioCallbackProxy::UpdateDefaultVolume, spAudioSession, NewVolume);
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
        dprintf("OnChannelVolumeChanged\n");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(
        LPCGUID NewGroupingParam,
        LPCGUID EventContext)
    {
        dprintf("OnGroupingParamChanged\n");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnStateChanged(
        AudioSessionState NewState)
    {
        dprintf("OnStateChanged CALLBACK: ");

        //std::shared_ptr<AudioMonitor> spAudioMonitor(m_pAudioMonitor.lock());
        std::shared_ptr<AudioSession> spAudioSession(m_pAudioSession.lock());
        if (!spAudioSession)
        {
            dprintf("spAudioSession == NULL!! on OnStateChanged()\n");
            return S_OK;
        }
        std::shared_ptr<AudioMonitor> spAudioMonitor(m_pAudioMonitor.lock());
        if (!spAudioMonitor)
            return S_OK;

        char *pszState = "?????";
        switch (NewState)
        {
        case AudioSessionStateActive:
            pszState = "active";
            ASYNC_CALL(spAudioMonitor->get_io(), &AudioCallbackProxy::ApplyVolumeSettings, spAudioSession);
            ASYNC_CALL(spAudioMonitor->get_io(), &AudioCallbackProxy::set_time_active_since, spAudioSession);
            break;
        case AudioSessionStateInactive:
            pszState = "inactive";
            ASYNC_CALL(spAudioMonitor->get_io(), &AudioCallbackProxy::RestoreVolume, spAudioSession);
            ASYNC_CALL(spAudioMonitor->get_io(), &AudioCallbackProxy::set_time_inactive_since, spAudioSession);
            break;
        case AudioSessionStateExpired: // only pops if we dont retaing a reference to the session
            pszState = "expired";
            //ASYNC_CALL(spAudioMonitor->get_io(), &AudioCallbackProxy::DeleteSession, spAudioMonitor,
            //  m_pAudioSession->getSIID());
            break;
        }
        dprintf("New session state = %s  PID[%d]\n", pszState, spAudioSession->getPID());

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
        dprintf("Audio session disconnected (reason: %s)\n",
            pszReason);
#endif
        return S_OK;
    }
};

// We dont need instances of real base, this derived class its just a fix for events.
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
        // IMPORTANT: get a ref before an async call, windows deletes the object after return
        pNewSessionControl->AddRef();

        {
            // IMPORTANT: Fix, dont know why yet, but the first callback never arrives if we dont do this now.
            CAudioSessionEvents_fix* pAudioEvents = new CAudioSessionEvents_fix();
            HRESULT hr = S_OK;
            CHECK_HR(hr = pNewSessionControl->RegisterAudioSessionNotification(pAudioEvents));
            CHECK_HR(hr = pNewSessionControl->UnregisterAudioSessionNotification(pAudioEvents));

            done:
            SAFE_RELEASE(pAudioEvents);

            if (FAILED(hr))
            {
                printf("CSessionNotifications::OnSessionCreated: Error fixing events: hr = %d\n", hr);
                return hr;
            }
        }

        if (pNewSessionControl)
        {
            dprintf("CSessionNotifications::OnSessionCreated: New Session Incoming:\n");
            std::shared_ptr<AudioMonitor> spAudioMonitor(m_pAudioMonitor.lock());
            if (spAudioMonitor)
            {
                ASYNC_CALL(spAudioMonitor->get_io(), &AudioCallbackProxy::SaveSession, spAudioMonitor,
                    pNewSessionControl, true);
            }
        }
        return S_OK;
    }
};


//////////////////////////////////////   Audio Session //////////////////////////////////////


AudioSession::AudioSession(IAudioSessionControl *pSessionControl, std::weak_ptr<AudioMonitor> wpAudioMonitor,
    float default_volume)
    : m_default_volume(default_volume)
    , m_wpAudioMonitor(wpAudioMonitor)
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
        dprintf("[ERROR] AudioSession::AudioSession pSessionControl == NULL\n");
        return;
    }

    // Fill static data first
    IAudioSessionControl2* pSessionControl2 = NULL;
    CHECK_HR(m_hrStatus = m_pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pSessionControl2));
    assert(pSessionControl2);

    LPWSTR siid = NULL;
    LPWSTR sid = NULL;

    CHECK_HR(m_hrStatus = pSessionControl2->GetSessionInstanceIdentifier(&siid)); // This one is unique
    m_siid = siid;
    CoTaskMemFree(siid);

    CHECK_HR(m_hrStatus = pSessionControl2->GetSessionIdentifier(&sid)); // This one is NOT unique
    m_sid = sid;
    CoTaskMemFree(sid);

    CHECK_HR(m_hrStatus = pSessionControl2->GetProcessId(&m_pid));

    SAFE_RELEASE(pSessionControl2);

    // Correct volume
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

    // if user default vol not set (negative) set it.
    float currrent_vol = GetCurrentVolume();
    if (m_default_volume < 0.0f)
        UpdateDefaultVolume(currrent_vol);

    /* Fix for windows, if a new session is detected when the process was just
        opened, volume change won work. wait for a bit */
    Sleep(20);

    // We just created this session handler, we dont know since when it was active/inactive, update from now.
#ifdef VO_ENABLE_EVENTS
    AudioSessionState State = AudioSessionStateInactive;
    CHECK_HR(m_hrStatus = m_pSessionControl->GetState(&State));
    if (State == AudioSessionStateActive)
        set_time_active_since();
    else
        set_time_inactive_since();

    // Apply current monitor volume settings on creation.
    ApplyVolumeSettings();
#else
    ApplyVolumeSettings();
    set_time_active_since();
#endif

    // Windows SndVol fix.
    /* if param default_volume >= 0 it means use it to change to the real SID vol default, as explained in
        AudioSession::ChangeAudio, Sessions with the same SID take their default volume
        from the last SID changed >5sec ago from the registry, see ChangeAudio for more doc. */
    if (default_volume >= 0.0f)
    {
        if ((m_default_volume != currrent_vol) && is_volume_at_default)
        {
            ChangeVolume(m_default_volume);	// fix correct windows volume before aplying settings
        }
    }

#ifdef _DEBUG
#ifndef VO_ENABLE_EVENTS
    AudioSessionState State;
    m_hrStatus = CHECK_HR(m_hrStatus = pSessionControl->GetState(&State));
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
    wprintf_s(L"*AudioSession::AudioSession Saved SIID PID[%d]: %s State: %s\n", getPID(), getSIID().c_str(),
        pszState);
#endif


done:
    // Check class status after creation. AudioSession::GetStatus() HRESULT type, if failed discart this session.
    ;

}

AudioSession::~AudioSession()
{
    dwprintf(L"~AudioSession:: PID[%d]Deleting Session %s\n", getPID(), getSID().c_str());

    StopEvents();

    RestoreVolume(AudioSession::NO_DELAY);

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
        m_pAudioEvents = new CAudioSessionEvents(this->shared_from_this(), m_wpAudioMonitor);

        // RegisterAudioSessionNotification calls another AddRef on m_pAudioEvents so Refs = 2 by now
        CHECK_HR(m_hrStatus = m_pSessionControl->RegisterAudioSessionNotification(m_pAudioEvents));
        dprintf("AudioSession::InitEvents() PID[%d] Init Session Events\n", getPID());
    }

done:
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
        // So we need to unregister and wait for current async'd events to finish before releasing our  
        // last ISessionControl.
        // NOTE: using async calls we garantize we can comply to this.
        CHECK_HR(m_hrStatus = m_pSessionControl->UnregisterAudioSessionNotification(m_pAudioEvents));
        dprintf("AudioSession::StopEvents() Stopped Session Events on PID[%d]\n", getPID());
    }

done:
    // if registered/unregistered too fast it can be > 1, assert it
    if (m_pAudioEvents != NULL)
        assert(CHECK_REFS(m_pAudioEvents) == 1);

    SAFE_RELEASE(m_pAudioEvents);
}

std::wstring AudioSession::getSID() const
{
    return m_sid;
}

std::wstring AudioSession::getSIID() const
{
    return m_siid;
}

DWORD AudioSession::getPID() const
{
    return m_pid;
}

float AudioSession::GetCurrentVolume()
{
    float current_volume = -1.0f;

    ISimpleAudioVolume* pSimpleAudioVolume = NULL;
    CHECK_HR(m_hrStatus = m_pSessionControl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&pSimpleAudioVolume));
    assert(pSimpleAudioVolume);

    CHECK_HR(m_hrStatus = pSimpleAudioVolume->GetMasterVolume(&current_volume));
    dprintf("AudioSession::GetCurrentVolume() PID[%d] = %.2f\n", getPID(), current_volume);

done:
    SAFE_RELEASE(pSimpleAudioVolume);

    return current_volume;
}

/*
    Changes Session Volume based on current config

    It uses current config to change the session volume.

    Note: SndVol uses the last changed session's volume on new instances of the same process(SID) as default vol.
    It takes ~5 sec to register a new volume level back at the registry, key:
    "HKEY_CURRENT_USER\Software\Microsoft\Internet Explorer\LowRegistry\Audio\PolicyConfig\PropertyStore"
    it overwrites sessions with the same SID.
    See AudioMonitor:SaveSession for how we use the correct default vol value without using this session SndVol level.
*/
HRESULT AudioSession::ApplyVolumeSettings()
{
    HRESULT hr = S_OK;

    std::shared_ptr<AudioMonitor> spAudioMonitor(m_wpAudioMonitor.lock());
    if (!spAudioMonitor) return S_OK; // AudioMonitor is currently shuting down.

    const float &current_vol_reduction = spAudioMonitor->m_settings.ses_global_settings.vol_reduction;
    bool reduce_vol = true;
    // Only change volume if the session is active and configured to do so
    if (spAudioMonitor->m_settings.ses_global_settings.change_only_active_sessions)
    {
        AudioSessionState State = AudioSessionStateInactive;
        m_pSessionControl->GetState(&State); // NOTE: profiler marks this line as the only hot zone ~19%.
        if (State == AudioSessionStateActive)
            reduce_vol = true;
        else
            reduce_vol = false;
    }

    // if AudioSession::is_volume_at_default is true, the session is intact, not changed, else not
    // if AudioMonitor::m_auto_change_volume_flag is true, auto volume reduction is activated, else disabled.
    if ((is_volume_at_default) && (spAudioMonitor->m_auto_change_volume_flag) && reduce_vol)
    {
        float set_vol;
        if (spAudioMonitor->m_settings.ses_global_settings.treat_vol_as_percentage)
            set_vol = m_default_volume * (1.0f - current_vol_reduction);
        else
            set_vol = current_vol_reduction;

        ChangeVolume(set_vol);
        is_volume_at_default = false; // mark that the session is NOT at default state.

        dprintf("AudioSession::ApplyVolumeSettings() PID[%d] Changed Volume to %.2f\n",
            getPID(), set_vol);
    }
    else
    {
        dprintf("AudioSession::ApplyVolumeSettings() PID[%d] skiped, flag=%d global_vol_reduction = %.2f, "
            "is_volume_at_default = %d, reduce_vol=%d \n", getPID(), spAudioMonitor->m_auto_change_volume_flag,
            current_vol_reduction, is_volume_at_default, reduce_vol);
    }

    return hr;
}

/*
    Sets new volume level as default for restore.

    Used when user changed volume manually while monitoring, updates his preference as new default, this should be
        called from callbacks when contextGUID is different of ours.
    That means we didnt change the volume. if not using events, defaults will be updated only when
        AudioMonitor is refreshing.
*/
void AudioSession::UpdateDefaultVolume(float new_def)
{
    m_default_volume = new_def;
    touch();

    dprintf("AudioSession::UpdateDefaultVolume PID[%d] (%.2f)\n", getPID(), new_def);
}

/*
    This method is used to fork calls for clarity from normal calls to async callback calls
        from AudioSession::RestoreVolume

    Is important to retain a shared_ptr so callbacks have something to call to always,
        so we dont have to manualy hold the instance before deleting..
        making this clearer and safer.

*/
void AudioSession::RestoreHolderCallback(boost::system::error_code const& e)
{
    if (e == boost::asio::error::operation_aborted)
    {
        dprintf("AudioSession::RestoreHolderCallback  PID[%d] ...Timer Cancelled\n", getPID());
        return;
    }

    if (e)
    {
        dprintf("[ERROR] AudioSession::RestoreHolderCallback PID[%d]  Asio: %s\n", getPID(), e.message().c_str());
        return;
    }

    dprintf("AudioSession::RestoreHolderCallback  PID[%d] Wait Complete Restoring Volume...\n", getPID());

    // Important: Send NO_DELAY always from here so we break the loop.
    RestoreVolume(AudioSession::NO_DELAY);

    std::shared_ptr<AudioMonitor> spAudioMonitor(m_wpAudioMonitor.lock());
    if (!spAudioMonitor) return; // AudioMonitor is currently shuting down.

    // Timer Completed. Delete it.
    spAudioMonitor->m_pending_restores.erase(this);

    // ...let asio stored AudioSession shared_ptr destroy its count now.
}

/*
    Restores Default session volume

    We saved the default volume on the session so we can restore it here
        m_default_volume always have the user preferred session default volume.
    If is_volume_at_default flag is true it means the session is already at default level.
    If is false or positive it means we have to restore it.

    callback_no_delay  -> optional parameter (default false) to indicate if we should
        create a callback timer (if configured to do so) or change vol directly.
*/
HRESULT AudioSession::RestoreVolume(const resume_t callback_no_delay)
{
    HRESULT hr = S_OK;
    assert(m_pSessionControl);

    if (!is_volume_at_default)
    {
        std::shared_ptr<AudioMonitor> spAudioMonitor(m_wpAudioMonitor.lock());
        if (!spAudioMonitor) return S_OK; // AudioMonitor is currently shuting down.

        // if delays are configured create asio timers to "self" call with callback_no_delay = true
        //		timers are also stored on AudioMonitor to cancel them when necessary.
        if (!callback_no_delay && 
            (spAudioMonitor->m_settings.ses_global_settings.vol_up_delay != std::chrono::milliseconds::zero()))
        {
            // play it safe, callback_no_delay should be true when called from destructor...
            std::shared_ptr<AudioSession> spAudioSession;
            try { spAudioSession = this->shared_from_this(); }
            catch (std::bad_weak_ptr&) { return hr; }
#ifdef _DEBUG
            if (spAudioMonitor->m_pending_restores.find(this) != spAudioMonitor->m_pending_restores.end())
                dprintf("AudioSession::RestoreVolume PID[%d] A pending restore timer is waiting... "
                    "stopping old timer and replacing it... \n", getPID());
#endif
            // Create an async callback timer :)
            // IMPORTANT: Delete timer from container when :
            //		1. Callback is completed before return.
            //		2. Volume is changed
            //		3. Monitor is started/resumed (AudioMonitor)
            //		4. A session is removed from container. (AudioMonitor)
            //          to free AudioSession destructor.
            // NOTE: dont stop timers, delete or replace them to cancel timer.
            // IMPORTANT: Use a shared_ptr per async call so we have something persistent to async! asio will copy it.
            spAudioMonitor->m_pending_restores[this] =
                ASYNC_CALL_DELAY(spAudioMonitor->m_io, spAudioMonitor->m_settings.ses_global_settings.vol_up_delay,
                &AudioSession::RestoreHolderCallback, spAudioSession, std::placeholders::_1);

            dprintf("AudioSession::RestoreVolume PID[%d] Created and saved delayed callback\n", getPID());

#if 0
            // Create an async callback timer :)
            std::unique_ptr<boost::asio::steady_timer> delay_timer =
                std::make_unique<boost::asio::steady_timer>(*m_AudioMonitor.m_io);
            delay_timer->expires_from_now(m_AudioMonitor.m_settings.vol_up_delay);
#ifdef _DEBUG
            if (m_AudioMonitor.m_pending_restores.find(this) != m_AudioMonitor.m_pending_restores.end())
                dprintf("AudioSession::RestoreVolume PID[%d] A pending restore timer is waiting... "
                    "stopping old timer and replacing it... \n", getPID());
#endif
            // IMPORTANT: Send a shared_ptr trough async call so we have something persistent to async!
            delay_timer->async_wait(std::bind(&AudioSession::RestoreHolderCallback, spAudioSession, std::placeholders::_1));
            // IMPORTANT: Delete timer from container when :
            //		1. Callback is completed before return.
            //		2. Volume is changed
            //		3. A session is removed from container. (AudioMonitor)
            //          to free AudioSession destructor.
            // NOTE: dont stop timers, delete or replace them to cancel timer.
            m_AudioMonitor.m_pending_restores[this] = std::move(delay_timer);

#ifdef _DEBUG
            printf("AudioSession::RestoreVolume PID[%d] Created and saved delayed callback\n", getPID());
#endif
#endif
            return hr;
        }

        // Now... restore
        ChangeVolume(m_default_volume);

        dprintf("AudioSession::RestoreVolume PID[%d] Restoring Volume of Session to %.2f\n", getPID(), m_default_volume);

        is_volume_at_default = true; // to signal the session is at default state.
    }
    else
    {
        dprintf("AudioSession::RestoreVolume PID[%d] Restoring Volume already at default state = %.2f\n", getPID(), m_default_volume);
    }

    return hr;
}

/*
    Forces volume on session
*/
void AudioSession::ChangeVolume(const float v)
{
    std::shared_ptr<AudioMonitor> spAudioMonitor(m_wpAudioMonitor.lock());
    if (!spAudioMonitor) return; // AudioMonitor is currently shuting down.

    ISimpleAudioVolume* pSimpleAudioVolume = NULL;
    CHECK_HR(m_hrStatus = m_pSessionControl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&pSimpleAudioVolume));
    assert(pSimpleAudioVolume);

    // IMPORTANT: Restore vol Timer is no longer needed, caducated.
    spAudioMonitor->m_pending_restores.erase(this);

    CHECK_HR(m_hrStatus = pSimpleAudioVolume->SetMasterVolume(v, &GUID_VO_CONTEXT_EVENT));
    touch();

    dprintf("AudioSession::ChangeVolume PID[%d] Forcing volume level = %.2f\n", getPID(), v);

done:
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


AudioMonitor::AudioMonitor(const vo::monitor_settings& settings, const std::wstring& device_id)
    : m_current_status(STOPPED)
    , m_wsDeviceID(device_id)
    , m_auto_change_volume_flag(false)
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

    // Initialize unique GUID for own events if not already created.
    //if (IsEqualGUID(GUID_VO_CONTEXT_EVENT_ZERO, GUID_VO_CONTEXT_EVENT))
    //CHECK_HR(hr = CoCreateGuid(&GUID_VO_CONTEXT_EVENT));

    dprintf("AudioMonitor GetSessionManager()...\n");
    try 
    { 
        hr = GetSessionManager(); 
        if (FAILED(hr))
            throw std::exception("HRESULT ERROR ", hr); // TODO boost::error_codes
    }
    catch (std::exception& e)
    { 
        std::cerr << e.what();
        m_current_status = AudioMonitor::INITERROR;
        return; 
    } // TODO error codes, log.
    dprintf("AudioMonitor Got Device OK.\n");

    m_processid = GetCurrentProcessId();

    SetSettings(m_settings);

    // Start thread after pre init is complete.
    // m_current_status flag will be set to ok when thread init is complete.
    m_current_status = AudioMonitor::INITERROR; 
    m_io.reset(new boost::asio::io_service);
    m_thread_monitor = std::thread(&AudioMonitor::poll, this);

    // queue a sync call, when it returns, thread init is complete and io_service is running.
    monitor_status_t status = SYNC_CALL_RET<monitor_status_t>(m_io, &AudioMonitor::GetStatus, this);

    dprintf("\t--AudioMonitor init complete--\n");
}

AudioMonitor::~AudioMonitor()
{
    if (!AudioMonitor::INITERROR)
    {
        // Exit MonitorThread
        m_abort = true;
        m_io->stop();
        // NOTE: define BOOST_ASIO_DISABLE_IOCP on windows if you want stop() to return as soon as it finishes
        //  executing current handler or if has no pending handlers to run, if not defined on windows it will wait until
        //  it has no pending handlers to run (empty queue only).
        if (m_thread_monitor.joinable())
            m_thread_monitor.join(); // wait to finish

        std::lock_guard<std::mutex> l(m_static_set_access);
        m_current_moniting_deviceids.erase(m_wsDeviceID);
    }

    // Stop everything and restore to default state
    // Unregister Event callbacks before destruction
    //	if we dont want memory leaks.
    // and delete all references to AudioSessions.
    Stop();

    SAFE_RELEASE(m_pSessionEvents);
    SAFE_RELEASE(m_pSessionManager2);

    dprintf("\n\t...AudioMonitor destroyed succesfuly.\n");
}

/*
    Simple get for asio io_service of AudioMonitor

    In case we need to queue calls from other places.
    Warning: When class is destroyed, user will have to reset this pointer, is no longer useful.
    But it garantees it wont be destroyed when it has work to do.
*/
std::shared_ptr<boost::asio::io_service> AudioMonitor::get_io()
{
    return m_io;
}

/*
    Poll for new calls on current thread

    When events are enabled this is the only way to guarantee what msdn says
    We do this using async calls from callbacks threads to this one.
    It locks the mutex so no other thread can access the class while polling.
    It also makes it easy to manage all the settings/events using only 1 thread.
*/
void AudioMonitor::poll()
{
    // No other thread can use this class while pooling
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    // IMPORTANT: call CoInitializeEx in this thread. not in constructors thread
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    dprintf("\n\t...AudioMonitor Thread Init\n\n");

    boost::asio::io_service::work work(*m_io);

#ifdef VO_ENABLE_EVENTS
    std::shared_ptr<boost::asio::steady_timer> expired_session_removal_timer =
        std::make_shared<boost::asio::steady_timer>(*m_io);
    expired_session_removal_timer->expires_from_now(m_delete_expired_interval);
    expired_session_removal_timer->async_wait(std::bind(&AudioMonitor::DeleteExpiredSessions,
        this, std::placeholders::_1, expired_session_removal_timer));
#endif

    m_current_status = AudioMonitor::monitor_status_t::STOPPED;

    // Call Dispatcher
    bool stop_loop = false;
    while (!stop_loop)
    {
        boost::system::error_code ec;
        m_io->run(ec);
        if (ec)
        {
            std::cerr << "[ERROR] Asio msg: " << ec.message().c_str() << std::endl;
        }
        m_io->reset();

        stop_loop = m_abort;
    }
}

/*
    Specifies the ducking options for the application.

    pSessionManager2 -> An already referenced pSessionManager2
    If DuckingOptOutChecked is TRUE system ducking is disabled; 
    FALSE, system ducking is enabled.
*/
HRESULT DuckingOptOut(bool DuckingOptOutChecked, IAudioSessionManager2* pSessionManager2)
{
    HRESULT hr = S_OK;

    if (!pSessionManager2)
        return E_INVALIDARG;

    // Disable ducking experience and later restore it if it was enabled
    IAudioSessionControl2* pSessionControl2 = NULL;
    IAudioSessionControl* pSessionControl = NULL;

    CHECK_HR(hr = pSessionManager2->QueryInterface(__uuidof(IAudioSessionControl), (void**)&pSessionControl));
    assert(pSessionControl);

    CHECK_HR(hr = pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pSessionControl2));
    assert(pSessionControl2);
    
    if (DuckingOptOutChecked)
    {
        CHECK_HR(hr = pSessionControl2->SetDuckingPreference(TRUE));
    }
    else
    {
        CHECK_HR(hr = pSessionControl2->SetDuckingPreference(FALSE));
    }

done:
    SAFE_RELEASE(pSessionControl2);
    SAFE_RELEASE(pSessionControl);

    return hr;
}

std::set<std::wstring> AudioMonitor::GetCurrentMonitoredEndpoints()
{
    std::lock_guard<std::mutex> l(m_static_set_access);

    return m_current_moniting_deviceids;
}

/*
    Handy Method to enumerates all audio endpoints devices.

    This method can be used to obtain ids of audio endpoint and use them on AudioMonitor constructor.
    Returns a map of wstrings DeviceIDs -> FriendlyName. Use desired wstring DeviceID on AudioMonitor creation.

    dwStateMask posible values are:
    http://msdn.microsoft.com/en-us/library/windows/desktop/dd370823%28v=vs.85%29.aspx
    they are used on this call:
    http://msdn.microsoft.com/en-us/library/windows/desktop/dd371400%28v=vs.85%29.aspx dwStateMask [in]

    On this method you have to manually check return on error.
*/
HRESULT AudioMonitor::GetEndpointsInfo(std::map<std::wstring, std::wstring>& audio_endpoints, DWORD dwStateMask)
{
    HRESULT hr = S_OK;
    IMMDeviceEnumerator *pEnumerator = NULL;
    IMMDeviceCollection *pCollection = NULL;
    IMMDevice *pEndpoint = NULL;
    IPropertyStore *pProps = NULL;
    LPWSTR pwszID = NULL;
    bool uninitialize_com = true;

    audio_endpoints.clear();

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if ((hr == RPC_E_CHANGED_MODE) || (hr == S_FALSE))
        uninitialize_com = false;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        (void**)&pEnumerator);
    CHECK_HR(hr)
    
    // DEVICE_STATE_ACTIVE by default
    hr = pEnumerator->EnumAudioEndpoints(eRender, dwStateMask, &pCollection);
    CHECK_HR(hr)

    UINT  count;
    hr = pCollection->GetCount(&count);
    CHECK_HR(hr)

    if (count == 0)
    {
        printf("No endpoints found.\n");
    }

    // Each loop prints the name of an endpoint device.
    for (ULONG i = 0; i < count; i++)
    {
        // Get pointer to endpoint number i.
        hr = pCollection->Item(i, &pEndpoint);
        CHECK_HR(hr)

        // Get the endpoint ID string.
        hr = pEndpoint->GetId(&pwszID);
        CHECK_HR(hr)

        hr = pEndpoint->OpenPropertyStore(STGM_READ, &pProps);
        CHECK_HR(hr)

        PROPVARIANT varName;
        // Initialize container for property value.
        PropVariantInit(&varName);

        // Get the endpoint's friendly-name property.
        hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
        CHECK_HR(hr)

        // Print endpoint friendly name and endpoint ID.
        audio_endpoints[pwszID] = varName.pwszVal;

        CoTaskMemFree(pwszID);
        pwszID = NULL;
        PropVariantClear(&varName);
        SAFE_RELEASE(pProps)
        SAFE_RELEASE(pEndpoint)
    }

done:
    if (FAILED(hr))
        printf("Error getting audio endpoints list\n");

    CoTaskMemFree(pwszID);
    SAFE_RELEASE(pEnumerator)
    SAFE_RELEASE(pCollection)
    SAFE_RELEASE(pEndpoint)
    SAFE_RELEASE(pProps)

    if (uninitialize_com)
        CoUninitialize();

    return hr;
}

/*
    Creates the IAudioSessionManager instance on default output device
*/
HRESULT AudioMonitor::GetSessionManager()
{
    HRESULT hr = S_OK;

    IMMDevice* pDevice = NULL;
    IMMDeviceEnumerator* pEnumerator = NULL;
    LPWSTR pwszID = NULL;
    bool uninitialize_com = true;

    std::lock_guard<std::mutex> l(m_static_set_access);

    // Call this from the threads doing work on WAPI interfaces. (in this case AudioMonitor thread)
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr == S_FALSE)
        printf("AudioMonitor::GetSessionManager  CoInitializeEx: The COM library is already initialized on "
        "this thread.");
    if (hr == RPC_E_CHANGED_MODE)
        printf("AudioMonitor::GetSessionManager  CoInitializeEx: A previous call to CoInitializeEx specified "
        "the concurrency model for this thread ");
    if ((hr == RPC_E_CHANGED_MODE) || (hr == S_FALSE))
        uninitialize_com = false;

    dprintf("AudioMonitor CreateSessionManager() Getting Manager2 instance from device...\n");

    // Create the device enumerator.
    CHECK_HR(hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        (void**)&pEnumerator));
    assert(pEnumerator != NULL);

    // If user specified and endpoint ID to monitor, use it, if not use default.
    if (m_wsDeviceID.empty())
    {
        // Get the default audio device.
        CHECK_HR(hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice));
        assert(pDevice != NULL);

        CHECK_HR(hr = pDevice->GetId(&pwszID));
        m_wsDeviceID = pwszID;
        CoTaskMemFree(pwszID);
        pwszID = NULL;
    }
    else
    {
        // Get user specified device using its device id.
        CHECK_HR(hr = pEnumerator->GetDevice(m_wsDeviceID.c_str(), &pDevice));
    }

    // if we are already monitoring this endpoint, abort
    if (m_current_moniting_deviceids.count(m_wsDeviceID))
        throw std::exception("Already monitoring this endpoint");
    // static set so we dont monitor the same endpoints on multiple instances of AudioMonitor.
    m_current_moniting_deviceids.insert(m_wsDeviceID);

    // Get the session manager. (this will fail on vista and below)
    CHECK_HR(hr = pDevice->Activate(
        __uuidof(IAudioSessionManager2), CLSCTX_ALL,
        NULL, (void**)&m_pSessionManager2));
    assert(m_pSessionManager2);

   // Disable ducking experience and later restore it if it was enabled
   // TODO: i dont know how to get current status to restore it later, better dont touch it then,
   //    let the user handle it.
   // CHECK_HR(hr = DuckingOptOut(true, m_pSessionManager2));

done:
    if (pwszID)
        CoTaskMemFree(pwszID);
    SAFE_RELEASE(pEnumerator);
    SAFE_RELEASE(pDevice);

    if (uninitialize_com)
        CoUninitialize();

    return hr;
}

/*
    Deletes all sessions and adds current ones

    Uses windows7+ enumerator to list all SndVol sessions
        then saves each session found.
*/
HRESULT AudioMonitor::RefreshSessions()
{
    if (!m_pSessionManager2)
        return E_INVALIDARG;

    HRESULT hr = S_OK;

    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    int cbSessionCount = 0;

    IAudioSessionEnumerator* pSessionList = NULL;
    IAudioSessionControl* pSessionControl = NULL;

    // Delete saved sessions before calling session enumerator, it will release all our saved references.
    DeleteSessions();

    // Get the current list of sessions.
    // IMPORTANT NOTE: DONT retain references to IAudioSessionControl before calling this function,
    //      it causes memory leaks.
    // IMPORTANT NOTE2: We have to use this call if we want receive new session notifications
    // http://msdn.microsoft.com/en-us/library/dd368281%28v=vs.85%29.aspx point 5. (verified)
    CHECK_HR(hr = m_pSessionManager2->GetSessionEnumerator(&pSessionList));

    // Get the session count.
    CHECK_HR(hr = pSessionList->GetCount(&cbSessionCount));

    dprintf("\n\n------ Refreshing sessions...\n\n");

    const bool unref_there = false;
    for (int index = 0; index < cbSessionCount; index++)
    {
        // Get the <n>th session.
        CHECK_HR(hr = pSessionList->GetSession(index, &pSessionControl));

        hr = SaveSession(pSessionControl, unref_there); // TODO handle error.

        SAFE_RELEASE(pSessionControl);
    }

done:
    SAFE_RELEASE(pSessionControl);
    SAFE_RELEASE(pSessionList);

    return hr;
}

/*
    Deletes all sessions from multimap
*/
void AudioMonitor::DeleteSessions()
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    // First delete all pending timers on audio sessions.
    m_pending_restores.clear();

    // then delete map to call AudioSession destructors.
    m_saved_sessions.clear();

    dwprintf(L"AudioMonitor::DeleteSessions() Saved sessions cleared\n");
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
        dprintf("ASIO Timer cancelled: %s\n", e.message().c_str());
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
            dwprintf(L"\nSession PID[%d] Too old, removing...\n", it->second->getPID());
            //  NOTE: if we dont erase the timer first, callback will be active until timeout
            //      and we wont get new session notifications of that session until it deletes itself.
            //      they each contain a shared_ptr to AudioSession.
            m_pending_restores.erase(it->second.get());
            it = m_saved_sessions.erase(it);
        }
        else
            it++;
    }

    dwprintf(L". DeleteExpired tick\n");
}

/*
    Applies current settings on all class elements.
*/
void AudioMonitor::ApplySettings()
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    // Search current saved sessions and remove based on settings.
    for (auto it = m_saved_sessions.begin(); it != m_saved_sessions.end();)
    {
        bool excluded = isSessionExcluded(it->second->getPID(), it->second->getSID());
        if (excluded)
        {
            dwprintf(L"\nRemoving PID[%d] due to new config...\n", it->second->getPID());
            it = m_saved_sessions.erase(it);
        }
        else
            it++;
    }

    if (m_auto_change_volume_flag)
    {
        // Update all session's volume
        for (auto it = m_saved_sessions.begin(); it != m_saved_sessions.end(); ++it)
        {
            assert(it->second->m_pSessionControl);
            it->second->ApplyVolumeSettings();
        }
    }
}

/*
    Return true if audio session is excluded from monitoring.
*/
bool AudioMonitor::isSessionExcluded(DWORD pid, std::wstring sid)
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    if (m_settings.exclude_own_process)
    {
        if (m_processid == pid)
            return true;
    }

    if (!sid.empty())
    {
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
    }

    return false;
}

/*
    Saves a New session in multimap (core method)

    Group of session with different SIID (SessionInstanceIdentifier) but with the same SID (SessionIdentifier) are
        grouped togheter on the same key (SID).
    See more notes inside.
    Before adding the session it detects whanever this session is another instance of the same process (same SID)
        and copies the default volume of the last changed session. We need to do this because if we dont, it will
        take the reduced volume of the other SID session as default..
 
    IAudioSessionControl* pSessionControl  -> pointer to the new session
    bool unref  -> do we Release() inside this function or not? useful for async calls
*/
HRESULT AudioMonitor::SaveSession(IAudioSessionControl* pSessionControl, bool unref)
{
    HRESULT hr = S_OK;

    if (!pSessionControl)
        return E_INVALIDARG;

    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    std::wstring ws_sid;
    std::wstring ws_siid;
    DWORD pid = 0;

    assert(pSessionControl);
    IAudioSessionControl2* pSessionControl2 = NULL;
    CHECK_HR(hr = pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pSessionControl2));
    assert(pSessionControl2);

    CHECK_HR(hr = pSessionControl2->GetProcessId(&pid));

    LPWSTR _sid = NULL;
    CHECK_HR(hr = pSessionControl2->GetSessionIdentifier(&_sid)); // This one is NOT unique
    ws_sid = _sid;
    CoTaskMemFree(_sid);

    bool excluded = isSessionExcluded(pid, ws_sid);

    if ((pSessionControl2->IsSystemSoundsSession() == S_FALSE) && !excluded)
    {
        dwprintf(L"\n---Saving New Session: PID[%d]:\n", pid);

        LPWSTR _siid = NULL;
        CHECK_HR(hr = pSessionControl2->GetSessionInstanceIdentifier(&_siid)); // This one is unique
        ws_siid =_siid;
        CoTaskMemFree(_siid);

        bool duplicate = false;

        //  If a SID already exists, copy the default volume of the last changed session of the same SID.
        //  As SndVol does: always copies the sound volume of the last changed session, try opening the same music
        //      player 3 times before the 3rd change volume to the second instance and then to the first, wait >5sec 
        //      and open the 3rd, SndVol will take the 1rst one volume. NOTE: Takes 5sec for SndVol to register last 
        //      volume of SID on the registry at least on win7. 
        //  Registry saves only SIDs so they overwrite each other, the last one takes precedence.
        float last_sid_volume_fix = -1.0;
        if (m_saved_sessions.count(ws_sid))
        {
            dprintf("AudioMonitor::SaveSession - Equal SID detected bucket_size=%llu\n", m_saved_sessions.count(ws_sid));

            auto range = m_saved_sessions.equal_range(ws_sid);

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
                    dwprintf(L"AudioMonitor::SaveSession - Equal SIID detected, DUPLICATE discarting...\n");
                    duplicate = true;
                    break;
                }
            }

            last_sid_volume_fix = last_changed->second->m_default_volume;
            dwprintf(L"AudioMonitor::SaveSession PID[%d] Copying default volume of last session PID[%d] %.2f\n", pid,
                    last_changed->second->getPID(), last_sid_volume_fix);
        }

        if (!duplicate)
        {
            // Initialize the new AudioSession and store it.
            //std::shared_ptr<AudioSession> pAudioSession = std::make_shared<AudioSession>(pSessionControl,
                //*this, last_sid_volume_fix); // TODO: doesnt work with private constructor
            std::shared_ptr<AudioSession> pAudioSession(new AudioSession(pSessionControl, this->shared_from_this(),
                last_sid_volume_fix));

            if (!FAILED(pAudioSession->GetStatus()))
            {

#ifdef VO_ENABLE_EVENTS
                pAudioSession->InitEvents();
#endif
                // Save session
                m_saved_sessions.insert(t_session_pair(ws_sid, pAudioSession));

            }
            else
                dwprintf(L"---AudioMonitor::SaveSession PID[%d] ERROR creating session\n", pid);
        }
    }
    else
    {
        dwprintf(L"---AudioMonitor::SaveSession PID[%d] skipped...\n", pid);
    }

done:
    SAFE_RELEASE(pSessionControl2);
    if (unref)
        SAFE_RELEASE(pSessionControl);

    return hr;
}

/*
    Deletes a single session from container
*/
void AudioMonitor::DeleteSession(std::shared_ptr<AudioSession> spAudioSession)
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    std::wstring ws_sid = spAudioSession->getSID();
    std::wstring ws_siid = spAudioSession->getSIID();
    if (m_saved_sessions.count(ws_sid))
    {
        auto range = m_saved_sessions.equal_range(ws_sid);
        for (auto it = range.first; it != range.second; ++it)
        {
            if (it->second->getSIID() == ws_siid)
            {
                // NOTE: if we dont erase the timer first, callback will be active until timeout
                //      and we wont get new session notifications of that session until it deletes itself.
                //      they each contain a shared_ptr to AudioSession.
                m_pending_restores.erase(spAudioSession.get());
                m_saved_sessions.erase(it);
                break;
            }
        }
    }
}

/*
    Stops monitoring current sessions

    First we stop events so new sessions cant come in
    Then we set volume change flag to false
    Finaly we delete all sessions, restoring default volume.
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
        ret = SYNC_CALL_RET<long>(m_io, &AudioMonitor::Stop, this);
    }
    else
    {
        if (m_current_status == AudioMonitor::monitor_status_t::INITERROR)
            return -1;

        if (m_current_status == AudioMonitor::monitor_status_t::STOPPED)
        {
            dwprintf(L"\n\t ---- AudioMonitor::Stop() Monitor already Stopped .... \n");
            return 0;
        }

        // Global class flag , volume reduction inactive
        m_auto_change_volume_flag = false;

#ifdef VO_ENABLE_EVENTS
        // First we stop new sessions from coming
        ret = StopEvents();
#endif

        // Deletes all sessions restoring volume
        DeleteSessions();

        dwprintf(L"\n\t ---- AudioMonitor::Stop() STOPPED .... \n\n");
        m_current_status = AudioMonitor::monitor_status_t::STOPPED;
    }

    return static_cast<long>(ret);
}

#ifdef VO_ENABLE_EVENTS 
/*
    Enables ISessionManager2 Events for new sessions

    Registers the class for callbacks to receive notifications on new sessions.
    NOTE: For this to work we need to release all our session references and "refresh" get current SndVol sessions
        see AudioMonitor::RefreshSession & AudioMonitor::Start

    TODO: do error codes.
*/
long AudioMonitor::InitEvents()
{
    HRESULT ret = S_OK;

    //if (std::this_thread::get_id() != m_thread_monitor.get_id())
    std::unique_lock<std::recursive_mutex> l(m_mutex, std::try_to_lock);

    if (!l.owns_lock())
    {
        ret = SYNC_CALL_RET<long>(m_io, &AudioMonitor::InitEvents, this);
    }
    else
    {
        if (m_current_status == AudioMonitor::monitor_status_t::INITERROR)
            return -1;

        // Events for adding new sessions
        if ((m_pSessionManager2 != NULL) && (m_pSessionEvents == NULL))
        {
            m_pSessionEvents = new CSessionNotifications(this->shared_from_this()); // AddRef() on constructor
            CHECK_HR(ret = m_pSessionManager2->RegisterSessionNotification(m_pSessionEvents));
            assert(m_pSessionEvents);

            dprintf("AudioMonitor::InitEvents() Init Sessions notification events completed.\n");

        done:;
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
        ret = SYNC_CALL_RET<long>(m_io, &AudioMonitor::StopEvents, this);
    }
    else
    {
        if (m_current_status == AudioMonitor::monitor_status_t::INITERROR)
            return -1;

        // Stop new session notifications
        if ((m_pSessionManager2 != NULL) && (m_pSessionEvents != NULL))
        {
            CHECK_HR(ret = m_pSessionManager2->UnregisterSessionNotification(m_pSessionEvents));
            dprintf("AudioMonitor::InitEvents() Stopped Sessions notification events.\n");
        }
        else
        {
            dprintf("AudioMonitor::InitEvents() Sessions notification events already stopped or no instanced.\n");
        }

        if (m_pSessionEvents != NULL)
            assert(CHECK_REFS(m_pSessionEvents) == 1);

        done:
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
        ret = SYNC_CALL_RET<long>(m_io, &AudioMonitor::Pause, this);
    }
    else
    {
        if (m_current_status == AudioMonitor::monitor_status_t::INITERROR)
            return -1;

        if (m_current_status == AudioMonitor::monitor_status_t::STOPPED)
        {
            dwprintf(L"\n\t ---- AudioMonitor::Pause() Monitor already Stopped .... \n");
            return 0;
        }
        if (m_current_status == AudioMonitor::monitor_status_t::PAUSED)
        {
            dwprintf(L"\n\t ---- AudioMonitor::Pause() Monitor already Paused .... \n");
            return 0;
        }

        // Global class flag , volume reduction inactive
        m_auto_change_volume_flag = false;

        // Restore Volume of all sessions currently monitored
        for (auto it = m_saved_sessions.begin(); it != m_saved_sessions.end(); ++it)
        {
            assert(it->second->m_pSessionControl);
            it->second->RestoreVolume();
        }

        dwprintf(L"\n\t ---- AudioMonitor::Pause  PAUSED .... \n\n");
        m_current_status = AudioMonitor::monitor_status_t::PAUSED;
    }
#endif

    return static_cast<long>(ret);
}

/*
    Starts monitoring sessions to change volume on

    Each time we start the monitor we refresh all current sessions (useful when not using events too)
        auto_change_volume_flag indicates on class level if we are currently
        changing volume or not, if its false it means AudioSession::ApplyVolumeSettings
        will have no effect and all sessions will be at default level.
    If not, it means we will change sessions volume every time an event o start is triggered.
*/
long AudioMonitor::Start()
{
    HRESULT ret = S_OK;

    //if (std::this_thread::get_id() != m_thread_monitor.get_id())
    std::unique_lock<std::recursive_mutex> l(m_mutex, std::try_to_lock);
    if (!l.owns_lock())
    {
        ret = SYNC_CALL_RET<long>(m_io, &AudioMonitor::Start, this);
    }
    else
    {
        if (m_current_status == AudioMonitor::monitor_status_t::INITERROR)
            return -1;

        if (m_current_status == AudioMonitor::monitor_status_t::RUNNING)
        {
            dwprintf(L"\n\t ---- AudioMonitor::Start() Monitor already Running .... \n");
            return 0;
        }

        if (m_current_status == AudioMonitor::monitor_status_t::STOPPED)
        {
            dwprintf(L"\n\t .... AudioMonitor::Start() STARTED ----\n\n");

            // IMPORTANT:
            // see http://msdn.microsoft.com/en-us/library/dd368281%28v=vs.85%29.aspx remarks point 5(five)
            // we must use the enumerator first or we wont receive new session notifications.
            ret = RefreshSessions(); // Deletes and re adds current sessions

#ifdef VO_ENABLE_EVENTS
            /* Now we enable new incoming sessions. */
            ret = InitEvents();
#endif
        }

        if (m_current_status == AudioMonitor::monitor_status_t::PAUSED)
        {
            dwprintf(L"\n\t .... AudioMonitor::Start() RESUMED ----\n\n");

            // Old pending restores are caducated.
            m_pending_restores.clear();
        }

        // Activate reduce volume flag and update vol reduction
        m_auto_change_volume_flag = true;

        // Update all session's volume based on current settings
        for (auto it = m_saved_sessions.begin(); it != m_saved_sessions.end(); ++it)
        {
            assert(it->second->m_pSessionControl);
            it->second->ApplyVolumeSettings();
        }

        m_current_status = AudioMonitor::monitor_status_t::RUNNING;
    }

    return static_cast<long>(ret); // TODO: error codes
}

/*
    Simple thread safe proxy, see AudioMonitor::RefreshSessions for more info.
*/
long AudioMonitor::Refresh()
{
    HRESULT ret = S_OK;

    //if (std::this_thread::get_id() != m_thread_monitor.get_id())
    std::unique_lock<std::recursive_mutex> l(m_mutex, std::try_to_lock);

    if (!l.owns_lock())
    {
        ret = SYNC_CALL_RET<long>(m_io, &AudioMonitor::Refresh, this);
    }
    else
    {
        if (m_current_status == AudioMonitor::monitor_status_t::INITERROR)
            return -1;

        ret = RefreshSessions();
    }

    return static_cast<long>(ret); // TODO: error codes
}

/*
    Gets current config
*/
vo::monitor_settings AudioMonitor::GetSettings()
{
    vo::monitor_settings ret;

    std::unique_lock<std::recursive_mutex> l(m_mutex, std::try_to_lock);

    if (!l.owns_lock())
    {
        ret = SYNC_CALL_RET<vo::monitor_settings>(m_io, &AudioMonitor::GetSettings, this);
    }
    else
    {
        ret = m_settings;
    }

    return ret;
}

/*
    Parses new config and applies it.

    If parameters are incorrect we try to correct them.
    Finally parameter settings will be modified with actual settings applied.
*/
void AudioMonitor::SetSettings(vo::monitor_settings& settings)
{
    std::unique_lock<std::recursive_mutex> l(m_mutex, std::try_to_lock);

    if (!l.owns_lock())
    {
        SYNC_CALL(m_io, &AudioMonitor::SetSettings, this, std::ref(settings));
    }
    else
    {
        if (m_current_status == AudioMonitor::monitor_status_t::INITERROR)
            return;

        m_settings = settings;

        if (!m_settings.use_included_filter)
        {
            // convert process names to lower
            for (auto n : m_settings.excluded_process)
            {
                std::transform(n.begin(), n.end(), n.begin(), ::tolower);
            }
        }
        else
        {
            // convert process names to lower
            for (auto n : m_settings.included_process)
            {
                std::transform(n.begin(), n.end(), n.begin(), ::tolower);
            }
        }

        if (m_settings.ses_global_settings.vol_reduction < 0)
            m_settings.ses_global_settings.vol_reduction = 0.0f;

        if (!m_settings.ses_global_settings.treat_vol_as_percentage)
        {
            if (m_settings.ses_global_settings.vol_reduction > 1.0f)
                m_settings.ses_global_settings.vol_reduction = 1.0f;
        }

        // TODO: complete here when adding options, if compiler with different align is used, comment this line.
        //static_assert(sizeof(vo::monitor_settings) == 144, "Update AudioMonitor::SetSettings!"); // a reminder, read todo.




        ApplySettings();

        // return applied settings
        settings = m_settings;
    }

}

float AudioMonitor::GetVolumeReductionLevel()
{
    float ret;

    std::unique_lock<std::recursive_mutex> l(m_mutex, std::try_to_lock);

    if (!l.owns_lock())
    {
        ret = SYNC_CALL_RET<float>(m_io, &AudioMonitor::GetVolumeReductionLevel, this);
    }
    else
    {
        if (m_current_status == AudioMonitor::monitor_status_t::INITERROR)
            return -1.0f;

        ret = m_settings.ses_global_settings.vol_reduction;
    }

    return ret;
}

auto AudioMonitor::GetStatus() -> monitor_status_t
{
    monitor_status_t ret;

    std::unique_lock<std::recursive_mutex> l(m_mutex, std::try_to_lock);

    if (!l.owns_lock())
    {
        ret = SYNC_CALL_RET<monitor_status_t>(m_io, &AudioMonitor::GetStatus, this);
    }
    else
    {
        ret = m_current_status;
    }

    return ret;
}

#endif
