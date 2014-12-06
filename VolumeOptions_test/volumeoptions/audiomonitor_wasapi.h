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

#include <boost/asio.hpp> // include Asio before including windows headers
#include <boost/asio/steady_timer.hpp>

#include <SDKDDKVer.h>
#include <Audiopolicy.h>
#include <Mmdeviceapi.h>

#include <unordered_map>
#include <set>
#include <utility>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

#include "../volumeoptions/config.h"
#include "../volumeoptions/vo_settings.h"

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(x)             \
    if(x != NULL)                   \
    {                               \
        ULONG _n = x->Release();    \
        /* n ? x : */ x = NULL ;    \
    }
#endif

#ifndef CHECK_HR
#define CHECK_HR(hr) if (FAILED(hr)) { assert(false); goto done; }
#endif

#ifdef _DEBUG
inline ULONG CHECK_REFS(IUnknown *p)
{
    ULONG n = p->AddRef();
    n = p->Release();
    return n;
}
#else
#define CHECK_REFS(...)
#endif

namespace vo {

class AudioMonitor;
/*
    Represents a single WASAPI Audio Session to be managed by AudioMonitor

    TODO: make it nested class of audiomonitor for now.
*/
class AudioSession : public std::enable_shared_from_this < AudioSession >
{
public:

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
    AudioSession(IAudioSessionControl *pSessionControl,
        const std::weak_ptr<AudioMonitor>& spAudioMonitor, float default_volume_fix = -1.0f);

    void InitEvents();
    void StopEvents();

    void state_changed_callback_handler(AudioSessionState newstatus);

    float GetCurrentVolume() const;
    HRESULT ApplyVolumeSettings(); // TODO: or make it public with async and bool restore_vol optional merging restorevolume
    void UpdateDefaultVolume(const float new_def);

    enum class resume_t { NORMAL = false, NO_DELAY = true };
    HRESULT RestoreVolume(resume_t callback_type = resume_t::NORMAL);
    void RestoreHolderCallback(boost::system::error_code const& e = boost::system::error_code());

    void ChangeVolume(const float v);

    void touch(); // sets m_last_modified_on now().
    void set_state(AudioSessionState state);

    AudioSessionState m_current_state;

    float m_default_volume; // always marks user default volume of this SID group session
    bool m_is_volume_at_default;  // if true, session volume is at user default volume

    bool m_excluded_flag;

    DWORD m_pid;
    std::wstring m_sid;
    std::wstring m_siid;

    mutable std::atomic<HRESULT> m_hrStatus;

    std::chrono::steady_clock::time_point m_last_modified_on;
    std::chrono::steady_clock::time_point m_last_active_state;

    IAudioSessionControl* m_pSessionControl;
    IAudioSessionEvents *m_pAudioEvents;
    IAudioSessionControl2* m_pSessionControl2;
    ISimpleAudioVolume* m_pSimpleAudioVolume;

    std::weak_ptr<AudioMonitor> m_wpAudioMonitor;  // To witch monitor it blongs

    /* Only this class can manage this object in thread safe way */
    friend class AudioMonitor;
    friend class AudioCallbackProxy;
};


/*
    WASAPI Monitor manager main class, Monitors current audio sessions to change volume on,
        based on customizable settings.
    
    Manages a single audio device per instance.

    Use ::create() to instance the class, it will return a std::shared_ptr.

*/
class AudioMonitor : public std::enable_shared_from_this < AudioMonitor >
{
public:
    /* Created with STOPPED status */

    // static std::shared_ptr<AudioMonitor> create(vo::monitor_settings&& settings,
        //const std::wstring& device_id = L"")  // if no variadic template support
    template<typename ...T>
    static std::shared_ptr<AudioMonitor> create(T&&... all)
    {
        return std::shared_ptr<AudioMonitor>(new AudioMonitor(std::forward<T>(all)...));
    }
    AudioMonitor(const AudioMonitor &) = delete; // non copyable
    AudioMonitor& operator= (const AudioMonitor&) = delete; // non copyassignable
    ~AudioMonitor();

    /* audio_endpoints: returns a DeviceID -> DeviceName map with current audio rendering devices */
    static HRESULT GetEndpointsInfo(std::map<std::wstring, std::wstring>& audio_endpoints,
        DWORD dwStateMask = DEVICE_STATE_ACTIVE);
    static std::set<std::wstring> GetCurrentMonitoredEndpoints();

    void ChangeDeviceID(const std::wstring& device_id);

    float GetVolumeReductionLevel();
    void SetSettings(vo::monitor_settings& settings);
    vo::monitor_settings GetSettings();

    /* If Resume is used while Stopped it will use Start() */
    long Stop(); // Stops all events and deletes all saved sessions restoring default state.
    long Pause(); // Restores volume on all sessions and freezes volume change.
    long Start(); // Resumes/Starts volume change and reapplies saved settings.
#ifdef _DEBUG
    long Refresh(); // DEBUG Gets all current sessions in SndVol
#endif

    enum class monitor_status_t { STOPPED, RUNNING, PAUSED, INITERROR };
    enum class monitor_error_t { OK, DEVICE_NOT_FOUND, DEVICEID_IN_USE, IOTHREAD_START_ERROR }; // TODO boost errors
    monitor_status_t GetStatus();
    
    std::shared_ptr<boost::asio::io_service> get_io() const;

private:

    AudioMonitor(const std::wstring& device_id = L"");
    void StartIOInit();
    void FinishIOInit();

#ifdef VO_ENABLE_EVENTS 
    long InitEvents();
    long StopEvents();
#endif

    void poll(); /* AudioMonitor main thread loop */

    HRESULT GetSessionManager(std::wstring& device_id);
    HRESULT RefreshSessions();
    void DeleteSessions();

    HRESULT SaveSession(IAudioSessionControl* pNewSessionControl, const bool unref);
    void DeleteSession(std::shared_ptr<AudioSession> spAudioSession); // Not used
    void DeleteExpiredSessions(boost::system::error_code const& e,
        std::shared_ptr<boost::asio::steady_timer> timer);
    void ApplySettings();
    bool isSessionExcluded(const DWORD pid, std::wstring sid = L"");

    void RemoveDeviceID(const std::wstring& device_id);
    HRESULT InitDeviceID(const std::wstring& device_id);

    IAudioSessionManager2* m_pSessionManager2;
    IAudioSessionNotification* m_pSessionEvents;
    std::wstring m_wsDeviceID; // current audio endpoint ID beign monitored.
    static std::set<std::wstring> m_current_monitored_deviceids;
    static std::mutex m_static_set_access;

    // Settings
    DWORD m_processid;
    vo::monitor_settings m_settings;
    const std::chrono::seconds m_inactive_timeout;
    const std::chrono::seconds m_delete_expired_interval;
    // Main sessions container type

    // Used to delay or cancel all volume restores  session_this_pointer -> timer
    std::unordered_map<const AudioSession*, std::unique_ptr<boost::asio::steady_timer>> m_pending_restores; //TODO: try to merge it with saved_sessions, new container or class

    bool m_auto_change_volume_flag; // SELFNOTE: we can delete this and use m_current_status, either way..
    //monitor_status_t m_current_status;
    std::atomic<monitor_status_t> m_current_status;
    monitor_error_t m_error_status;

    // Main sessions container type
    typedef std::unordered_multimap<std::wstring, std::shared_ptr<AudioSession>> t_saved_sessions;
    // Sessions currently Monitored, 
    //	map of SID -> list of AudioSession pointers with unique SIID (SessionInstanceIdentifier) 
    // You could look at it as group of different SIID sessions with the same SID.
    // note: remember to delete its corresponding session in m_pending_restores
    t_saved_sessions m_saved_sessions;
    typedef std::pair<std::wstring, std::shared_ptr<AudioSession>> t_session_pair;

    friend class AudioSession;
    friend class AudioCallbackProxy; /* To select wich private methods others classes can access */

    /* To sync Events with main class without "blocking" (async)
        or we cause mem leaks on simultaneous callbacks (confirmed) */
    std::shared_ptr<boost::asio::io_service> m_io;
    bool m_abort;
    std::thread m_thread_monitor; /* main class thread */

    // used when posting synchronous function calls to audiomonitor
    mutable std::mutex m_io_mutex;
    mutable std::condition_variable m_cond;


    // used to lock access to the class by only his own thread
    mutable std::recursive_mutex m_mutex;
};

} // end namespace vo

#endif

#endif