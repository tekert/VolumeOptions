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

#define BOOST_USE_WINDOWS_H

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
//  on future boost versions this may change and no longer be experimental, works very good, even better than default.
#if defined(BOOST_INTERPROCESS_WINDOWS) && defined(BOOST_INTERPROCESS_FORCE_GENERIC_EMULATION)
#error "Please comment line 22: #define BOOST_INTERPROCESS_FORCE_GENERIC_EMULATION in boost/interprocess/detail/workaround.hpp"
#endif


#include <set>
#include <string>
#include <memory>
#include <thread>
#include <mutex>

namespace vo {
namespace ipc {

/*
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
        * Variable number of message queues for each discovered process, to broadcast or send individual messages. 
        (every one of the message queues uses a modification of boost message queues for windows managed mem)

        Objects:
        * One shared set in his each "personal" process -> m_personal_managed_shm
        * One string with PID in each "personal" process -> m_personal_managed_shm (TODO)
        * One recurive mutex in -> m_global_managed_shm

        We serialize access to DeviceIPCManager devices info with a shared native mutex 
            (it must support abandonement error)


    To sum up:
        Every process stores lookup info about wich deviceid(like services) it provides in a personal (per process)
            shared set like explained above.

        Think it like, how a network switch ARP protocol find the mac of an ip in lan, but this is shared memory so:
        We first ask every of our process (like a boradcast) if that process has this 'deviceid', if it has it
            we store that info in our local table, (this is done atomically scanning all processes sets)
        Now every time we need to send something to that deviceid y uses the process internal table (a std::map)
            to know wich internal_id is used (think internalid like MAC, and deviceid like IP, exept we lack
            a broadcast protocol so each 'mac' or internalid is a iterable number from 1 to ~50 or 100)
        The protocol for sending messages for other process is simple:
            Higher 32 bits for source remoteid, lower 32bits for message id, and 128 max bytes for buffer data, where
                we send the deviceid always.
            If a process receives a message, it responds with an 'ACK' always (if its alive of course)
                and the response message about the received message back to source process.
            In every received message we check if that deviceid corresponds to 'this' process, using the global mutex
                to make it atomic.

        TODO: i know i should use process IDs as 'mac', but i need a way to broadcast..., maybe in a global shared
            segment with current process IDs running, but if a process crashes it will still be there, someone should go
            and clean it but if the one that is cleaning crashes?, for that reason i need to analize more.

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

    use get_offsetptr() to obtain the offset shm_wstring_set pointer in local process memory.

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

namespace win
{

    class MessageQueueIPCHandler
    {



    };


    class DeviceIPCManager
    {
    public:
        
        // Make this class a singleton, we really need one per process.
        static DeviceIPCManager& DeviceIPCManager::get()
        {
            std::call_once(m_once_flag, []{ m_instance.reset(new DeviceIPCManager); });
            return *m_instance.get();
        }
        virtual ~DeviceIPCManager();
        DeviceIPCManager(const DeviceIPCManager &) = delete; // non copyable
        DeviceIPCManager& operator= (const DeviceIPCManager&) = delete; // non copyassignable

        void process_command(int command);

        bool find_device(const std::wstring& deviceid); // Deprecated
        int unset_device(const std::wstring& deviceid);
        int set_device(const std::wstring& deviceid);

        void clear_local_insertions();

        std::shared_ptr<boost::interprocess::managed_windows_shared_memory> get_personal_shared_mem_manager();
        std::shared_ptr<boost::interprocess::managed_windows_shared_memory> get_global_shared_mem_manager();

    private:

        DeviceIPCManager() throw(...);

        // Singleton helpers
        static std::unique_ptr<DeviceIPCManager> m_instance;
        static std::once_flag m_once_flag;

        // TODO reorganize this class

        enum pid_lookup_table_modes_t { pid_add = 1, pid_remove = 2, pid_search = 3 };
        template <pid_lookup_table_modes_t mode> // "add" or "remove"
        bool pid_table(
            std::shared_ptr<boost::interprocess::managed_windows_shared_memory>& sp_pidtable,
            const boost::interprocess::ipcdetail::OS_process_id_t process_id);

        // the ugly thing described before.
        bool scan_shared_segments(const std::wstring& deviceid = L"",
            boost::interprocess::ipcdetail::OS_process_id_t *const found_in_pid = nullptr);

        // deviceID(managed by) -> process(internal id) 
        std::map<std::wstring, unsigned long> m_remote_device_masters;

        // keeps track of local insertions to erase them from sharedmem on destruction. (not used with this design)
        std::set<std::wstring> m_local_insertions;

        // Our shared objects:
        std::unique_ptr<SharedWStringSet> m_personal_up_devices_set;
        boost::interprocess::interprocess_recursive_mutex *m_global_sharedset_rmutex_offset = nullptr;

        std::shared_ptr<boost::interprocess::managed_windows_shared_memory>
            create_free_managed_smem(const std::string& base_name,
                const boost::interprocess::offset_t block_size, unsigned int& chosen_id);
        std::shared_ptr<boost::interprocess::managed_windows_shared_memory>
            open_create_managed_smem(const std::string& name, const boost::interprocess::offset_t block_size);
        std::shared_ptr<boost::interprocess::windows_shared_memory>
            open_create_smem(const std::string& name, const boost::interprocess::offset_t block_size);

        std::shared_ptr<boost::interprocess::managed_windows_shared_memory> m_global_managed_shm;
        std::shared_ptr<boost::interprocess::managed_windows_shared_memory> m_global_shared_pidtable;
        std::shared_ptr<boost::interprocess::managed_windows_shared_memory> m_personal_managed_shm;
        std::shared_ptr<boost::interprocess::message_queue> m_personal_message_queue;

        // Shared segments names
        const std::string m_global_managed_shared_memory_name;
        const std::string m_global_pid_table_name;
        const std::string m_global_recursive_mutex_name;
        std::string m_personal_managed_sm_name;
        std::string m_personal_message_queue_name;
        // Shared segments personal base names
        const std::string m_personal_managed_sm_base_name;
        const std::string m_personal_message_queue_base_name;
        // Objects names
        std::string m_personal_devices_set_name; // stored inside 'm_personal_managed_sm_name'



        bool create_message_queue_handler(const std::string& name);
        void mq_listen_handler();
        bool mq_send_message(std::shared_ptr<boost::interprocess::message_queue>& mq_destination,
            uint_least32_t message, const int mode = 0, const int priority = 1);
        inline bool mq_send_message(const std::string& mq_destionation_name, const uint_least32_t message,
            const int mode = 0, const int priority = 1);
        inline bool mq_send_message(const boost::interprocess::ipcdetail::OS_process_id_t pid,
            const uint_least32_t message, const int mode = 0, const int priority = 1);
        std::thread m_thread_personal_mq;

        boost::interprocess::ipcdetail::OS_process_id_t m_process_id;

        const unsigned int m_max_slots = 50;
#if defined(BOOST_INTERPROCESS_WINDOWS) && defined(BOOST_INTERPROCESS_FORCE_GENERIC_EMULATION)
        const unsigned int m_max_retry = 0; // how many retries to reclaim an abandoned windows native mutex
#else
        const unsigned int m_max_retry = 20; // how many retries to reclaim an abandoned windows native mutex
#endif

        enum message_queue_messages_t
        {
            mq_test = 0u,
            mq_ping = 0x1,

            mq_wasapi_start = 0x2,
            mq_wasapi_pause = 0x3,

            mq_add_sender = 0xFFFFF0EE,

            mq_ack = 0xEF0FFFFE,

            mq_abort = 0xFFFFFFFF
        };
    };


} // end namespace win
} // end namespace ipc
} // end namespace vo

#endif