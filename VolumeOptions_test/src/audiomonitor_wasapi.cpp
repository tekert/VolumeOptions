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

#include "../volumeoptions/config.h"
#include "../volumeoptions/audiomonitor_wasapi.h"

// NOTE: Dont change these unless neccesary.
#include <initguid.h> // for macro DEFINE_GUID definition http://support2.microsoft.com/kb/130869/en-us
/* zero init global GUID for events, for later on AudioMonitor constructor */
DEFINE_GUID(GUID_VO_CONTEXT_EVENT_ZERO, 0x00000000L, 0x0000, 0x0000,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
// {D2C1BB1F-47D8-48BF-AC69-7E4E7B2DB6BF}  For event distinction
static const GUID GUID_VO_CONTEXT_EVENT =
{ 0xd2c1bb1f, 0x47d8, 0x48bf, { 0xac, 0x69, 0x7e, 0x4e, 0x7b, 0x2d, 0xb6, 0xbf } };


namespace vo {

// NOTE TODO: these two will be deleted when my IPC module is complete.
std::set<std::wstring> AudioMonitor::m_current_monitored_deviceids;
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
    // NOT USED, sessions wont expire: 
    // see http://msdn.microsoft.com/en-us/library/dd368281%28v=vs.85%29.aspx remarks last paragraph
    //  either way, we manualy release sessions inactive for more than 2 min by defualt to compensate.
    static void DeleteSession(std::shared_ptr<AudioMonitor> pam, std::shared_ptr<AudioSession> spAudioSession)
    {
        pam->DeleteSession(spAudioSession);
    }

    static void state_changed_callback_handler(std::shared_ptr<AudioSession> pas, AudioSessionState newstatus)
    {
        return pas->state_changed_callback_handler(newstatus);
    }
    static void UpdateDefaultVolume(std::shared_ptr<AudioSession> pas, float new_def)
    {
        pas->UpdateDefaultVolume(new_def);
    }
    static void set_state(std::shared_ptr<AudioSession> pas, AudioSessionState state)
    {
        pas->set_state(state);
    }

    /* Classes with access */
    friend class CAudioSessionEvents; /* needed for callbacks to access this class using async calls */
    friend class CSessionNotifications; /* needed for callbacks to access this class using async calls */
};

/////////////////////////////////////////////////////////////////////////////////////////////////////
///////////// Some exported helpers for async calls from other proyects of mine /////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////

// Used to sync calls from other threads usign io_service. Generic templates
namespace
{
    // Helpers to make async calls, sync.
    template <class R>
    void ret_sync_queue(R* ret, bool* done, std::condition_variable* e, std::mutex* m,
        std::function<R(void)> f)
    {
        *ret = f();
        std::lock_guard<std::mutex> l(*m);
        *done = true;
        e->notify_all();
    }

    void sync_queue(bool* done, std::condition_variable* e, std::mutex* m, std::function<void(void)> f)
    {
        f();
        std::lock_guard<std::mutex> l(*m);
        *done = true;
        e->notify_all();
    }


    /* If compiler supports variadic templates use this, its nicer */
    /* perfect forwarding,  rvalue references */

    // Simple ASIO proxy async call.
    template <typename ft, typename... pt>
    void ASYNC_CALL(const std::shared_ptr<boost::asio::io_service>& io, ft&& f, pt&&... args)
    {
        io->post(std::bind(std::forward<ft>(f), std::forward<pt>(args)...));
    }

    // Does ASIO async call and waits it to complete.
    template <typename ft, typename... pt>
    void SYNC_CALL(const std::shared_ptr<boost::asio::io_service>& io, std::condition_variable& cond,
        std::mutex& io_mutex, ft&& f, pt&&... args)
    {
        bool done = false;
        io->dispatch(std::bind(&sync_queue, &done, &cond, &io_mutex,
            std::function<void(void)>(std::bind(std::forward<ft>(f), std::forward<pt>(args)...))));

        std::unique_lock<std::mutex> l(io_mutex);
        while (!done) { cond.wait(l); };
        l.unlock();
    }

    // Does ASIO async call and waits for return.
    template <typename rt, typename ft, typename... pt>
    rt SYNC_CALL_RET(const std::shared_ptr<boost::asio::io_service>& io, std::condition_variable& cond,
        std::mutex& io_mutex, ft&& f, pt&&... args)
    {
        bool done = false;
        rt r;
        io->dispatch(std::bind(&ret_sync_queue<rt>, &r, &done, &cond, &io_mutex,
            std::function<rt(void)>(std::bind(std::forward<ft>(f), std::forward<pt>(args)...))));

        std::unique_lock<std::mutex> l(io_mutex);
        while (!done) { cond.wait(l); };
        l.unlock();
        return r;
    }
    
    // Async callbacks with delays
    template <class rep, class period, typename ft, typename... pt>
    std::unique_ptr<boost::asio::steady_timer>
        ASYNC_CALL_DELAY(std::shared_ptr<boost::asio::io_service>& io,
            const std::chrono::duration<rep, period>& tdelay, ft&& f, pt&&... args)
    {
        std::unique_ptr<boost::asio::steady_timer> delay_timer =
            std::make_unique<boost::asio::steady_timer>(*io, tdelay);
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

    We use async call here, i decided to use asio because.. well i've been using it for years.
    I could implement a thread safe queue for async calls but i already had this tested and done

    MSDN:
    1 The methods in the interface must be nonblocking. The client should never wait on a synchronization
        object during an event callback.
    2 The client should never call the IAudioSessionControl::UnregisterAudioSessionNotification method during
        an event callback.
    3 The client should never release the final reference on a WASAPI object during an event callback.

    NOTES:
    *1 To be non blocking(for long) and thread safe using our AudioMonitor class the only safe easy way is to
            use async calls.
    *2 Easy enough. (we also lock AudioSession ptr at first to be extra safe it wont be unregistered on other thread)
    *3 See AudioSession class destructor for more info about that.
*/
#define TEST_NO_SHAREDPTR 0     // I need it so i can create events in AudioSession constructor, testing.
class CAudioSessionEvents : public IAudioSessionEvents
{
    LONG _cRef;
#if !TEST_NO_SHAREDPTR
    std::weak_ptr<AudioSession> m_pAudioSession;
#else
    AudioSession* m_pAudioSession = NULL;
#endif
    std::weak_ptr<AudioMonitor> m_pAudioMonitor;

protected:

    ~CAudioSessionEvents()
    {}
    
    CAudioSessionEvents()
        : _cRef(1)
    {}

public:
#if TEST_NO_SHAREDPTR
    CAudioSessionEvents(AudioSession* pAudioSession,
#else
    CAudioSessionEvents(const std::weak_ptr<AudioSession>& pAudioSession,
#endif
        const std::weak_ptr<AudioMonitor>& pAudioMonitor)
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
        dprintf("CALLBACK: OnDisplayNameChanged\n");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnIconPathChanged(
        LPCWSTR NewIconPath,
        LPCGUID EventContext)
    {
        dprintf("CALLBACK: OnIconPathChanged\n");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(
        float NewVolume,
        BOOL NewMute,
        LPCGUID EventContext)
    {
        dprintf("CALLBACK: OnSimpleVolumeChanged ");

        std::shared_ptr<AudioSession> spAudioSession;

        // If we didnt generate this event
        if (!IsEqualGUID(*EventContext, GUID_VO_CONTEXT_EVENT))
        {
#if !TEST_NO_SHAREDPTR
            spAudioSession = m_pAudioSession.lock();
#else
            if (!m_pAudioSession) return S_OK;
            try { spAudioSession = m_pAudioSession->shared_from_this(); }
            catch (std::bad_weak_ptr&) {}
#endif
            if (!spAudioSession)
            {
                dprintf("spAudioSession == NULL!\n");
                return S_OK;
            }

            std::shared_ptr<AudioMonitor> spAudioMonitor(m_pAudioMonitor.lock());
            if (!spAudioMonitor)
                return S_OK;

            dprintf("External change, updating user default volume... ");
            ASYNC_CALL(spAudioMonitor->get_io(), &AudioCallbackProxy::UpdateDefaultVolume, spAudioSession, NewVolume);
        }

#ifdef _DEBUG
        if (!spAudioSession)
        {
#if !TEST_NO_SHAREDPTR
            spAudioSession = m_pAudioSession.lock();
#else
            if (!m_pAudioSession) return S_OK;
            try { spAudioSession = m_pAudioSession->shared_from_this(); }
            catch (std::bad_weak_ptr&) {}
#endif
        }
        DWORD pid = 0;
        if (spAudioSession)
            pid = spAudioSession->getPID();
        if (NewMute)
        {
            dprintf("MUTE [%d]\n", pid);
        }
        else
        {
            dprintf("Volume = %d%% [%d]\n", (UINT32)(100 * NewVolume + 0.5), pid);
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
        dprintf("CALLBACK: OnChannelVolumeChanged\n");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(
        LPCGUID NewGroupingParam,
        LPCGUID EventContext)
    {
        dprintf("CALLBACK: OnGroupingParamChanged\n");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnStateChanged(
        AudioSessionState NewState)
    {
        dprintf("CALLBACK OnStateChanged: ");

#if TEST_NO_SHAREDPTR
        if (!m_pAudioSession) return S_OK;
        std::shared_ptr<AudioSession> spAudioSession;
        try { spAudioSession = m_pAudioSession->shared_from_this(); }
        catch (std::bad_weak_ptr&) {}
#else
        std::shared_ptr<AudioSession> spAudioSession(m_pAudioSession.lock());
#endif
        if (!spAudioSession)
        {
            dprintf("spAudioSession == NULL!\n");
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
            ASYNC_CALL(spAudioMonitor->get_io(), &AudioCallbackProxy::state_changed_callback_handler, spAudioSession,
                NewState);
            break;
        case AudioSessionStateInactive:
            pszState = "inactive";
            ASYNC_CALL(spAudioMonitor->get_io(), &AudioCallbackProxy::state_changed_callback_handler, spAudioSession,
                NewState);
            break;
        case AudioSessionStateExpired:
            // NOTE: Only pops if we dont retaing a reference to the session, so we wont, 
            //  see AudioCallbackProxy::DeleteSession comments
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
        dprintf("CALLBACK OnSessionDisconnected: ");

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

#if TEST_NO_SHAREDPTR
    CAudioSessionEvents_fix(AudioSession* pAudioSession,
#else
    CAudioSessionEvents_fix(const std::weak_ptr<AudioSession>& pAudioSession,
#endif
            const std::weak_ptr<AudioMonitor>& pAudioMonitor) 
        : CAudioSessionEvents(pAudioSession, pAudioMonitor)
    {}

public:

    CAudioSessionEvents_fix() 
        : CAudioSessionEvents()
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

    CSessionNotifications(const std::weak_ptr<AudioMonitor>& pAudioMonitor)
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
            // IMPORTANT: Fix, dont know why, but the first callback never arrives if we dont do this now.
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

    /////////////////////////////////////////////////////////////////////////////////////////////
    //////////////////////////////////////  Audio Session  //////////////////////////////////////
    /////////////////////////////////////////////////////////////////////////////////////////////

AudioSession::AudioSession(IAudioSessionControl *pSessionControl, const std::weak_ptr<AudioMonitor>& wpAudioMonitor,
    float default_volume)
    : m_default_volume(default_volume)
    , m_wpAudioMonitor(wpAudioMonitor)
    , m_hrStatus(S_OK)
    , m_pSessionControl(pSessionControl)
    , m_pAudioEvents(NULL)
    , m_pSimpleAudioVolume(NULL)
    , m_pSessionControl2(NULL)
    , m_is_volume_at_default(true)
    , m_excluded_flag(false)
    , m_session_dead(false)
    , m_last_modified_on(std::chrono::steady_clock::time_point::max())
{
    if (pSessionControl == NULL)
    {
        m_hrStatus = E_POINTER;
        printf("[ERROR] AudioSession::AudioSession pSessionControl == NULL\n");
        return;
    }

    dprintf("AudioSession::AudioSession default_volume correction=%f\n", default_volume);

    // Get const data first
    IAudioSessionControl2* pSessionControl2 = NULL;
    CHECK_HR(m_hrStatus = m_pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2),
        (void**)&pSessionControl2));
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

    assert(m_pSessionControl);
    // NOTE: retaining a copy to a WASAPI session interface IAudioSessionControl  causes the session to never expire.
    m_pSessionControl->AddRef();
#if 0
    CHECK_HR(m_hrStatus = m_pSessionControl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&m_pSimpleAudioVolume));
    assert(m_pSimpleAudioVolume);

    CHECK_HR(m_hrStatus = m_pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&m_pSessionControl2));
    assert(m_pSimpleAudioVolume);
#endif

    // Correct volume
    if (m_default_volume > 1.0f)
        m_default_volume = 1.0f;

    // if user default vol not set (negative) set it.
    float currrent_vol = GetCurrentVolume();
    if (m_default_volume < 0.0f)
        UpdateDefaultVolume(currrent_vol);

    // We dont know yet the state, we will get it after enabling events when contructor
    //  finishes, so event callbacks are queued correctly with a valid shared_ptr.
    //  set default as active, this also initializes m_last_active_state.
    set_state(AudioSessionState::AudioSessionStateActive);

    /* Fix for Sndvol, if a new session is detected when the process was just
    opened, wasapi volume change won work, no way around it. wait for a bit */
    Sleep(20);

    // Windows SndVol fix.
    /* 
    'default_volume' has the volume of another recently changed same SID volume, negative if not. 
        Sessions with the same SID take their default volume from the last SID changed >5sec ago from the registry, 
        *see AudioSession::ApplyVolumeSettings and AudioMonitor::SaveSession for more doc.
    */
    if (default_volume >= 0.0f)
    {
        if (m_is_volume_at_default && (m_default_volume != currrent_vol))
        {
            dprintf("AudioSession::AudioSession Windows SndVol fix (%f != %f) , is_volume_at_default =%d\n",
                m_default_volume, currrent_vol, m_is_volume_at_default);
            ChangeVolume(m_default_volume);	// fix correct windows volume before aplying settings
        }
    }

#ifdef _DEBUG
    AudioSessionState State;
    CHECK_HR(m_hrStatus = pSessionControl->GetState(&State));

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
    SAFE_RELEASE(pSessionControl2);

    // Check class status after creation. AudioSession::GetStatus() HRESULT type, if failed discart this session.
    // NOTE: i dont like exceptions on windows api code.
    ;
}

AudioSession::~AudioSession()
{
    if (!m_session_dead)
        dwprintf(L"~AudioSession:: PID[%d]Deleting Session %s\n", getPID(), getSIID().c_str());

    ShutdownSession();
}

/*
    This will render the object basically dead for wasapi without destroying it.

    Necessary to release all wasapi references before enumerating new ones, (wasapi rules...)
        if we dont release references before enumerating it leaks memory,
        more info on AudioMonitor::RefreshSessions().

    The reason i had to create this is that i cant destroy the object and release wasapi references on destructor
        when there are pending queued callbacks with this session shared_ptr references on io_service.
        for example: there could be queued volume restore cancelled callbacks with a session shared_ptr
        or queued events callbacks with a session shared_ptr just before session shared_ptr delete.
*/
void AudioSession::ShutdownSession()
{
    if (m_session_dead)
        return;

    // First, before releasing, unregister events.
    StopEvents();

    // Set Session volume level to default state before releasing.
    RestoreVolume(resume_t::NO_DELAY);

    if (m_pSessionControl) assert(CHECK_REFS(m_pSessionControl) == 1);
    // if (m_pSimpleAudioVolume) assert(CHECK_REFS(m_pSimpleAudioVolume) == 1);
    // if (m_pSessionControl2) assert(CHECK_REFS(m_pSessionControl2) == 1);

    //SAFE_RELEASE(m_pSessionControl2);
    //SAFE_RELEASE(m_pSimpleAudioVolume);
    SAFE_RELEASE(m_pSessionControl);

    m_session_dead = true;
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
        assert(m_pSessionControl);
        if (!m_pSessionControl) return;

        // CAudioSessionEvents constructor sets Refs on 1 so remember to release
        m_pAudioEvents = new CAudioSessionEvents(this->shared_from_this(), m_wpAudioMonitor);

        // RegisterAudioSessionNotification calls another AddRef on m_pAudioEvents so Refs = 2 by now
        CHECK_HR(m_hrStatus = m_pSessionControl->RegisterAudioSessionNotification(m_pAudioEvents));
        dprintf("AudioSession::InitEvents() PID[%d] Init Session Events\n", getPID());

        // After enabling events, set initial state
        //  we dont know since when it was active/inactive (update after init events).
        AudioSessionState State = AudioSessionStateInactive;
        CHECK_HR(m_hrStatus = m_pSessionControl->GetState(&State));
        set_state(State);
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

float AudioSession::GetCurrentVolume() const
{
    float current_volume = -1.0f;

    assert(m_pSessionControl);
    if (!m_pSessionControl) return current_volume;

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
    Simple handler for new sessesion state events. 
    (called from wasapi registered session events callback class)
*/
void AudioSession::state_changed_callback_handler(AudioSessionState newstatus)
{
    set_state(newstatus); // set this before calling apply settings

    switch (newstatus)
    {
    case AudioSessionState::AudioSessionStateActive:
        ApplyVolumeSettings();
        break;

    case AudioSessionState::AudioSessionStateInactive:
        RestoreVolume();
        break;
    }
}

/*
    Changes Session Volume based on current saved session settings.

    It gets settings from his AudioMonitor to decide if change vol or not.

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
    if (!spAudioMonitor) return S_OK; // AudioMonitor is currently shuting down, abort.

    // shortcut reference to monitor globals settings.
    const session_settings& ses_setting = spAudioMonitor->m_settings.ses_global_settings;
    const float &current_vol_reduction = ses_setting.vol_reduction;

    bool change_vol = true;
    // Only change volume if the session is active and configured to do so
    if (ses_setting.change_only_active_sessions)
    {
        if (m_current_state == AudioSessionState::AudioSessionStateActive)
            change_vol = true;
        else
            change_vol = false;
    }

    if (m_excluded_flag)
        change_vol = false;

    // if AudioSession::is_volume_at_default is true, the session is at user default volume.
    // if AudioMonitor::m_auto_change_volume_flag is true, auto volume reduction is activated, else disabled.
    if (spAudioMonitor->m_auto_change_volume_flag && change_vol)
    {
        float set_vol;
        if (ses_setting.treat_vol_as_percentage)
        {
            assert((current_vol_reduction >= -1.0f) && (current_vol_reduction <= 1.0f));
            // if negative, will actually increase volume! (limit -1.0f to 1.0f)
            if (current_vol_reduction >= 0.0f)
                set_vol = m_default_volume * (1.0f - current_vol_reduction); // %

            if (set_vol > 1.0f) set_vol = 1.0f;
        }
        else
        {
            assert((current_vol_reduction >= 0.0f) && (current_vol_reduction <= 1.0f));
            set_vol = 1.0f - current_vol_reduction; // fixed (limit 0.0f to 1.0f)
        }

        // Volume will be changed because of settings, pending delayed vol restore has no purpose.
        spAudioMonitor->m_pending_restores.erase(this);

        ChangeVolume(set_vol);
        m_is_volume_at_default = false; // mark that the session is NOT at default state.

        dprintf("AudioSession::ApplyVolumeSettings() PID[%d] Changed Volume to %.2f\n",
            getPID(), set_vol);
    }
    else
    {
        dprintf("AudioSession::ApplyVolumeSettings() PID[%d] skiped, flag=%d global_vol_reduction = %.2f, "
            "is_volume_at_default = %d, reduce_vol=%d \n", getPID(), spAudioMonitor->m_auto_change_volume_flag,
            current_vol_reduction, m_is_volume_at_default, change_vol);
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
void AudioSession::UpdateDefaultVolume(const float new_def)
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
        printf("[ERROR] AudioSession::RestoreHolderCallback PID[%d]  Asio: %s\n", getPID(), e.message().c_str());
        return;
    }

    // NOTE: an asio timer can be canceled but, if it was already expired, it will call its handler
    //  as normal (non aborted), so we have to manually check if we canceled it or not.
    //  asio::*_timer::cancel() will return if it was canceled or already queued for execution.
    //  but we directly handle that by checking if we deleted the timer wanting to cancel its handler.
    std::shared_ptr<AudioMonitor> spAudioMonitor(m_wpAudioMonitor.lock());
    if (spAudioMonitor)
    {
        AudioMonitor::t_pending_restores::iterator it = spAudioMonitor->m_pending_restores.find(this);

        // if this timer was deleted but it got queued beforehand, stop execution.
        if (it == spAudioMonitor->m_pending_restores.end())
            return;

        // Timer completed, we can discard it.
        spAudioMonitor->m_pending_restores.erase(it);

    } // else AudioMonitor is currently shuting down.

    dprintf("AudioSession::RestoreHolderCallback  PID[%d] Wait Complete Restoring Volume...\n", getPID());

    // Important: Send NO_DELAY always from here so we break the loop.
    RestoreVolume(resume_t::NO_DELAY);

    // ...let asio stored AudioSession shared_ptr destroy its count now.
}

/*
    Restores Default session volume

    We saved the default volume on the session so we can restore it here
        m_default_volume always have the user preferred session default volume.
    If 'is_volume_at_default' flag is true it means the session is already at default level.
    If is false it means we have to restore it.

    callback_type  -> optional parameter (default NORMAL) to indicate if we should
        create a callback timer (if configured to do so) or change vol without delay (NO_DELAY).
*/
HRESULT AudioSession::RestoreVolume(resume_t callback_type)
{
    HRESULT hr = S_OK;

    if (!m_is_volume_at_default)
    {
        if (callback_type == resume_t::NORMAL)
        {
            // callback_type should be no_delay when chain called from AudioMonitor destructor...
            std::shared_ptr<AudioMonitor> spAudioMonitor(m_wpAudioMonitor.lock());
            if (!spAudioMonitor) callback_type = resume_t::NO_DELAY; // AudioMonitor is currently shuting down.

            std::shared_ptr<AudioSession> spAudioSession;
            // play it safe, callback_type should be no_delay when called from AudioSession destructor...
            try { spAudioSession = this->shared_from_this(); }
            catch (std::bad_weak_ptr&) { callback_type = resume_t::NO_DELAY; }

            // if delays are configured create asio timers to "self" call with callback_no_delay = true
            //		timers are also stored on AudioMonitor to cancel them when necessary.
            if ((callback_type == resume_t::NORMAL) &&
                spAudioMonitor->m_settings.ses_global_settings.vol_up_delay != std::chrono::milliseconds::zero())
            {
                // play it safe, skip this when called from AudioSession destructor...
                if (!spAudioSession) return hr;
#ifdef _DEBUG
                if (spAudioMonitor->m_pending_restores.find(this) != spAudioMonitor->m_pending_restores.end())
                    dprintf("AudioSession::RestoreVolume PID[%d] A pending restore timer is waiting... "
                    "stopping old timer and replacing it... \n", getPID());
#endif
                // Create an async callback timer :)
                // IMPORTANT: Delete timer from container when :
                //		1. Callback is completed.
                //		2. We change session volume.
                //		3. A session is removed from container. (AudioMonitor)
                //          to free AudioSession destructor sooner.
                // NOTE: dont cancel() timers, delete or replace them.
                // IMPORTANT: Use a shared_ptr per async call so we have something persistent to async.
                spAudioMonitor->m_pending_restores[this] =
                    ASYNC_CALL_DELAY(spAudioMonitor->m_io, spAudioMonitor->m_settings.ses_global_settings.vol_up_delay,
                    &AudioSession::RestoreHolderCallback, spAudioSession, std::placeholders::_1);

                dprintf("AudioSession::RestoreVolume PID[%d] Created and saved delayed callback\n", getPID());

                return hr;
            }
        }

        std::shared_ptr<AudioMonitor> spAudioMonitor(m_wpAudioMonitor.lock());
        if (spAudioMonitor)
        {   // Restore vol Timer is no longer needed, caducated.
            spAudioMonitor->m_pending_restores.erase(this);
        } // else  AudioMonitor is currently shuting down, m_pending_restores will be deleted.

        // Now... restore
        ChangeVolume(m_default_volume);

        dprintf("AudioSession::RestoreVolume PID[%d] Restoring Volume of Session to %.2f\n", getPID(), m_default_volume);

        m_is_volume_at_default = true; // to signal the session is at default state.
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
    if (!m_pSessionControl) return;

    ISimpleAudioVolume* pSimpleAudioVolume = NULL;
    CHECK_HR(m_hrStatus = m_pSessionControl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&pSimpleAudioVolume));
    assert(pSimpleAudioVolume);

    CHECK_HR(m_hrStatus = pSimpleAudioVolume->SetMasterVolume(v, &GUID_VO_CONTEXT_EVENT));
    touch();

    dprintf("AudioSession::ChangeVolume PID[%d] new volume level = %.2f\n", getPID(), v);

done:
    SAFE_RELEASE(pSimpleAudioVolume);
}

void AudioSession::touch()
{
    m_last_modified_on = std::chrono::steady_clock::now();
}

void AudioSession::set_state(AudioSessionState state)
{
    switch (state)
    {
    case AudioSessionState::AudioSessionStateActive:
        dprintf("AudioSession::set_state PID[%d] m_last_active_state to max()\n", getPID());
        m_last_active_state = std::chrono::steady_clock::time_point::max();
        m_current_state = AudioSessionState::AudioSessionStateActive;
        break;

    case AudioSessionState::AudioSessionStateInactive:
    case AudioSessionState::AudioSessionStateExpired:
        dprintf("AudioSession::set_state PID[%d]  m_last_active_state to now()\n", getPID());
        m_last_active_state = std::chrono::steady_clock::now();
        m_current_state = AudioSessionState::AudioSessionStateInactive;
        break;
    }
}


    /////////////////////////////////////////////////////////////////////////////////////////////
    /////////////////////////////////////  AudioMonitor   ///////////////////////////////////////
    /////////////////////////////////////////////////////////////////////////////////////////////

AudioMonitor::AudioMonitor(const std::wstring& device_id)
    : m_current_status(monitor_status_t::INITERROR)
    , m_error_status(monitor_error_t::OK)
    , m_auto_change_volume_flag(false)
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

    hr = InitDeviceID(device_id); // if empty will use default endpoint.
    if (FAILED(hr))
        return;

    m_processid = GetCurrentProcessId();

    if (m_error_status != monitor_error_t::DEVICEID_IN_USE)
    {
        StartIOInit();
    }
}

void AudioMonitor::StartIOInit()
{
    // Start thread after pre init is complete.
    // m_current_status flag will be set to ok when thread init is complete.
    m_io.reset(new boost::asio::io_service);
    m_thread_monitor = std::thread(&AudioMonitor::poll, this);

    // When io_service is running, m_current_status flag will be set and finish init.
    ASYNC_CALL(m_io, &AudioMonitor::FinishIOInit, this);
}

void AudioMonitor::FinishIOInit()
{
    m_current_status = AudioMonitor::monitor_status_t::STOPPED;

    dprintf("\t--AudioMonitor init complete--\n");
}

AudioMonitor::~AudioMonitor()
{
    if (m_io.use_count())
    {
        // Exit MonitorThread
        m_abort = true;
        m_io->stop();
        // NOTE: define BOOST_ASIO_DISABLE_IOCP on windows if you want stop() to return as soon as it finishes
        //  executing current handler or if has no pending handlers to run, if not defined on windows
        //  it will wait until it has no pending handlers to run (empty queue only).

        if (m_thread_monitor.joinable())
            m_thread_monitor.join(); // wait to finish
    }
    {
        std::lock_guard<std::mutex> l(m_static_set_access);
        m_current_monitored_deviceids.erase(m_wsDeviceID);
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
    But it garantees it wont be destroyed when using it.
*/
std::shared_ptr<boost::asio::io_service> AudioMonitor::get_io() const
{
    return m_io;
}

/*
    Poll for new calls on current thread

    When using wasapi events, this is a safe way to handle multiple OS callbacks and
        user/plugin threads by handling all calls here sequentially.
    We have 3 methods other threads can use to sync with this thread: async, sync and sync with return.
    For security we also lock a class mutex so no other thread can directly access the class
        while polling, if mutex if locked, threads will sync with this thread.
*/
void AudioMonitor::poll()
{
    // No other friendly class thread can use this class while pooling
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

    // Syncronizes all method calls with AudioMonitor main thread.
    bool stop_loop = false;
    while (!stop_loop)
    {
        boost::system::error_code ec;
        m_io->run(ec);
        if (ec)
        {
            std::cerr << "[ERROR] Asio msg: " << ec.message() << std::endl;
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

    return m_current_monitored_deviceids;
}

/*
    Handy Method to enumerates all audio endpoints devices.

    This method can be used to obtain ids of audio endpoint and use them on AudioMonitor constructor.
    Returns a map of wstrings DeviceIDs -> FriendlyName. Use desired wstring DeviceID on AudioMonitor creation.
    http://msdn.microsoft.com/en-us/library/windows/desktop/dd370837%28v=vs.85%29.aspx (deviceIds)

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

HRESULT AudioMonitor::InitDeviceID(const std::wstring& _device_id)
{
    HRESULT hr = S_OK;

    std::wstring device_id(_device_id);
    hr = GetSessionManager(device_id); // if empty, will be set with default endpoint if no errors occurs.
    if (FAILED(hr))
    {
        if (hr == E_NOTFOUND)
        {
            std::cerr << "ERROR InitDeviceID() The device ID does not identify an audio device"
                "that is in this system. " << std::endl;
            m_error_status = monitor_error_t::DEVICE_NOT_FOUND;
        }
        std::wcerr << "ERROR InitDeviceID(" << _device_id << ")  HRESULT : " << hr << std::endl;
    }
    else
    {
        std::lock_guard<std::mutex> l(m_static_set_access);
        // if we are already monitoring this endpoint, abort
        if (m_current_monitored_deviceids.count(device_id))
        {
            // todo heavy: check if other process or instance is using it and continue, dont return, then sync.
            // TODO UPDATE: working on IPC module.
            std::cerr << "AudioMonitor::InitDeviceID() ERROR: Already monitoring this DeviceID endpoint" << std::endl;
            m_error_status = monitor_error_t::DEVICEID_IN_USE;
        }
        else
        {
            // static set so we dont monitor the same endpoints on multiple instances of AudioMonitor.
            m_current_monitored_deviceids.insert(device_id);

            m_wsDeviceID = device_id;
            dprintf("AudioMonitor::InitDeviceID()  Using Device Status: OK.\n");
        }
    }

    return hr;
}

/*
    Creates the IAudioSessionManager instance on default output device if device_id is empty
*/
HRESULT AudioMonitor::GetSessionManager(std::wstring& device_id)
{
    HRESULT hr = S_OK;

    IMMDevice* pDevice = NULL;
    IMMDeviceEnumerator* pEnumerator = NULL;
    LPWSTR pwszID = NULL;
    bool uninitialize_com = true;

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

    dwprintf(L"AudioMonitor CreateSessionManager() Getting Manager2 instance from DeviceID: %s...\n",
        device_id.c_str());

    // Create the device enumerator.
    CHECK_HR(hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        (void**)&pEnumerator));
    assert(pEnumerator != NULL);

    // If user specified and endpoint ID to monitor, use it, if not use default.
    if (device_id.empty())
    {
        // Get the default audio device.
        CHECK_HR(hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice));
        assert(pDevice != NULL);

        CHECK_HR(hr = pDevice->GetId(&pwszID));
        device_id = pwszID;
        CoTaskMemFree(pwszID);
        pwszID = NULL;
    }
    else
    {
        // Get user specified device using its device id.
        CHECK_HR(hr = pEnumerator->GetDevice(device_id.c_str(), &pDevice));
    }

    // Get the session manager. (this will fail on vista and below)
    CHECK_HR(hr = pDevice->Activate(
        __uuidof(IAudioSessionManager2), CLSCTX_ALL,
        NULL, (void**)&m_pSessionManager2));
    assert(m_pSessionManager2);

   // Disable ducking experience and later restore it if it was enabled
   // NOTE: i dont know how to get current status to restore it later, better dont touch it then,
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
    Deletes all sessions references and asks wasapi enumerator for current sessions.

    Uses windows7+ enumerator to list all SndVol sessions
        then saves each session found.

    NOTE: this is requiered for NewSessionNotifications to start working from a stop.
         more notes inside.
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
    Releases, restores and deletes all saved sessions.
*/
void AudioMonitor::DeleteSessions()
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    // First delete all pending timers on audio sessions.
    // (this will cancel pending timers, 
    //  not pending timers will be executed but will be manually cancelled)
    m_pending_restores.clear();

    // Use shutdown first to delete all wasapi internal references
    // (it will leave the session at default state),
    // more info on AudioSession::ShutdownSession()
    for (auto s : m_saved_sessions)
    {
        s.second->ShutdownSession();
    }
    // then delete map to erase sesion shared_ptr references.
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
    if (e == boost::asio::error::operation_aborted) 
        return;

    if (e)
    {
        printf("ASIO ERROR DeleteExpiredSessions Timer: %s\n", e.message().c_str());
    }

    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    timer->expires_from_now(m_delete_expired_interval);
    timer->async_wait(std::bind(&AudioMonitor::DeleteExpiredSessions,
        this, std::placeholders::_1, timer));

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

            it->second->ShutdownSession();
            it = m_saved_sessions.erase(it);
        }
        else
            it++;
    }

    dwprintf(L". DeleteExpired tick\n");
}

/*
    Applies current parsed saved settings on all class elements.
*/
void AudioMonitor::ApplyMonitorSettings()
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    // Search current saved sessions and set exclude flag based on monitor settings.
    for (auto it = m_saved_sessions.begin(); it != m_saved_sessions.end(); ++it)
    {
        bool excluded = isSessionExcluded(it->second->getPID(), it->second->getSID());
        if (excluded)
        {
            dwprintf(L"\nExluding PID[%d] due to new config...\n", it->second->getPID());
            it->second->m_excluded_flag = true;
            it->second->RestoreVolume(AudioSession::resume_t::NO_DELAY);
        }
        else
            it->second->m_excluded_flag = false;
    }

    if (m_auto_change_volume_flag)
    {
        // Update all session's volume
        for (auto it = m_saved_sessions.begin(); it != m_saved_sessions.end(); ++it)
        {
            it->second->ApplyVolumeSettings();
        }
    }
}

/*
    Return true if audio session is excluded from monitoring.
*/
bool AudioMonitor::isSessionExcluded(const DWORD pid, std::wstring sid)
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
        grouped togheter on the same multimap key (SID).
    See more notes inside.
    Before adding the session it detects whanever this session is another instance of the same process (same SID)
        and copies the default volume of the last changed session. We need to do this because if we dont, it will
        take the changed(non user default) volume of the other SID session as default..
 
    IAudioSessionControl* pSessionControl  -> pointer to the new session to add.
    bool unref  -> do we Release() pSessionControl inside this function or not? useful for async calls
*/
HRESULT AudioMonitor::SaveSession(IAudioSessionControl* pSessionControl, const bool unref)
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

    bool is_excluded = isSessionExcluded(pid, ws_sid);

    if ((pSessionControl2->IsSystemSoundsSession() == S_FALSE))
    {
        dwprintf(L"\n---Saving New Session: PID[%d]:\n", pid);

        LPWSTR _siid = NULL;
        CHECK_HR(hr = pSessionControl2->GetSessionInstanceIdentifier(&_siid)); // This one is unique
        ws_siid =_siid;
        CoTaskMemFree(_siid);

        bool duplicate = false;

        //  If a SID already exists, copy the default volume of the last changed session of the same SID.
        //  As SndVol does: always copies the sound volume of the last changed session, try opening the same music
        //      player 3 times, before the 3rd change volume to the second instance and then to the first, wait >5sec 
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

            // if created succesfuly, finish updating status and save it.
            if (!FAILED(pAudioSession->GetStatus()))
            {
                if (is_excluded)
                    pAudioSession->m_excluded_flag = true;

#ifdef VO_ENABLE_EVENTS
                // Enable events after constructor finishes so callbacks are queued in io_service.
                pAudioSession->InitEvents();
#endif
                // Update new session's volume to sync with current settings
                pAudioSession->ApplyVolumeSettings();

                // Save session
                m_saved_sessions.insert(t_session_pair(ws_sid, pAudioSession));
            }
            else
                wprintf(L"---AudioMonitor::SaveSession PID[%d] ERROR opening session\n", pid);
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
    Releases, restores and deletes a single session from container
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
                //      each timer contains a shared_ptr to AudioSession.
                m_pending_restores.erase(spAudioSession.get());

                it->second->ShutdownSession();
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
        ret = SYNC_CALL_RET<long>(m_io, m_cond, m_io_mutex, &AudioMonitor::Stop, this);
    }
    else
    {
        if (m_current_status == monitor_status_t::INITERROR)
            return -1;

        if (m_current_status == monitor_status_t::STOPPED)
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

        // Deletes all sessions 
        // This will trigger AudioSession destructors, restoring volume.
        DeleteSessions();

        dwprintf(L"\n\t ---- AudioMonitor::Stop() STOPPED .... \n\n");
        m_current_status = monitor_status_t::STOPPED;
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
        ret = SYNC_CALL_RET<long>(m_io, m_cond, m_io_mutex, &AudioMonitor::InitEvents, this);
    }
    else
    {
        if (m_current_status == monitor_status_t::INITERROR)
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
        ret = SYNC_CALL_RET<long>(m_io, m_cond, m_io_mutex, &AudioMonitor::StopEvents, this);
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
    return static_cast<long>(ret);
#else
    //if (std::this_thread::get_id() != m_thread_monitor.get_id())
    std::unique_lock<std::recursive_mutex> l(m_mutex, std::try_to_lock);

    if (!l.owns_lock())
    {
        ret = SYNC_CALL_RET<long>(m_io, m_cond, m_io_mutex, &AudioMonitor::Pause, this);
    }
    else
    {
        if (m_current_status == monitor_status_t::INITERROR)
            return -1;

        if (m_current_status == monitor_status_t::STOPPED)
        {
            dwprintf(L"\n\t ---- AudioMonitor::Pause() Monitor already Stopped .... \n");
            return 0;
        }
        if (m_current_status == monitor_status_t::PAUSED)
        {
            dwprintf(L"\n\t ---- AudioMonitor::Pause() Monitor already Paused .... \n");
            return 0;
        }

        // Global class flag , volume reduction inactive
        m_auto_change_volume_flag = false;

        // Restore Volume of all sessions currently monitored
        for (auto it = m_saved_sessions.begin(); it != m_saved_sessions.end(); ++it)
        {
            it->second->RestoreVolume();
        }

        dwprintf(L"\n\t ---- AudioMonitor::Pause  PAUSED .... \n\n");
        m_current_status = monitor_status_t::PAUSED;
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
        ret = SYNC_CALL_RET<long>(m_io, m_cond, m_io_mutex, &AudioMonitor::Start, this);
    }
    else
    {
        if (m_current_status == monitor_status_t::INITERROR)
            return -1;

        if (m_current_status == monitor_status_t::RUNNING)
        {
            dwprintf(L"\n\t ---- AudioMonitor::Start() Monitor already Running .... \n");
            return 0;
        }

        if (m_current_status == monitor_status_t::STOPPED)
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

        if (m_current_status == monitor_status_t::PAUSED)
        {
            dwprintf(L"\n\t .... AudioMonitor::Start() RESUMED ----\n\n");
        }

        // Signal reduce volume flag and apply volume change settings.
        m_auto_change_volume_flag = true;

        // Update all session's volume based on current settings
        for (auto it = m_saved_sessions.begin(); it != m_saved_sessions.end(); ++it)
        {
            it->second->ApplyVolumeSettings();
        }

        m_current_status = monitor_status_t::RUNNING;
    }

    return static_cast<long>(ret); // TODO: error codes
}

#ifdef _DEBUG
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
        ret = SYNC_CALL_RET<long>(m_io, m_cond, m_io_mutex, &AudioMonitor::Refresh, this);
    }
    else
    {
        if (m_current_status == monitor_status_t::INITERROR)
            return -1;

        ret = RefreshSessions();
    }

    return static_cast<long>(ret); // TODO: error codes
}
#endif

/*
    Gets current config
*/
vo::monitor_settings AudioMonitor::GetSettings()
{
    vo::monitor_settings ret;

    std::unique_lock<std::recursive_mutex> l(m_mutex, std::try_to_lock);

    if (!l.owns_lock())
    {
        ret = SYNC_CALL_RET<vo::monitor_settings>(m_io, m_cond, m_io_mutex, &AudioMonitor::GetSettings, this);
    }
    else
    {
        ret = m_settings;
    }

    return ret;
}

/*
    Parses new config and applies it.

    Settings will be modified when parsed if some values where incorrect.
*/
void AudioMonitor::SetSettings(vo::monitor_settings& settings)
{
    std::unique_lock<std::recursive_mutex> l(m_mutex, std::try_to_lock);

    if (!l.owns_lock())
    {
        SYNC_CALL(m_io, m_cond, m_io_mutex, &AudioMonitor::SetSettings, this, std::ref(settings));
    }
    else
    {
        if (m_current_status == monitor_status_t::INITERROR)
            return;

        m_settings = settings;

        // convert excluded process names to lower
        for (auto n : m_settings.excluded_process)
        {
            std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        }

        // convert included process names to lower
        for (auto n : m_settings.included_process)
        {
            std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        }

        // If volume is in %, can be positive or negative.
        //  example if vol reduction % is -50%, will actually increase volume by 50%!
        // max limit for both is 1.0f
        if (m_settings.ses_global_settings.vol_reduction > 1.0f)
            m_settings.ses_global_settings.vol_reduction = 1.0f;
        if (m_settings.ses_global_settings.treat_vol_as_percentage)
        {
            // limit -1.0f to 1.0f
            if (m_settings.ses_global_settings.vol_reduction < -1.0f)
                m_settings.ses_global_settings.vol_reduction = -1.0f;
        }
        if (!m_settings.ses_global_settings.treat_vol_as_percentage)
        {
            // limit 0.0f to 1.0f
            if (m_settings.ses_global_settings.vol_reduction < 0.0f)
                m_settings.ses_global_settings.vol_reduction = 0.0f;
        }

        if (m_settings.ses_global_settings.vol_up_delay.count() < 0)
            m_settings.ses_global_settings.vol_up_delay = std::chrono::milliseconds::zero();

        // TODO: complete here when adding options, if compiler with different align is used, comment this line.
#ifdef _DEBUG
       // static_assert(sizeof(vo::monitor_settings) == 144, "Update AudioMonitor::SetSettings!"); // a reminder, read todo.
#endif




        ApplyMonitorSettings();

        // return applied settings
        settings = m_settings;

        dprintf("AudioMonitor::SetSettings new settings parsed and applied.\n");
    }

}

float AudioMonitor::GetVolumeReductionLevel()
{
    float ret;

    std::unique_lock<std::recursive_mutex> l(m_mutex, std::try_to_lock);

    if (!l.owns_lock())
    {
        ret = SYNC_CALL_RET<float>(m_io, m_cond, m_io_mutex, &AudioMonitor::GetVolumeReductionLevel, this);
    }
    else
    {
        if (m_current_status == monitor_status_t::INITERROR)
            return -1.0f;

        ret = m_settings.ses_global_settings.vol_reduction;
    }

    return ret;
}

auto AudioMonitor::GetStatus() -> monitor_status_t
{
    return m_current_status; // thread safe, std::atomic
}



#if 0 //TEST DELETE
// Shortcut to sync/async calls
template <typename ft, typename... pt>
void AudioMonitor::ASYNC_CALL(ft&& f, pt&&... args)
{
    return ASYNC_CALL(m_io, std::forward<ft>(f), std::forward<pt>(args)...));
}
template <typename rt, typename ft, typename... pt>
rt AudioMonitor::SYNC_CALL_RET(ft&& f, pt&&... args)
{
    return SYNC_CALL_RET<rt>(m_io, m_cond, m_io_mutex, std::forward<ft>(f), std::forward<pt>(args)...));
}
template <typename ft, typename... pt>
void AudioMonitor::SYNC_CALL(ft&& f, pt&&... args)
{
    SYNC_CALL(m_io, m_cond, m_io_mutex, std::forward<ft>(f), std::forward<pt>(args)...));
}
#endif


} // end namespace vo

#endif
