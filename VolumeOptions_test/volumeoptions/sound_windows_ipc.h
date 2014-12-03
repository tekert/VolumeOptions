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

#ifndef SOUND_WINDOWS_IPC_H
#define SOUND_WINDOWS_IPC_H

#include <boost/version.hpp>
#include <boost/detail/workaround.hpp>
#if BOOST_WORKAROUND(BOOST_VERSION, < 105700)
#error "Please use boost > 1.57  native windows mutex abandonement error is not supported in this version."
#endif

#include <boost/interprocess/managed_windows_shared_memory.hpp>

#if defined(BOOST_INTERPROCESS_FORCE_GENERIC_EMULATION)
// Some protection for internal library abandoned generic mutexes, throws error 22(mutex) 23(condition) on timeout
#define BOOST_INTERPROCESS_ENABLE_TIMEOUT_WHEN_LOCKING
#define BOOST_INTERPROCESS_TIMEOUT_WHEN_LOCKING_DURATION_MS 4000
#endif

#include <boost/interprocess/allocators/allocator.hpp>
#include <boost/interprocess/containers/set.hpp>
#include <boost/interprocess/containers/string.hpp>
// These below must be included using native windows mutexes
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/interprocess_recursive_mutex.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>

// forked from boost 1_57_0, only 3 lines changed. they are marked with EDIT keyword
#include "../volumeoptions/boost/interprocess/ipc/message_queue_win.hpp" // modified to use windows managed mem

// Note, boost 1_57_0  boost/interprocess/detail/workaround.hpp was eddited to use experimental windows native sync
//  this was done commenting out the line 22: #define BOOST_INTERPROCESS_FORCE_GENERIC_EMULATION
//  on future boost versions this may change and no longer be experimental, works very good, its needed.
#if defined(BOOST_INTERPROCESS_WINDOWS) && defined(BOOST_INTERPROCESS_FORCE_GENERIC_EMULATION)
#error "Please comment line 22: #define BOOST_INTERPROCESS_FORCE_GENERIC_EMULATION in boost/interprocess/detail/workaround.hpp"
#endif

//#include <boost/thread.hpp>

#include <set>
#include <string>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

#include "../volumeoptions/sound_windows.h"

namespace vo {
namespace ipc {

/*
    
    NOTE: this module is experimental and IPC is optional, the last part: "message protocol" was done in a rush.
        the shared memory blocks are good (DeviceIPCManager is almost good, MessageQueueIPCHandler is really ugly),
        this is 'working' but it will need a complete new design and code rewrite to be presentable (is that, i was
        initially tought to be small code and simple, but it cant be simple and pretty when coding an ipc protocol)
    
*/

/*
    To sum up:

        Every process stores info about wich deviceid(like services) it provides in a personal (per process)
            shared set like explained below, this is atomic, like if where a single global set.

        Every process also has a local remote_processes map, like a cache of where to find the processs that manages a
            particular 'deviceid'

        Think it like, how a network switch ARP protocol find the mac of an ip in lan, but this is shared memory so:

        We have a global lookup table where every running process posts his PID (its simple, we hope it doesnt corrupt)
            think global lookup table like cables connected to a switch.
        We first ask every of the process in the global lookup table (like a broadcast) if that process has a
            requested 'deviceid', if it has it we store that info in our local remote process map cache,
            (this is done atomically scanning all processes sets)
        Now every time we need to send something to that 'deviceid' we use the local remote cache (a std::map)
            to know to wich process_id to send to (think processid like MAC, and 'deviceid' like IP).
        The protocol for sending messages for other process is simple:
             32 bits for source processid, 32bits for message_code,  32bits for message id, 
             and 128 max bytes for buffer data (used to send deviceid or string reponses).
            If a process receives a message, it responds with code 'ACK' always (if its alive of course)
                and the response message string if applicable back to source process on the same messageid.
            In every received message we check if 'deviceid' is managed by 'this' process.
            if waiting for a reponse to a messageid we sent, if timeouts (10 millisec max), the sender removes that
                process from the global pid table and updates his local cache with a new broadcast trying to claim
                that device id.

    =============================================================================================================
    |  These are some personal headches using boost interprocess and cheking object integrity on porcess abort  |
    =============================================================================================================

    Skip to 'to sum up' for short version.

    Ok, this is tricky, we need to protect access to a shared object.
        boost managed mem is already protected for creating objects, finding objects references with a internal mutex.

    The thing is.. if we use a mutex to protect an object stored in a single shared memory, and the process
        terminates while holding a mutex we will get undefined state in our object, if we lock the mutex on destruction
        to erase data, we are calling for trouble too, object destruction is also called on process abort depending.
    Even worse.. the internal library managed shared memory alloc mutex can get abandoned. in that case there could
        be unlinked allocated memory (sort of leak in a segment) or if using atomic_func, partial object creation,
        in essence.. a problem.


    Solution:

    ALL_PLATFORMS: use robust mutexes or timed mutexes to check for abandonement and then recreate the segment
        with a shadow backup copy and a log like databases do.

    WINDOWS_ONLY: a simple solution that works on windows is taking advantage that the OS will remove the shared
        memory when the process terminates, that means every process should write to his own segment of shared mem
        using a easy to find/iterate name for the other process/es to find.
        We have to write our object there.
        This solution is only apropiate when not many access are done and a limited number of process will use it.
        ugly yeap.. until i can think of something else.

        Current implementation: windows only (subject to change on library updates):

        Every process creates his own shared SharedWStringSet in his "own" shared memory, and every time is
        going to access the object it locks a shared mutex using native windows mutex so we can check for abandonement,
        it searchs for others shared mems currently open(not process terminated) and checks if it can find something,
        ugly but effective, we dont need performance on this.. besides i tested 500 microseconds to search 50 mems
        on windows using native mutex, way better than the generic ones in performance.

        Shared windows managed segments:
        * One for each individual process. (m_personal_managed_shm)
        * One and only one to store a shared mutex or global objects (m_global_managed_shm)
        * One for message queue endpoint for receiving messages...
        * Opens Variable number of message queues for each discovered process, to broadcast or send individual
            messages. 
        (every one of the message queues uses a modification of boost message queues for windows managed mem)

        Objects:
        * One shared set in his each "personal" process -> m_personal_managed_shm
        * One recurive mutex in -> m_global_managed_shm

        We serialize access to DeviceIPCManager 'devices sets' with a shared native mutex 
            (it must support abandonement error)


    =============================================================================================================
    |                                              IDEAS                                                        |
    =============================================================================================================

    Until i can find a way to garantee object state:
        maybe with object cheking/reconstruction, boost robust mutex or native mutex and database like logs.


    I've been reading thesis and projects on this subject, im thinking of doing a copy on write explanation:

        Writes are fully serialized, only one write transaction may be active at a time globally, 
            segment is opened with copy on write mode.
        We have 3 segments of shared memory, SMy, SMz and SMs (special one small enough to hold a bit and a mutex).
        SMs holds a bit indicating wich shared memory has the most recent valid data, writers change this value.
            example 1 = access SMy, 0 = access SMz
        Before a writer finishes, if bit is 0(SMz) it overwrites all his changes to the other SMy, and before
            unlocking the writers mutex, swaps the bit and unlocks.
        Now every reader should check first that bit and choose what segment to read.

        With my device looup design  i need every access to be atomic so, a single global mutex for all access will
            replace the writers mutex.

        This is similar to a shadow copy i think, its the only thing i can come up with to protect objects on process
            termination, it requieres a full copy for every write and double the memory...
            i should only copy local modified pages, need to see how.
            im working on this, maybe one day ill do it.
        The current solution is better i think.. someday.. maybe someday i will continue thinking.
            or compile and use MPI, tough.. too heavy for this simple thing.

*/  


// Typedefs of allocators and containers

// Use windows managed shared mem in this case                                      /////////////////////
typedef boost::interprocess::managed_windows_shared_memory::segment_manager         segment_manager_t;
typedef boost::interprocess::allocator<void, segment_manager_t>                     void_shm_allocator;
                                                                                    /////////////////////
typedef boost::interprocess::allocator<char, segment_manager_t>                     char_shm_allocator;
typedef boost::interprocess::basic_string < char,
    std::char_traits<char>, char_shm_allocator >                                    shm_string;
                                                                                    /////////////////////
typedef boost::interprocess::allocator<wchar_t, segment_manager_t>                  wchar_t_shm_alocator;
typedef boost::interprocess::basic_string < wchar_t, std::char_traits<wchar_t>,
    wchar_t_shm_alocator >                                                          shm_wstring;
                                                                                    /////////////////////
typedef boost::interprocess::allocator<shm_wstring, segment_manager_t>              wstring_shm_allocator;
typedef boost::interprocess::set < shm_wstring, std::less<shm_wstring>,
    wstring_shm_allocator >                                                         shm_wstring_set;
                                                                                    /////////////////////

class DeviceIPCManager;

/*
    Instantiates a shared wstring set in managed shared memory indicated by parameter sp_managed_shm

    Use get_offsetptr() to obtain the offset shm_wstring_set pointer in local process memory.

    throws on error creating or accessing the shared set.
*/
class SharedWStringSet // TODO: template it wstring or string
{
public:
    SharedWStringSet(std::shared_ptr<boost::interprocess::managed_windows_shared_memory> sp_managed_shm,
        const std::string& set_name) throw(...);
    ~SharedWStringSet();

    shm_wstring_set* get_localoffsetptr();

    std::string get_set_name() const;
    bool we_created_this() const;
 
private:

    void construct_objects_atomic(
        std::shared_ptr<boost::interprocess::managed_windows_shared_memory> sp_managed_shm);

    // Set to store wstrings in shared memory
    shm_wstring_set* m_set_wstring_offset = nullptr; // managed shared memory pointer to wstring set.

    std::string m_device_set_name;

    bool m_we_created_set;
};

namespace win {

#if 0
class DeviceIPCManager2
{
public:
    inline DeviceIPCManager& get()
    {
        return DeviceIPCManager::get();
    }

    ~DeviceIPCManager2()
    {
        std::lock_guard<std::mutex> l(m_mutex);
        if (!--num_instances)
        {
            m_instance.reset();
        }
    }
    DeviceIPCManager2()
    {
        std::lock_guard<std::mutex> l(m_mutex);
        if (++num_instances == 1)
        {
            m_instance.reset(std::make_unique<DeviceIPCManager>());
        }
    }

private:
    static std::unique_ptr<DeviceIPCManager> m_instance;
    static std::mutex m_mutex;
    static unsigned int num_instances;
};
#endif

class MessageQueueIPCHandler;

/*
    Handles interprocess comunication to sync wich process controls wich audio device ID. (that part is good)

    It also handles communication messages to other volume options processes (working, but im not proud of)
*/
class DeviceIPCManager
{
public:

    // Make this class a singleton, we really need one per process.
    static DeviceIPCManager& DeviceIPCManager::get()
    {
        // NOTE: c++11 only, 
        //  if not using c++11, use std::call_once BUT instance object after calling get(), this was hard to debug
        static DeviceIPCManager inst;
        return inst;
    }

    virtual ~DeviceIPCManager();
    DeviceIPCManager(const DeviceIPCManager &) = delete; // non copyable
    DeviceIPCManager& operator= (const DeviceIPCManager&) = delete; // non copyassignable

    enum command_t {am_start, am_pause, am_test};
    int process_command(const command_t command, const std::wstring& deviceid, std::shared_ptr<AudioMonitor> sp);

    bool find_device(const std::wstring& deviceid); // Deprecated
    int unset_device(const std::wstring& deviceid);
    int set_device(const std::wstring& deviceid, std::shared_ptr<vo::AudioMonitor> spAudioMonitor);

    void clear_local_insertions();

    std::shared_ptr<vo::AudioMonitor> get_audiomonitor_of(const std::wstring& deviceid);

    std::shared_ptr<boost::interprocess::managed_windows_shared_memory> get_personal_shared_mem_manager();
    std::shared_ptr<boost::interprocess::managed_windows_shared_memory> get_global_shared_mem_manager();

private:

    DeviceIPCManager() throw(...);

    // PID table, to lookup for remote processes
    enum pid_lookup_table_modes_t { pid_add = 1, pid_remove = 2, pid_search = 3 };
    template <pid_lookup_table_modes_t mode>
    bool pid_table(std::shared_ptr<boost::interprocess::managed_windows_shared_memory>& sp_pidtable,
        const boost::interprocess::ipcdetail::OS_process_id_t process_id);

    bool scan_shared_segments(const std::wstring& deviceid = L"",
        boost::interprocess::ipcdetail::OS_process_id_t *const found_in_pid = nullptr);

    // Shared segment creation helpers:
    std::shared_ptr<boost::interprocess::managed_windows_shared_memory>
        create_free_managed_smem(const std::string& base_name,
        const boost::interprocess::offset_t block_size, unsigned int& chosen_id);
    std::shared_ptr<boost::interprocess::managed_windows_shared_memory>
        open_create_managed_smem(const std::string& name, const boost::interprocess::offset_t block_size);
    std::shared_ptr<boost::interprocess::windows_shared_memory>
        open_create_smem(const std::string& name, const boost::interprocess::offset_t block_size);


    // deviceID(managed by) -> process(process id)  local lookup cache, so we dont have to do a broadcast.
    std::map<std::wstring, boost::interprocess::ipcdetail::OS_process_id_t> m_tracking_managers_cache;

    // keeps track of process managed devieids, current process insertions to shared set.
    std::unordered_map<std::wstring, std::weak_ptr<vo::AudioMonitor>> m_local_claimed_devices;
    std::recursive_mutex m_local_claimed_devices_mutex;


    // Our shared objects:
    std::unique_ptr<SharedWStringSet> m_personal_shared_set;
    boost::interprocess::interprocess_recursive_mutex *m_global_rmutex_offset = nullptr;

    // Message Queue for Volume Options comms
    std::unique_ptr<MessageQueueIPCHandler> m_message_queue_handler;


    // Instanced shared segments:
    std::shared_ptr<boost::interprocess::managed_windows_shared_memory> m_global_managed_shm;
    std::shared_ptr<boost::interprocess::managed_windows_shared_memory> m_global_shared_pidtable;
    std::shared_ptr<boost::interprocess::managed_windows_shared_memory> m_personal_managed_shm;
        

    // Shared segments names
    const std::string m_global_managed_shared_memory_name;
    const std::string m_global_pid_table_name;
    const std::string m_global_recursive_mutex_name;
    std::string m_personal_managed_sm_name;

    // Shared segments personal base names
    const std::string m_personal_managed_sm_base_name;

    // Objects names
    std::string m_personal_devices_set_name; // stored inside 'm_personal_managed_sm_name'

    // current process pid.
    boost::interprocess::ipcdetail::OS_process_id_t m_process_id;

#if defined(BOOST_INTERPROCESS_WINDOWS) && defined(BOOST_INTERPROCESS_FORCE_GENERIC_EMULATION)
    const unsigned int m_max_retry = 0; // how many retries to reclaim an abandoned windows native mutex
#else
    const unsigned int m_max_retry = 20; // how many retries to reclaim an abandoned windows native mutex
#endif

    friend class MessageQueueIPCHandler;
};


/*
    Instances a message queue to handle comunication with other VolumeOptions processes

    NOTE: its intended to create one of these per process, TODO: enforce this.
        this needs a complete rewrite, but its working and dont have time now.
*/
class MessageQueueIPCHandler
{
public:
    MessageQueueIPCHandler(boost::interprocess::ipcdetail::OS_process_id_t process_id);
    ~MessageQueueIPCHandler();
    MessageQueueIPCHandler(const MessageQueueIPCHandler &) = delete; // non copyable
    MessageQueueIPCHandler& operator= (const MessageQueueIPCHandler&) = delete; // non copyassignable

    enum mq_message_code_t
    {
        mq_test = 0x0,
        mq_ping = 0x1,
        mq_wasapi_start = 0x2,
        mq_wasapi_pause = 0x3,
        mq_ack = 0xEF0FFFFE,
        mq_abort = 0xFFFFFFFF
    };

    struct vo_response_data_t
    {
        static const std::wstring OK;
        static const std::wstring FAIL;
        static const std::wstring TIMEOUT;
    };

    struct vo_message_t
    {
        vo_message_t(boost::interprocess::ipcdetail::OS_process_id_t _source_pid,
            mq_message_code_t mc)
            : source_pid(_source_pid)
            , message_code(mc)
        {}
        vo_message_t(boost::interprocess::ipcdetail::OS_process_id_t _source_pid,
            mq_message_code_t mc, std::wstring _device_id)
            : source_pid(_source_pid)
            , message_code(mc)
            , device_id(_device_id)
        {}

        boost::interprocess::ipcdetail::OS_process_id_t source_pid;
        mq_message_code_t message_code;
        std::wstring device_id;
    };

    // This controls the policy when the buffer is full to send.
    enum full_policy_t
    {
        trysend,
        block,
        timedblock
    };

    unsigned long send_message(std::shared_ptr<boost::interprocess::message_queue>& mq_destination,
        const vo_message_t& message, const full_policy_t& smode = trysend, const int& priority = 1,
        const unsigned long reply_message_id = 0);

    inline unsigned long send_message(const std::string& mq_destionation_name,
        const vo_message_t& message, const full_policy_t& smode = trysend, const int& priority = 1,
        const unsigned long reply_message_id = 0);

    inline unsigned long send_message(const boost::interprocess::ipcdetail::OS_process_id_t& pid,
        const vo_message_t& message, const full_policy_t& smode = trysend, const int& priority = 1,
        const unsigned long reply_message_id = 0);

    std::wstring wait_for_reply(const unsigned long message_id);

private:

    bool create_message_queue_handler(const std::string& name);
    void listen_handler();
    struct mq_packet_t;
    void store_response(mq_packet_t r);

    inline unsigned long get_unused_messageid(); // unique per process

    std::shared_ptr<boost::interprocess::message_queue> m_personal_message_queue;

    const std::string m_personal_message_queue_base_name;
    std::string m_personal_message_queue_name;

   // boost::thread m_thread_personal_mq;
    std::thread m_thread_personal_mq;

    static const int MQDATASIZE = 128;
    struct mq_packet_t
    {
        mq_packet_t() { memset(buffer, 0, MQDATASIZE); }
        unsigned long source_pid; // process id
        MessageQueueIPCHandler::mq_message_code_t message_code;
        char buffer[MQDATASIZE];
        unsigned long message_id; // unique message id for this process
    };
    static unsigned long ms_next_message_id;
    static std::recursive_mutex ms_static_messageid_gen;

    // messageid -> received message (received replies messages have code mq_ack and messageid)
    std::unordered_map<unsigned long, mq_packet_t> m_receive_buffer;
    std::mutex m_receive_buffer_mutex;
    std::condition_variable m_receive_buffer_cond;

#if defined(BOOST_INTERPROCESS_WINDOWS) && defined(BOOST_INTERPROCESS_FORCE_GENERIC_EMULATION)
    const unsigned int m_max_retry = 0; // how many retries to reclaim an abandoned windows native mutex
#else
    const unsigned int m_max_retry = 20; // how many retries to reclaim an abandoned windows native mutex
#endif

    // current process pid.
    boost::interprocess::ipcdetail::OS_process_id_t m_process_id;
};


} // end namespace win
} // end namespace ipc
} // end namespace vo

#endif