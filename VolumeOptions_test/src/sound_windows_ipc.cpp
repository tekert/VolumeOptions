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

#include "../volumeoptions/vo_config.h"
#include "../volumeoptions/utilities.h"
#include "../volumeoptions/sound_windows_ipc.h" // NOTE: include this it before boost/interprocess includes

#include <limits>

#include <boost/interprocess/sync/scoped_lock.hpp>


namespace vo {
namespace ipc {
namespace win {

    ///////////////////////////////////////////////////////////////////////////////////////////////////////
    /////////                  VolumeOptions Wasapi shared memory manager                       ///////////
    ///////////////////////////////////////////////////////////////////////////////////////////////////////


/*
    See notes on header to understand all this.

    Singleton class.

    throws boost::interproces::interprocess_exception on managed segment creation error.
*/
DeviceIPCManager::DeviceIPCManager()
    : m_process_id()
    , m_global_managed_shared_memory_name("volumeoptions_win_global_msm-"VO_GUID_STRING)
    , m_global_pid_table_name("volumeoptions_win_global_pidtable-"VO_GUID_STRING)
    , m_global_recursive_mutex_name("volumeoptions_win_interp_rmutex-"VO_GUID_STRING)
    , m_personal_managed_sm_base_name("volumeoptions_win_personal_msm-"VO_GUID_STRING)
{
    using namespace boost::interprocess;

    // Set names for this process shared segments
    m_process_id = ipcdetail::get_current_process_id();
    m_personal_managed_sm_name = 
        m_personal_managed_sm_base_name + "-" + std::to_string(m_process_id);
    m_personal_devices_set_name = m_personal_managed_sm_base_name + "-devices_set";

    // We use windows managed shared mem in this case

    // SELFNOTE: make sure is big enogh, resizing is not easy.
    //
    // Pages in windows are typicaly 4k bytes 'dwPageSize' (mapped_region::get_page_size() is wrong about "Page size", 
    //  it not the same as 'dwAllocationGranularity' "Page granularity" ).
    //  dwAllocationGranularity is "The granularity with which virtual memory is allocated". used by VirtualAlloc.
    // either way, 64KiB is safe to have room, dwAllocationGranularity is typically 64KiB
    const offset_t block_size_personal_shared_segment = 64 * 1024;
    const offset_t block_size_unique_shared_segment = 32 * 1024;

    unsigned int retry = 0;
retry_lock:
    try
    {
        // --------- First, instantiate managed shared sectors: ---------

        // OpenCreate the unique shared memory segment for VolumeOptions.
        m_global_managed_shm = 
            open_create_managed_smem(m_global_managed_shared_memory_name, block_size_unique_shared_segment);

        // The lookup table to find other processes, we hope this will not get corrupted easily TODO: maybe merge this with global
        m_global_shared_pidtable =
            open_create_managed_smem(m_global_pid_table_name, block_size_unique_shared_segment);

        // Create a personal windows shared memory. (processID will be used as suffix)
        //  (we will be the only writer as explained in header)
        m_personal_managed_shm = std::make_shared<managed_windows_shared_memory>(create_only,
            m_personal_managed_sm_name.c_str(), block_size_personal_shared_segment);
        dprintf("IPC ManagedSharedMem: CREATED - %s \n", m_personal_managed_sm_name.c_str());
     
        // Create a personal message queue endpoint, to receive messages (using processID as name suffix).
        m_message_queue_handler = std::make_unique<MessageQueueIPCHandler>(m_process_id); // throws on error

        // --------- Next, instantiate sync primitives: ---------

        // Instance the unique global mutex for VolumeOptions.
        m_global_rmutex_offset = m_global_managed_shm->find_or_construct<interprocess_recursive_mutex>
            (m_global_recursive_mutex_name.c_str())();

        // --------- Last, instantiate our objects: ---------

        // Finally, open or create our personal shared set (1 writer (only us), multiple readers)
        m_personal_shared_set = std::make_unique<SharedWStringSet>
        //   where to search/place it           name for this set (it will open-create it)
            (m_personal_managed_shm,            m_personal_devices_set_name);


        // --------- Now we can use objects: ---------

        // Set our pid as active. do this last on construction.
        if (!pid_table<pid_lookup_table_modes_t::pid_add>(m_global_shared_pidtable, m_process_id))
        {
            // TODO, do a check if its alive if not delete it.
            std::exception("this shouldnt happen, a duplicate pid already stored?");
        }
    }
    catch (interprocess_exception& e)
    {
        std::cerr << e.what() << "  ecode: " << e.get_error_code() << "  native_code: "
            << e.get_native_error() << "function" << __FUNCTION__ << std::endl;

        if ((e.get_error_code() == error_code_t::owner_dead_error) && (retry++ <= m_max_retry))
            goto retry_lock;

        throw;
    }

}

DeviceIPCManager::~DeviceIPCManager()
{
    // Mark this process as inactive. delete if from table.
    pid_table<pid_lookup_table_modes_t::pid_remove>(m_global_shared_pidtable, m_process_id);

    // Unlink our resources inside the shared mem first, before destructing our shared mem.
    m_personal_shared_set.reset();

    m_message_queue_handler.reset();
}

std::shared_ptr<boost::interprocess::managed_windows_shared_memory>
    DeviceIPCManager::get_personal_shared_mem_manager()
{
    return m_personal_managed_shm;
}

std::shared_ptr<boost::interprocess::managed_windows_shared_memory>
    DeviceIPCManager::get_global_shared_mem_manager()
{
    return m_global_managed_shm;
}

/*
    Adds or removes a pid from the global shared active processes id lookup table.

    modes: 
    "pid_add"     return true if no duplicate exists when insertion took place.
    "pid_remove"  return true if the pid was removed, false if pid was not found
    "pid_search"  return true if pid exists, false if not.
 
*/
template <DeviceIPCManager::pid_lookup_table_modes_t mode>
bool DeviceIPCManager::pid_table(
    std::shared_ptr<boost::interprocess::managed_windows_shared_memory>& sp_pidtable,
    const boost::interprocess::ipcdetail::OS_process_id_t process_id)
{
    using namespace boost::interprocess;

    assert(sp_pidtable);

    std::string pid_string = std::to_string(m_process_id);

    unsigned int retry = 0;
retry_lock:
    try
    {
        // *see set_device comment.
        scoped_lock<interprocess_recursive_mutex> lock(*m_global_rmutex_offset);

        switch (mode)
        {
            case pid_lookup_table_modes_t::pid_add:
            {
                // we are mutex protected so this is safe here.
                if (!sp_pidtable->find<bool>(pid_string.c_str()).first)
                {
                    // remember: this throws if already there, boost doc is wrong.
                    bool* ret = sp_pidtable->construct<bool>(pid_string.c_str())(true);
                    dprintf("IPC PidTable: PID adding: %s, result: %d\n", pid_string.c_str(), ret? 1 : 0);
                    return true;
                }
                else
                {
                    dprintf("IPC PidTable: PID adding: %s, already there\n", pid_string.c_str());
                    return false;
                }
            }
            break;

            case pid_lookup_table_modes_t::pid_remove:
            {
                //!Destroys the created object, returns false if not present
                bool r = sp_pidtable->destroy<bool>(pid_string.c_str());
                dprintf("IPC PidTable: PID removing: %s, result: %d\n", pid_string.c_str(), r);
                return r;
            }
            break;

            case pid_lookup_table_modes_t::pid_search:
            {
                bool* ret = sp_pidtable->find<bool>(pid_string.c_str()).first;
                dprintf("IPC PidTable: PID Searching for: %s, result: %d\n", pid_string.c_str(), ret? 1 : 0);
                if (ret)
                    return *ret;
                else
                    return false;
            }
            break;
        }
    }
    catch (interprocess_exception& e)
    {
        // the only catch here is our main mutex gets abandoned (error_code_t::owner_dead_error)
        std::cerr << e.what() << "  ecode: " << e.get_error_code() << "  native_code: "
            << e.get_native_error() << "function" << __FUNCTION__ << std::endl;
        if ((e.get_error_code() == error_code_t::owner_dead_error) && (retry++ <= m_max_retry))
            goto retry_lock;

        throw;
    }

    return false;

#if 0
    // i realized that recontruction takes more code than the alternative, so.. this idea is abandoned.
    const int pid_block_size = 11;

    assert(sp_pidtable.get());

    std::string pid_string = std::to_string(m_process_id);
    // max number in unsigned long int(windows DWORD)(4 bytes) is 10 chars
    assert(sizeof(m_process_id) == 4);
    assert(pid_string.size() <= 10);

    unsigned int retry = 0;
retry_lock:
    try
    {
        // *see set_device comment.
        scoped_lock<interprocess_recursive_mutex> lock(*m_global_rmutex_offset);

        //////////////////////////////////////////////////////////////////////....
        // Start block 10 bytes | End block 1byte with 0 |  ....
        //////////////////////////////////////////////////////////////////////....
        // if segment size is 100 and block size is 11, the limit is 100 - (100 % 11) = 99

        mapped_region m_pid_table_region(*sp_pidtable, read_write);

        const std::size_t segment_sz = m_pid_table_region.get_size();
        // limit is multiple of pid_block_size
        const std::size_t segment_limit = segment_sz - (segment_sz % pid_block_size);
        // find a free slot
        unsigned int pos = 0;
        for (pos = 0;
            (pos < unsigned int(-1)) && (pos < segment_limit);
            pos += pid_block_size)
        {
            // if first byte is 0, this is a free slot
            if (m_pid_table_region.get_address[pos] == '\0')
                break;
        }

        if (pos >= segment_limit)
            throw std::exception("no more space to store pid"); // TODO proper error codes later

        char *address_start = (char*)m_pid_table_region.get_address() + pos;
        // we will separate pid with a null byte
        char a[11] = pid_string.c_str();
        memcpy(address_start, pid_string.c_str(), pid_string.size() + 1);

    }
    catch (interprocess_exception& e)
    {
        // the only catch here is our main mutex gets abandoned (error_code_t::owner_dead_error)
        std::cerr << e.what() << "  ecode: " << e.get_error_code() << "  native_code: "
            << e.get_native_error() << "function" << __FUNCTION__ << std::endl;
        if ((e.get_error_code() == error_code_t::owner_dead_error) && (retry++ <= m_max_retry))
            goto retry_lock;

        throw;
    }

    return true;
#endif
}


/*
    Opens or creates a named shared memory segment, 
        it will need to be mapped.

    throws boost::interprocess::interprocess_exception  on error.
*/
std::shared_ptr<boost::interprocess::windows_shared_memory>
DeviceIPCManager::open_create_smem(const std::string& name,
    const boost::interprocess::offset_t block_size)
{
    using namespace boost::interprocess;

    std::shared_ptr<boost::interprocess::windows_shared_memory> sp_msm;
    try
    {
        sp_msm = std::make_shared<windows_shared_memory>(open_only, name.c_str(), read_write);
        dprintf("IPC Not mapped SharedMem: OPENED - %s \n", name.c_str());
    }
    catch (interprocess_exception)
    {
        try
        {
            //std::cerr << e.what() << "    ->  Creating a new managed_sharedmem then."<< '\n';
            sp_msm = std::make_shared<windows_shared_memory>(open_or_create, name.c_str(), read_write, block_size);
            dprintf("IPC Not mapped SharedMem: OPENED-CREATED - %s \n", name.c_str());
        }
        catch (interprocess_exception)
        {
            throw;
        }
    }

    return sp_msm;
}

/*
    Opens or creates a named managed shared memory

    throws boost::interprocess::interprocess_exception  on error.
*/
std::shared_ptr<boost::interprocess::managed_windows_shared_memory>
    DeviceIPCManager::open_create_managed_smem(const std::string& name,
    const boost::interprocess::offset_t block_size)
{
    using namespace boost::interprocess;

    std::shared_ptr<boost::interprocess::managed_windows_shared_memory> sp_msm;
    try
    {
        sp_msm = std::make_shared<managed_windows_shared_memory>(open_only, name.c_str());
        dprintf("IPC ManagedSharedMem: OPENED - %s \n", name.c_str());
    }
    catch (interprocess_exception)
    {
        try
        {
            //std::cerr << e.what() << "    ->  Creating a new managed_sharedmem then."<< '\n';
            sp_msm = std::make_shared<managed_windows_shared_memory>(open_or_create, name.c_str(), block_size);
            dprintf("IPC ManagedSharedMem: OPENED-CREATED - %s \n", name.c_str());
        }
        catch (interprocess_exception)
        {
            throw;
        }
    }

    return sp_msm;
}

/*
    Finds a free name using intergers from 1 to 300 as sufix for base_name and creates
        a managed shared memory segment there, chosen_id will be set to free slot found.

    Returns not empty :   if a slot was found and shared mem was created, it will return chosen_id and shared_ptr.
    Returns empty :       if no slots free, shared mem not created.
    throws boost::interprocess::interprocess_exception on error.
*/
std::shared_ptr<boost::interprocess::managed_windows_shared_memory>
    DeviceIPCManager::create_free_managed_smem(const std::string& base_name,
    const boost::interprocess::offset_t block_size, unsigned int& chosen_id)
{
    using namespace boost::interprocess;

    std::string test_name;
    unsigned short free_slot;
    std::shared_ptr<boost::interprocess::managed_windows_shared_memory> sp_msm;
    for (free_slot = 1; free_slot != 300; free_slot++)
    {
        unsigned int retry = 0;
        retry_access:
        try
        {
            test_name = base_name + "-" + std::to_string(free_slot);
            sp_msm = std::make_shared< managed_windows_shared_memory>(create_only, test_name.c_str(), block_size);
            dprintf("IPC SharedMem: CREATED - %s\n", test_name.c_str());

            chosen_id = free_slot;

            break;
        }
        catch (boost::interprocess::interprocess_exception& e)
        {
            if (e.get_error_code() == error_code_t::already_exists_error)
                continue;

            if ((e.get_error_code() == error_code_t::owner_dead_error) && (retry++ <= m_max_retry))
                goto retry_access;

            if (free_slot == std::numeric_limits<uint_least16_t>::max())
                return 0;

            // else throw error.
            throw;
        }
    }

    if (sp_msm) assert(block_size == sp_msm->get_size());

    return sp_msm;
}

/*
    Search all individual shared memory blocks belonging to other processes and search the set for a matching
        wstring, this operation is atomic. (it needs to be)
    Or if deviceid is not supplied scan all the sets of other shared mems and updates our m_tracking_managers_cache.

    Return true if deviceid found in any process.
    returns found_in_id with process id only if supplied deviceid was found in that process.

    NOTE: we take advange of the fact that unreferenced shared memory on windows gets deleted, so we dont have to
        worry about corrupted objects etc. more info on header.
        Abanadoned mutexes are now handled internally with a retry count to reclaim it, if it cant be reclaimed it
            returns false.
*/
bool DeviceIPCManager::scan_shared_segments(const std::wstring& deviceid,
    boost::interprocess::ipcdetail::OS_process_id_t *const found_in_pid)
{
    using namespace boost::interprocess;

    assert(m_global_shared_pidtable.get());

    unsigned int retry = 0;
retry_lock:
    vo::high_resolution_clock::time_point now = vo::high_resolution_clock::now(); // TODO remove this line
    try
    {
        // This lock is not intended to protect the shared object (this is read only)
        //  its intended to make this search operation atomic at higher level.
        scoped_lock<interprocess_recursive_mutex> lock(*m_global_rmutex_offset);

        std::string sm_test_name;
        // Search all actives processes, this is like an ARP broadcast
        // NOTE: this is not thread safe, we must stop all activity first with a global mutex.
        for (auto b = m_global_shared_pidtable->named_begin(); b != m_global_shared_pidtable->named_end(); b++)
        {
            unsigned int retry_set_access = 0;
        retrysetaccess:
            try
            {
                std::unique_ptr<SharedWStringSet> upset;
                std::shared_ptr<managed_windows_shared_memory> spmsm;
                std::unique_ptr<void_shm_allocator> upconvertible_void_alloc;
                SharedWStringSet* pset = nullptr;

                // Use name of stored m_global_shared_pidtable object as pid string.
                // These Pids are the ones currently active.
                sm_test_name = m_personal_managed_sm_base_name + "-" + b->name();
                if (sm_test_name != m_personal_managed_sm_name)
                {
                    // try this name
                    spmsm = std::make_shared<managed_windows_shared_memory>(open_only, sm_test_name.c_str());
                    // opens the set if it exists in this managed memory, if not, its created empty.
                    // it can throw if a mutex is abandoned inside.
                    upset = std::make_unique<SharedWStringSet>(spmsm, m_personal_devices_set_name);
                    upconvertible_void_alloc = std::make_unique<void_shm_allocator>(spmsm->get_segment_manager());

                    pset = upset.get();
                }
                else // its ours, already opened.
                {
                    pset = m_personal_shared_set.get();
                    upconvertible_void_alloc = std::make_unique<void_shm_allocator>
                        (m_personal_managed_shm->get_segment_manager());
                }

                // NOTE: We dont need a mutex here, we are already using a global mutex for other purpose.

                if (!deviceid.empty())
                {   // check if device its there, if not, continue searching the next slot.
                    if (pset->get_localoffsetptr()->count(shm_wstring(deviceid.c_str(), *upconvertible_void_alloc)))
                    {
                        dwprintf(L"Found %s", deviceid.c_str()); dprintf(" in %s\n", sm_test_name.c_str());
                        if (found_in_pid) *found_in_pid = std::stoul(std::string(b->name()));
                        return true;
                    }
                }
                else
                {   // NOTE: not really used in this design, maybe delete this block..
                    if (sm_test_name != m_personal_managed_sm_name)
                    { // Scan this remote segment's set to find wich devices it has.
                        for (auto rd_id : *pset->get_localoffsetptr())
                        {   // update our local remote device managers map.
                            m_tracking_managers_cache[rd_id.c_str()] = std::stoul(std::string(b->name()));
                        }
                    }
                }
            }
            catch (boost::interprocess::interprocess_exception& e)
            {
                if ((e.get_error_code() == error_code_t::timeout_when_locking_error) // for interprocess mutexes
                    || (e.get_error_code() == error_code_t::timeout_when_waiting_error) // for interprocess conditions
                    || (e.get_error_code() == error_code_t::owner_dead_error)) // for windows native mutexes (boost 1.57)
                {
                    std::cerr << e.what() << "  ecode: " << e.get_error_code() << "  native_code: "
                        << e.get_native_error() << "function" << __FUNCTION__ << std::endl;

                    // retry set access if a windows native mutex got abandoned (it can be reclaimed now)
                    if ((e.get_error_code() == error_code_t::owner_dead_error)
                        && (retry_set_access++ <= m_max_retry))
                        goto retrysetaccess;
                }

                continue;
            }
        } // for
    }
    catch (interprocess_exception& e)
    {
        // the only catch here is our main mutex gets abandoned (error_code_t::owner_dead_error)
        std::cerr << e.what() << "  ecode: " << e.get_error_code() << "  native_code: "
            << e.get_native_error() << "function" << __FUNCTION__ << std::endl;

        if ((e.get_error_code() == error_code_t::owner_dead_error) && (retry++ <= m_max_retry))
            goto retry_lock;

        // if (e.get_error_code() == error_code_t::owner_dead_error) throw;
    }

    // this search will only happen when adding device ids, so its not so bad.
    vo::high_resolution_clock::time_point later = vo::high_resolution_clock::now(); // TODO remove
    printf("find took: %llu microsec\n", std::chrono::duration_cast<std::chrono::microseconds>(later - now).count());

    return false;
}

/*
    Main method for communicate with other processes

    Sends command to remote deviceid manager if exists

    return codes:
    0 -> deviceid is managed by this process already or we couldnt get info, do nothing
    -1 -> message sent ok, response received as failed.
    1 -> message sent ok, response received as OK.

    NOTE: this is.. very error prone, there are hardly any checks... needs rewrite :(
*/
int DeviceIPCManager::process_command(const command_t command, const std::wstring& deviceid,
    std::shared_ptr<AudioMonitor> spAudioMonitor)
{
    using namespace boost::interprocess;


    unsigned int retry = 0;
retry_lock:
    try
    {
        // *see set_device comment.
        scoped_lock<interprocess_recursive_mutex> lock(*m_global_rmutex_offset);

        {
            std::lock_guard<std::recursive_mutex> lock_local(m_local_claimed_devices_mutex);
            if (m_local_claimed_devices.count(deviceid))
                return 0;
        }

        if (!m_tracking_managers_cache.count(deviceid))
            set_device(deviceid, spAudioMonitor);

        int send_retry = 0;
        const int max_send_retry = 3;
    retry_send:
        unsigned long mid = 0;
        switch (command)
        {

        case am_start:
        {
            MessageQueueIPCHandler::vo_message_t message(m_process_id, MessageQueueIPCHandler::mq_wasapi_start,
                deviceid);
            mid = m_message_queue_handler->send_message(m_tracking_managers_cache[deviceid], message,
                MessageQueueIPCHandler::full_policy_t::trysend);
        }
        break;

        case am_pause:
        {
            MessageQueueIPCHandler::vo_message_t message(m_process_id, MessageQueueIPCHandler::mq_wasapi_pause,
                deviceid);
            mid = m_message_queue_handler->send_message(m_tracking_managers_cache[deviceid], message,
                MessageQueueIPCHandler::full_policy_t::trysend);
        }
        break;

        case am_test: // test
        {
            MessageQueueIPCHandler::vo_message_t message(m_process_id, MessageQueueIPCHandler::mq_test,
                deviceid);
            unsigned long mid = m_message_queue_handler->send_message(m_tracking_managers_cache[deviceid], message,
                MessageQueueIPCHandler::full_policy_t::trysend);
            dprintf("\nTEST: sending test message to pid: %u, sent: %u\n", m_tracking_managers_cache[deviceid], mid);
            if (mid)
            {
                std::wstring r = m_message_queue_handler->wait_for_reply(mid);
                dwprintf(L"TEST: Response received: %s\n", r.c_str());
            }
        }
        break;

        } // end switch


        // if message was sent
        if (mid)
        {
            // wait for a reply to that message.          
            std::wstring response = m_message_queue_handler->wait_for_reply(mid);
            dwprintf(L"TEST: Response received: %s\n", response.c_str());

            // if remote process is down, try to claim this device id
            if (response == MessageQueueIPCHandler::vo_response_data_t::TIMEOUT)
            {
                // cache for that process is invalid, remove it.
                m_tracking_managers_cache.erase(deviceid);
                // Mark that process as unresponsive. delete if from global table, hope this doesnt corrupt.
                pid_table<pid_lookup_table_modes_t::pid_remove>(m_global_shared_pidtable,
                    m_tracking_managers_cache[deviceid]);

                int r = set_device(deviceid, spAudioMonitor); // try to claim device id
                switch (r)
                {
                    case -1:
                    case 0:
                    case 1: // if error, already claimed, or claimed, return 0
                        return 0;
                    break;

                    case 2: // m_tracking_managers_cache was updated with new remote process
                        if (++send_retry > max_send_retry) return 0;
                        goto retry_send; // cache should be updated now, retry send to new host.
                    break;
                }
            }
            else
            {
                if (response == MessageQueueIPCHandler::vo_response_data_t::FAIL)
                    return -1;
                else if (response == MessageQueueIPCHandler::vo_response_data_t::OK)
                    return 1;
            }
        }
    }
    catch (interprocess_exception& e)
    {
        // the only catch here is our main mutex gets abandoned (error_code_t::owner_dead_error)
        std::cerr << e.what() << "  ecode: " << e.get_error_code() << "  native_code: "
            << e.get_native_error() << "function" << __FUNCTION__ << std::endl;

        if ((e.get_error_code() == error_code_t::owner_dead_error) && (retry++ <= m_max_retry))
            goto retry_lock;
    }

    return 0;
}

// Deprecated, remove it later.
bool DeviceIPCManager::find_device(const std::wstring& deviceid)
{
    return this->scan_shared_segments(deviceid);
}

/*
    To tell that we want to stop traking or managing this device id.

    returns 0:  error, couldnt unset deviceid.
    returns -1: we weren't managing or traking this device id, nothing is done.
    returns 1: unset succesfull, we are no longer managing this id, (let other processes find out and update)
    returns 2: we are no longer remotelly tracking this device id.
*/
int DeviceIPCManager::unset_device(const std::wstring& deviceid) // TODO add remove audiomonitor wp too
{
    using namespace boost::interprocess;

    unsigned int retry = 0;
retry_lock:
    try
    {
        // *see set_device comment.
        scoped_lock<interprocess_recursive_mutex> lock(*m_global_rmutex_offset);

        void_shm_allocator convertible_void_alloc(m_personal_managed_shm->get_segment_manager());
        shm_wstring shm_deviceid(deviceid.c_str(), convertible_void_alloc);

        if (m_personal_shared_set->get_localoffsetptr()->erase(shm_deviceid))
        {
            std::lock_guard<std::recursive_mutex> lock_local(m_local_claimed_devices_mutex);
            m_local_claimed_devices.erase(deviceid);
            return 1;
        }
        else
        {
            // if we where not managing it, maybe we where traking it.
            if (m_tracking_managers_cache.erase(deviceid))
                return 2;
        }

        return -1;
    }
    catch (interprocess_exception& e)
    {
        // the only catch here is our main mutex gets abandoned (error_code_t::owner_dead_error)
        std::cerr << e.what() << "  ecode: " << e.get_error_code() << "  native_code: "
            << e.get_native_error() << "function" << __FUNCTION__ << std::endl;

        if ((e.get_error_code() == error_code_t::owner_dead_error) && (retry++ <= m_max_retry))
            goto retry_lock;
    }

    return 0;
}

/*
    Tries to claim deviceid to be managed by us.

    returns 0 :     error, couldnt set deviceid.
    returns -1 :    device id already claimed to us, nothing is done.
    returns 1 :     device id free and claimed to us (we will broadcast that to our running processes).
    returns 2 :     deviceid controlled by remote process (cache will now point to that remote processes)
*/
int DeviceIPCManager::set_device(const std::wstring& deviceid, std::shared_ptr<vo::AudioMonitor> spAudioMonitor)
{
    using namespace boost::interprocess;

    unsigned int retry = 0;
retry_lock:
    try
    {
        // If using generic default interprocess mutexes:
        //      define BOOST_INTERPROCESS_ENABLE_TIMEOUT_WHEN_LOCKING and his MS counterpart
        //      and check for abandonement using inteprocess_exeption error_code_t::timeout_when_locking_error
        //      assume abandonement, mutex will get locked after throw without chance to reclaim it or 
        //      transfer ownership.
        //      (currently boost lib doesnt support it on generic spin mutex, so we are dead there)
        //
        // If using native windows interprocess mutex: (currently using those)
        //      locking an abandoned mutex will throw error_code_t::owner_dead_error, it will unlock the mutex
        //      after throw so we can reclaim it, but object state will be undefined, we currently use separate
        //      shared mems segments for that, so we have options.
        //
        //  Either way, managed memory internal mutex can get abandoned too, so alloc will throw code 22 using
        //      generic mutex or code 24(owner_dead_error) usign native windows mutex, in that case catch it and
        //      retry, anyone could've abandoned 'm_personal_managed_shm internal managed mem mutex'.
        scoped_lock<interprocess_recursive_mutex> lock(*m_global_rmutex_offset);

        // First check if this device is in any of our processes.
        unsigned long remote_pid;
        if (scan_shared_segments(deviceid, &remote_pid))
        {
            m_tracking_managers_cache[deviceid] = remote_pid;
            // TODO: check if its alive, obtain its message queue name and send a ping message
            return 2;
        }

        void_shm_allocator convertible_void_alloc(m_personal_managed_shm->get_segment_manager());
        auto pair = m_personal_shared_set->get_localoffsetptr()->emplace(deviceid.c_str(), convertible_void_alloc);
        if (pair.second)
        {
            std::lock_guard<std::recursive_mutex> lock_local(m_local_claimed_devices_mutex);
            m_local_claimed_devices[deviceid] = spAudioMonitor;
            return 1;
        }
        else
            return -1;
    }
    catch (interprocess_exception& e)
    {
        // the only catch here is our main mutex gets abandoned (error_code_t::owner_dead_error)
        std::cerr << e.what() << "  ecode: " << e.get_error_code() << "  native_code: "
            << e.get_native_error() << "function" << __FUNCTION__ << std::endl;

        if ((e.get_error_code() == error_code_t::owner_dead_error) && (retry++ <= m_max_retry))
            goto retry_lock;
    }

    return 0;
}

std::shared_ptr<vo::AudioMonitor> DeviceIPCManager::get_audiomonitor_of(const std::wstring& deviceid)
{
    std::lock_guard<std::recursive_mutex> lock_local(m_local_claimed_devices_mutex);

    if (m_local_claimed_devices.count(deviceid))
    {
        std::shared_ptr<vo::AudioMonitor> sp = m_local_claimed_devices[deviceid].lock();
        if (!sp) m_local_claimed_devices.erase(deviceid);
        return sp;
    }

    return nullptr;
}

// NOTE: this is not necessary in current design, i leave this here just in case.
void DeviceIPCManager::clear_local_insertions()
{
    using namespace boost::interprocess;

    unsigned int retry = 0;
    if (m_personal_shared_set)
    {
    retry_lock:
        try
        {
            // serialize writters
            scoped_lock<interprocess_recursive_mutex> lock(*m_global_rmutex_offset);

            std::lock_guard<std::recursive_mutex> lock_local(m_local_claimed_devices_mutex);

            // Delete from shared set what we locally inserted
            void_shm_allocator convertible_void_alloc(m_personal_managed_shm->get_segment_manager());
            for (auto i : m_local_claimed_devices)
            {
                shm_wstring shm_deviceid(i.first.c_str(), convertible_void_alloc);
                m_personal_shared_set->get_localoffsetptr()->erase(shm_deviceid);
            }
            m_local_claimed_devices.clear();
        }
        catch (interprocess_exception& e)
        { // we can get or mutex abandoned or bad alloc here.

            if ((e.get_error_code() == error_code_t::owner_dead_error) && (retry++ <= m_max_retry))
                goto retry_lock;

            // silently return on bad alloc
        }
    }
}




///////////////////////////////////////////////////////////////////////////////////////////////////////
/////////                  VolumeOptions Message Queue comm manager                         ///////////
///////////////////////////////////////////////////////////////////////////////////////////////////////


const std::wstring MessageQueueIPCHandler::vo_response_data_t::OK = L"mq OK";
const std::wstring MessageQueueIPCHandler::vo_response_data_t::FAIL = L"mq FAIL";
const std::wstring MessageQueueIPCHandler::vo_response_data_t::TIMEOUT = L"mq TIMEOUT";



unsigned long MessageQueueIPCHandler::ms_next_message_id = 1;
std::recursive_mutex MessageQueueIPCHandler::ms_static_messageid_gen;

inline unsigned long MessageQueueIPCHandler::get_unused_messageid()
{
    std::lock_guard<std::recursive_mutex> l(ms_static_messageid_gen);
    return ms_next_message_id++;
}



MessageQueueIPCHandler::MessageQueueIPCHandler(boost::interprocess::ipcdetail::OS_process_id_t process_id)
    : m_personal_message_queue_base_name("volumeoptions_win_message_queue-"VO_GUID_STRING)
{
    using namespace boost::interprocess;

    m_process_id = process_id;

    m_personal_message_queue_name =
        m_personal_message_queue_base_name + "-" + std::to_string(process_id);

    create_message_queue_handler(m_personal_message_queue_name);

}

MessageQueueIPCHandler::~MessageQueueIPCHandler()
{
    //TODO: make this less uglier, maybe remove singleton and use a wrapper, or use boost::thread

    // Send mq_abort to message queue to terminate thread and join.
    vo_message_t abort(m_process_id, mq_abort);
    if (send_message(m_personal_message_queue, abort, timedblock, 10))
    {
        // There is a bug with in VS2012-2013 std::thread, if main exists first, join will deadlock
        // This happens if this destructor is called from a static instance when main() is exiting.
        // Solutions:
        //  1 Use boost::thread (adds 50KB to binary, two more libs)
        //  2 Exit MessageQueueIPCHandler::m_thread_personal_mq thread before main returns (uhmm)
        //  3 Detach from thread (need to terminate it if not called from static and wait before detach, uhm)
        //  4 Dont make statics or singletons, (hard to do with a process ipc manager)

       // if (m_thread_personal_mq.joinable())
        //    m_thread_personal_mq.join();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // ugly yep, i need to wait for main to exit first.
    try  {
        m_thread_personal_mq.detach(); // so the destructor will not call terminate causing a exception on process exit.
    }
    catch (const std::system_error) 
    {}

   // m_personal_message_queue.reset();
}


/*
    Creates our endpoint for Process comunication,
    received messages will be handled in separate thrad
*/
bool MessageQueueIPCHandler::create_message_queue_handler(const std::string& name)
{
    using namespace boost::interprocess;

    if (name.empty())
        throw std::exception("create_message_queue_handler name empty");

    try
    {
        // name must be unique
        m_personal_message_queue = std::make_shared<message_queue>
            (create_only                //only create
            , name.c_str()              //name
            , 30                        //max message number
            , sizeof(mq_packet_t)      //max message size
            );

        dprintf("IPC MessageQueue: CREATED - %s \n", name.c_str());

        // Create a thread to handle incoming messages.
        m_thread_personal_mq = std::thread(&MessageQueueIPCHandler::listen_handler, this);
      //  m_thread_personal_mq = boost::thread(&MessageQueueIPCHandler::listen_handler, this);
    }
    catch (interprocess_exception)
    {
        throw;
    }

    return true;
}


inline
unsigned long MessageQueueIPCHandler::send_message(const boost::interprocess::ipcdetail::OS_process_id_t& process_id,
    const vo_message_t& message, const full_policy_t& smode, const int& priority, const unsigned long reply_message_id)
{
    using namespace boost::interprocess;

    std::string mq_destionation_name(m_personal_message_queue_base_name + "-" + std::to_string(process_id));

    return send_message(mq_destionation_name, message, smode, priority, reply_message_id);
}

inline
unsigned long MessageQueueIPCHandler::send_message(const std::string& mq_destionation_name, 
    const vo_message_t& message, const full_policy_t& smode, const int& priority, const unsigned long reply_message_id)
{
    using namespace boost::interprocess;

    std::shared_ptr<message_queue> sp_mq;
    try
    {
        // TODO: cache remote queues. see how, expire etc.
        sp_mq = std::make_shared<message_queue>(open_only, mq_destionation_name.c_str());
        dprintf("IPC MessageQueue: Destination OPENED - %s \n", mq_destionation_name.c_str());
    }
    catch (interprocess_exception)
    {
        return false;
    }

    return send_message(sp_mq, message, smode, priority, reply_message_id);
}

/*
    Returns the messaid used to send the message, or 0 if the message was not sent, buffer full.

    use reply_message_id only when sending acks. TODO: i have to rewrite the senders. or use MPI.

    throws on error.
*/
unsigned long MessageQueueIPCHandler::send_message(std::shared_ptr<boost::interprocess::message_queue>& mq_destination,
    const vo_message_t& message, const full_policy_t& smode, const int& priority, const unsigned long reply_message_id)
{
    using namespace boost::interprocess;

    assert(mq_destination);
    if (!mq_destination)
        return 0;

    // Translate message to mq format
    mq_packet_t mq_packet;
    mq_packet.message_code = message.message_code;
    mq_packet.source_pid = message.source_pid;

    assert(((message.device_id.size() + 1) * sizeof(wchar_t)) <= MQDATASIZE);
    if (((message.device_id.size() + 1) * sizeof(wchar_t)) > MQDATASIZE) return 0; // data too long, throw
    memcpy(mq_packet.buffer, message.device_id.c_str(), (message.device_id.size() + 1)*sizeof(wchar_t));

    // I caugth this debugging, dont call a static mutex on program termination unless you know the init order.
    // Also, if this is a reply(ack), use sender messageid.
    if ((message.message_code != mq_abort) && (reply_message_id == 0))
        mq_packet.message_id = get_unused_messageid();
    else
        mq_packet.message_id = reply_message_id;

    const unsigned int max_send_retry = 5;
    unsigned int retry = 0;
retry_lock:
    try
    {
        bool r = 0;
        switch (smode)
        {
            case trysend:
            {
                r = mq_destination->try_send(&mq_packet, sizeof(mq_packet), priority);
            }
            break;

            case block:
            {
                mq_destination->send(&mq_packet, sizeof(mq_packet), priority);
                r = true;
            }
            break;

            case timedblock: // (fixed) 1 second
            {
                boost::posix_time::ptime wait_time
                    = boost::posix_time::microsec_clock::universal_time() // use universal time on windows.
                    + boost::posix_time::milliseconds(1000);
                r = mq_destination->timed_send(&mq_packet, sizeof(mq_packet), priority, wait_time);
            }
            break;

            default:
                return 0;
        }

        if (r)
            return mq_packet.message_id;
        else
            return 0;
    }
    catch (boost::interprocess::interprocess_exception& e)
    {
        if ((e.get_error_code() == error_code_t::timeout_when_locking_error) // for interprocess mutexes
            || (e.get_error_code() == error_code_t::timeout_when_waiting_error) // for interprocess conditions
            || (e.get_error_code() == error_code_t::owner_dead_error)) // for windows native mutexes (boost 1.57)
        {
            std::cerr << e.what() << "  ecode: " << e.get_error_code() << "  native_code: "
                << e.get_native_error() << "function" << __FUNCTION__ << std::endl;

            // retry lock access if a windows native mutex got abandoned (it can be reclaimed now)
            if ((e.get_error_code() == error_code_t::owner_dead_error)
                && (retry++ <= max_send_retry))
                goto retry_lock;

            if (retry >= max_send_retry)
                throw;
        }
    }

    return 0;
}

/*
    Wait for a response to a message_id packet sent.

    It will wait short milliseconds for a response (this is shared memory)
*/
std::wstring MessageQueueIPCHandler::wait_for_reply(const unsigned long message_id)
{
    std::unique_lock<std::mutex> l(m_receive_buffer_mutex);

    while (!m_receive_buffer.count(message_id))
    {
        if (m_receive_buffer_cond.wait_for(l, std::chrono::milliseconds(30)) == std::cv_status::timeout)
            return vo_response_data_t::TIMEOUT;
        dprintf("IPC wait_for_reply signaled\n");
    }

    std::wstring r(reinterpret_cast<wchar_t*>(m_receive_buffer[message_id].buffer));

    m_receive_buffer.erase(message_id);

    return r;
}

void MessageQueueIPCHandler::store_response(MessageQueueIPCHandler::mq_packet_t r)
{
    if (r.buffer[0] != '\0')
    {
        std::lock_guard<std::mutex> l(m_receive_buffer_mutex);

        if (m_receive_buffer.size() > 64)
            m_receive_buffer.clear(); // reset, this shouldnt happen, im tired too.

        m_receive_buffer[r.message_id] = r;

        m_receive_buffer_cond.notify_all();
    }

}

void MessageQueueIPCHandler::listen_handler()
{
    using namespace boost::interprocess;

    assert(m_personal_message_queue.get());

    dprintf("IPC MessageQueue: Receive thread running...\n");

    mq_packet_t message;
    unsigned int priority;
    message_queue::size_type recvd_size;
    const message_queue::size_type max_size = m_personal_message_queue->get_max_msg_size();

    bool abort = false;
    while (!abort)
    {
        unsigned int retry = 0;
    retry_lock:
        try
        {
            m_personal_message_queue->receive(&message, sizeof(message), recvd_size, priority);
            message.buffer[MQDATASIZE - 1] = '\0';
        }
        catch (boost::interprocess::interprocess_exception& e)
        {
            if ((e.get_error_code() == error_code_t::timeout_when_locking_error) // for interprocess mutexes
                || (e.get_error_code() == error_code_t::timeout_when_waiting_error) // for interprocess conditions
                || (e.get_error_code() == error_code_t::owner_dead_error)) // for windows native mutexes (boost 1.57)
            {
                std::cerr << e.what() << "  ecode: " << e.get_error_code() << "  native_code: "
                    << e.get_native_error() << "function" << __FUNCTION__ << std::endl;

                // retry set access if a windows native mutex got abandoned (it can be reclaimed now)
                if ((e.get_error_code() == error_code_t::owner_dead_error)
                    && (retry++ <= m_max_retry))
                    goto retry_lock;

                if (retry >= m_max_retry)
                    abort = true;
            }
            else
                abort = true; // TODO report error.
        }

        dprintf("Received message: source_pid: %u ", message.source_pid);
        dprintf("message_code: %u ", message.message_code);
        dwprintf(L"string: %s  (message_id: %u size: %u  priority: %d)\n",
            message.buffer, message.message_id, recvd_size, priority);

        if (recvd_size != max_size) // malformed
            continue;

        std::wstring reply;

        switch (message.message_code)
        {
            case mq_test:
            {
                reply = L"mq_test OK";
            }
            break;

            case mq_ping:
                reply = L"mq_ping OK";
                break;

            case mq_ack: // a response to a message sent.
            {
                store_response(message);
            }
            break;

            case mq_wasapi_start:
            {
                std::wstring deviceid(reinterpret_cast<wchar_t*>(message.buffer));
                std::shared_ptr<vo::AudioMonitor> sp;
                if (sp = DeviceIPCManager::get().get_audiomonitor_of(deviceid))
                {
                    sp->Start(); // TODO do a stack, use other method
                    reply = vo_response_data_t::OK;
                }
                else
                    reply = vo_response_data_t::FAIL;
            }
            break;

            case mq_wasapi_pause:
            {
                std::wstring deviceid(reinterpret_cast<wchar_t*>(message.buffer));
                std::shared_ptr<vo::AudioMonitor> sp;
                if (sp = DeviceIPCManager::get().get_audiomonitor_of(deviceid))
                {
                    sp->Pause(); // TODO do a stack, use other method
                    reply = vo_response_data_t::OK;
                }
                else
                    reply = vo_response_data_t::FAIL;
            }
            break;

            case mq_abort:
                if (message.source_pid == m_process_id)
                    abort = true;
            break;

            default:
                ;

        } // end switch

        if (!reply.empty())
        {
            // send reply to sender, code:ack, use original message id
            vo_message_t ackpacket(m_process_id, mq_ack, reply);
            send_message(message.source_pid, ackpacket, timedblock, 5, message.message_id);
        }

    } // end while

    // if this thread terminates, remove it from pid table. dah this needs rewrite.
    DeviceIPCManager::get().pid_table<DeviceIPCManager::pid_lookup_table_modes_t::pid_remove>
        (DeviceIPCManager::get().m_global_shared_pidtable, m_process_id);

    dprintf("IPC: Closing listen handler thread...\n");
}


} // end namespace ::ipc::win




///////////////////////////////////////////////////////////////////////////////////////////////////////
/////////                  Generic Windows Shared Managed Containers                        ///////////
///////////////////////////////////////////////////////////////////////////////////////////////////////


/*
    Creates or opens a set of wstrings in managed shared memory, if created, the name of the set will have 
      set_name as name.

    InterProcess thread safe, local process NOT thread safe.

    We can obtain our local pointer to the set using  get_localoffsetptr()

    throws boost::interprocess::interprocess_exception on error. (bad alloc or abandoned mutex code 22, or 24)
*/
SharedWStringSet::SharedWStringSet(std::shared_ptr<boost::interprocess::managed_windows_shared_memory> sp_managed_shm,
    const std::string& set_name)
    : m_we_created_set(false)
{
    using namespace boost::interprocess;

    assert(sp_managed_shm);
    assert(sp_managed_shm->get_size() >= 64 * 1024);

    m_device_set_name = set_name;
    try
    {
        // Create a set in shared memory, we need to know if we created or found it, do it atomically
        auto atomic_construct = std::bind(&SharedWStringSet::construct_objects_atomic, this, sp_managed_shm);
        // NOTE: the internal mutex of this method can get abandoned too, let it throw back to our DeviceIPCManager
        sp_managed_shm->atomic_func(atomic_construct);
    }
    catch (boost::interprocess::bad_alloc &e)
    {
        std::cerr << "managed shared mememory bad_alloc " << e.what() << '\n';
        throw;
    }

    if (m_we_created_set)
    {
        dprintf("IPC Set: sucessfully constructed: %s.\n", m_device_set_name.c_str());
    }
}

/*
    boost::interprocess managed memory internal mutex can get abandoned here....
        nothing we can do exept use individual shared memory segments and discard the segment
        if boost::interprocess throws mutex abanadoned like error (code 22, 24).
*/
void SharedWStringSet::construct_objects_atomic(
    std::shared_ptr<boost::interprocess::managed_windows_shared_memory> sp_managed_shm)
{
    using namespace boost::interprocess;

    if (!sp_managed_shm) // this shouldn't happen
        throw std::exception("construct_objects_atomic: managed shared memory pointer null");

    // An allocator convertible to any allocator<T, segment_manager_t> type
    void_shm_allocator convertible_void_alloc(sp_managed_shm->get_segment_manager());

    // Try to find first if we are creating it or referencing it, atomic
    try
    {
        // Construct our sharedmem compatible set inside our managed sharedmem with a name.
        m_set_wstring_offset = sp_managed_shm->construct<shm_wstring_set>
            //(object name), (first ctor parameter, second ctor parameter)
            (m_device_set_name.c_str())(std::less<shm_wstring>(), convertible_void_alloc); // <wstringallocator>
    }
    // NOTE: boost code doc is wrong, throws if already exists, it doesnt return 0 as it says in code.
    catch (interprocess_exception& e)
    {
        if (e.get_error_code() == error_code_t::already_exists_error)
        {
            // if the set already exists, find it
            if (!m_set_wstring_offset)
            {
                m_set_wstring_offset = sp_managed_shm->find<shm_wstring_set>
                    (m_device_set_name.c_str()).first;
            }
            else
                m_we_created_set = true;
        }
        else
            throw;
    }
}

SharedWStringSet::~SharedWStringSet()
{}

shm_wstring_set* SharedWStringSet::get_localoffsetptr()
{
    return m_set_wstring_offset;
}

std::string SharedWStringSet::get_set_name() const
{
    return m_device_set_name;
}

bool SharedWStringSet::we_created_this() const
{
    return m_we_created_set;
}


} // end namespace ipc
} // end namespace vo
