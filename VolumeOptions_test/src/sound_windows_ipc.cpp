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

#include <boost/interprocess/sync/scoped_lock.hpp>


namespace vo {
namespace ipc {
namespace win {

    ///////////////////////////////////////////////////////////////////////////////////////////////////////
    /////////                  VolumeOptions Wasapi shared memory manager                       ///////////
    ///////////////////////////////////////////////////////////////////////////////////////////////////////

/*
    See notes on header to understand all this.

    throws boost::interproces::interprocess_exception on managed segment creation error.
*/
WasapiSharedManager::WasapiSharedManager()
    : m_max_slots(50)
{
    using namespace boost::interprocess;

    // We use windows managed shared mem in this case

    // SELFNOTE: make sure is big enogh, resizing is not easy.
    //
    // Pages in windows are typicaly 4k bytes 'dwPageSize' (mapped_region::get_page_size() is wrong about "Page size", 
    //  it not the same as 'dwAllocationGranularity' "Page granularity" ).
    //  dwAllocationGranularity is "The granularity with which virtual memory is allocated". used by VirtualAlloc.
    // either way, 128KiB is safe to have room, dwAllocationGranularity is typically 64KiB
    const offset_t block_size_personal_shared_segment = 128 * 1024;
    const offset_t block_size_unique_shared_segment = 64 * 1024;

    try
    {
        // OpenCreate the unique shared memory segment for VolumeOptions.    // throws on error
        open_create_managed_smem(m_managed_global_shared_memory_name, block_size_unique_shared_segment);

        // Find a free slot number for our personal windows shared memory. 
        //  (we will be the writer as explained in header)      // throws on error
        if (!create_free_managed_smem(m_managed_personal_shared_memory_base_name, block_size_personal_shared_segment))
            throw std::exception("no more free slots"); // TODO error codes.

        // Instance the unique global mutex for VolumeOptions.
        m_global_sharedset_rmutex_offset = m_global_managed_shm->find_or_construct<interprocess_recursive_mutex>
            (m_interp_recursive_mutex_name.c_str())();


        // --------- Now instantiate our objects: ---------

        // Finally, open or create our personal shared set (1 writer (only us), multiple readers)
        m_personal_up_devices_set = std::make_unique<SharedWStringSet>
        //   where to search/place it           name for this set (it will open-create it)
            (m_personal_managed_shm,            m_managed_personal_devices_set_name);
    }
    catch (interprocess_exception& e)
    {
        std::cerr << e.what() << "  ecode: " << e.get_error_code() << "  native_code: "
            << e.get_native_error() << std::endl;
        throw;
    }

}

WasapiSharedManager::~WasapiSharedManager()
{
    using namespace boost::interprocess;

    // unlink our resources inside the shared mem first, before destructing our shared mem.
    m_personal_up_devices_set.reset();

}

std::string WasapiSharedManager::get_personal_opened_manager_name()
{
    return m_opened_personal_managed_sm_name;
}

std::shared_ptr<boost::interprocess::managed_windows_shared_memory>
    WasapiSharedManager::get_personal_shared_mem_manager()
{
    return m_personal_managed_shm;
}

std::shared_ptr<boost::interprocess::managed_windows_shared_memory>
WasapiSharedManager::get_global_shared_mem_manager()
{
    return m_global_managed_shm;
}

/*
    Opens or creates a named managed shared memory block

    throws boost::interprocess::interprocess_exception  on error.
*/
void WasapiSharedManager::open_create_managed_smem(const std::string& name, boost::interprocess::offset_t block_size)
{
    using namespace boost::interprocess;

    try
    {
        m_global_managed_shm.reset(new managed_windows_shared_memory(open_only, name.c_str()));
        dprintf("IPC SharedMem: OPENED - %s \n", name.c_str());
    }
    catch (interprocess_exception)
    {
        try
        {
            //std::cerr << e.what() << "    ->  Creating a new managed_sharedmem then."<< '\n';
            m_global_managed_shm.reset(
                new managed_windows_shared_memory(open_or_create, name.c_str(), block_size));
            dprintf("IPC SharedMem: OPENED-CREATED - %s \n", name.c_str());
        }
        catch (interprocess_exception)
        {
            throw;
        }
    }
}

/*
    Finds a free name using numbers from 0 to m_max_slots as sufix for base_name and creates
        a managed shared memory block there.

    Returns true if a slot was found and shared mem was created
    Returns false if no slots free, shared mem not created.
    throws boost::interprocess::interprocess_exception on not handled error.
*/
bool WasapiSharedManager::create_free_managed_smem(const std::string& base_name, boost::interprocess::offset_t block_size)
{
    using namespace boost::interprocess;

    std::string test_name;
    for (unsigned short free_slot = 0; free_slot != m_max_slots; free_slot++)
    {
        unsigned int retry = 0;
        retry_access:
        try
        {
            test_name = base_name + "-" + std::to_string(free_slot);
            m_personal_managed_shm.reset(
                new managed_windows_shared_memory(create_only, test_name.c_str(), block_size));
            dprintf("IPC SharedMem: CREATED - %s\n", test_name.c_str());

            m_opened_personal_managed_sm_name = test_name;
            break;
        }
        catch (boost::interprocess::interprocess_exception& e)
        {
            if (e.get_error_code() == error_code_t::already_exists_error)
                continue;

            if ((e.get_error_code() == error_code_t::owner_dead_error) && (retry++ <= m_max_retry))
                goto retry_access;

            if (free_slot == m_max_slots)
                return false;

            // else throw error.
            throw;
        }
    }

    assert(block_size == m_personal_managed_shm->get_size());

    return true;
}

/*
    Here comes all the uglyness described on header, at least it works better on windows.

    Search all individual shared memory blocks belonging to other processes and search the set for a matching
        wstring, this operation is atomic. (it needs to be)

    Return true if found in any process.

    NOTE: we take advange of the fact that unreferenced shared memory on windows gets deleted, so we dont have to
        worry about corrupted objects etc. more info on header.
        Abanadoned mutexes are now handled internally with a retry count to reclaim it, if it cant be reclaimed it
            returns false.
*/
bool WasapiSharedManager::find_device(const std::wstring& deviceid)
{
    using namespace boost::interprocess;

    unsigned int retry = 0;
retry_lock:
    vo::high_resolution_clock::time_point now = vo::high_resolution_clock::now(); // TODO remove this line
    try
    {
        scoped_lock<interprocess_recursive_mutex> lock(*m_global_sharedset_rmutex_offset);

        void_shm_allocator convertible_void_alloc(m_personal_managed_shm->get_segment_manager());

        std::string sm_test_name;
        // Search for opened blocks of shared memory (ugly yep, but effective)
        for (unsigned short free_slot = 0; free_slot != m_max_slots; free_slot++)
        {
            unsigned int retry_set_access = 0;
            retrysetaccess:
            try
            {
                std::unique_ptr<SharedWStringSet> upset;
                std::shared_ptr<managed_windows_shared_memory> spmsm;
                SharedWStringSet* pset_offset = nullptr;

                // yeah... i know.
                sm_test_name = m_managed_personal_shared_memory_base_name + "-" + std::to_string(free_slot);
                if (sm_test_name != m_opened_personal_managed_sm_name)
                {
                    // try this name
                    spmsm.reset((new managed_windows_shared_memory(open_only, sm_test_name.c_str())));
                    // opens the set if it exists in this managed memory, if not, its created empty.
                    // it can throw if a mutex is abandoned inside.
                    upset = std::make_unique<SharedWStringSet>(spmsm, m_managed_personal_devices_set_name);

                    pset_offset = upset.get();
                }
                else // its ours, already opened.
                    pset_offset = m_personal_up_devices_set.get();

                // Finally checks if its there, if not, continue searching the next slot.
                if (pset_offset->get_localoffsetptr()->count(shm_wstring(deviceid.c_str(), convertible_void_alloc)))
                {
                    dwprintf(L"Found %s", deviceid.c_str()); dprintf(" in %s\n", sm_test_name.c_str());
                    return true;
                }
            }
            catch (boost::interprocess::interprocess_exception& e)
            {
                if ((e.get_error_code() == error_code_t::timeout_when_locking_error) // for interprocess mutexes
                    || (e.get_error_code() == error_code_t::timeout_when_waiting_error) // for interprocess conditions
                    || (e.get_error_code() == error_code_t::owner_dead_error)) // for windows native mutexes (boost 1.57)
                {
                    std::cerr << e.what() << "  ecode: " << e.get_error_code() << "  native_code: "
                        << e.get_native_error() << std::endl;

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
            << e.get_native_error() << std::endl;

        if ((e.get_error_code() == error_code_t::owner_dead_error) && (retry++ <= m_max_retry))
            goto retry_lock;

        // if (e.get_error_code() == error_code_t::owner_dead_error) throw;
    }

    vo::high_resolution_clock::time_point later = vo::high_resolution_clock::now(); // TODO remove
    printf("find took: %llu microsec\n", std::chrono::duration_cast<std::chrono::microseconds>(later - now).count());

    return false;
}

bool WasapiSharedManager::remove_device(const std::wstring& deviceid)
{
    using namespace boost::interprocess;

    unsigned int retry = 0;
retry_lock:
    try
    {
        // *see set_device comment.
        scoped_lock<interprocess_recursive_mutex> lock(*m_global_sharedset_rmutex_offset);

        void_shm_allocator convertible_void_alloc(m_personal_managed_shm->get_segment_manager());
        shm_wstring shm_deviceid(deviceid.c_str(), convertible_void_alloc);

        size_t sz = m_personal_up_devices_set->get_localoffsetptr()->erase(shm_deviceid);
        if (sz)
        {
            m_local_insertions.erase(deviceid);
            return true;
        }
    }
    catch (interprocess_exception& e)
    {
        // the only catch here is our main mutex gets abandoned (error_code_t::owner_dead_error)
        std::cerr << e.what() << "  ecode: " << e.get_error_code() << "  native_code: "
            << e.get_native_error() << std::endl;

        if ((e.get_error_code() == error_code_t::owner_dead_error) && (retry++ <= m_max_retry))
            goto retry_lock; // instead of caling the same function recursive..
    }

    return false;
}

bool WasapiSharedManager::set_device(const std::wstring& deviceid)
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
        //      (currently boost lib doesnt support it on generic mutex, so we are dead there)
        //
        // If using native windows interprocess mutex: (currently using those)
        //      locking an abandoned mutex will throw error_code_t::owner_dead_error, it will unlock the mutex
        //      after throw so we can reclaim it, but object state will be undefined, we currently use separate
        //      shared mems segments for that, so we have options.
        //
        //  Either way, managed memory internal mutex can get abandoned too, so alloc will throw code 22 using
        //      generic mutex or code 24(owner_dead_error) usign native windows mutex, in that case catch it and
        //      retry.
        scoped_lock<interprocess_recursive_mutex> lock(*m_global_sharedset_rmutex_offset);

        // First check if this device is in any of our processes.
        if (find_device(deviceid))
            return false;

        void_shm_allocator convertible_void_alloc(m_personal_managed_shm->get_segment_manager());
        auto pair = m_personal_up_devices_set->get_localoffsetptr()->emplace(deviceid.c_str(), convertible_void_alloc);
        if (pair.second) m_local_insertions.insert(deviceid);

        return pair.second;
    }
    catch (interprocess_exception& e)
    {
        // the only catch here is our main mutex gets abandoned (error_code_t::owner_dead_error)
        std::cerr << e.what() << "  ecode: " << e.get_error_code() << "  native_code: "
            << e.get_native_error() << std::endl;

        if ((e.get_error_code() == error_code_t::owner_dead_error) && (retry++ <= m_max_retry))
            goto retry_lock;
    }

    return false;
}

void WasapiSharedManager::clear_local_insertions()
{
    using namespace boost::interprocess;

    unsigned int retry = 0;
    if (m_personal_up_devices_set)
    {
    retry_lock:
        try
        {
            scoped_lock<interprocess_recursive_mutex> lock(*m_global_sharedset_rmutex_offset); //(make this atomic)

            // Delete what we locally inserted
            for (auto i : m_local_insertions)
                remove_device(i);
        }
        catch (interprocess_exception& e)
        { // we can get or mutex abandoned or bad alloc here.

            if ((e.get_error_code() == error_code_t::owner_dead_error) && (retry++ <= m_max_retry))
                goto retry_lock;

            // silently abort on bad alloc
        }
    }
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
    assert(sp_managed_shm->get_size() >= 128 * 1024);

    m_device_set_name = set_name;
    try
    {
        // Create a set in shared memory, we need to know if we created or found it, do it atomically
        auto atomic_construct = std::bind(&SharedWStringSet::construct_objects_atomic, this, sp_managed_shm);
        // NOTE: the internal mutex of this method can get abandoned too, let it throw back to our WasapiSharedManager
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


#if 0
void SharedWStringSet::clear_local_insertions()
{
    using namespace boost::interprocess;

    std::shared_ptr<managed_windows_shared_memory> sp_managed_shm = m_wp_managed_shm.lock();
    if (!sp_managed_shm)
        return;

    int retry_limit = 1, retry = 0;

    if (m_set_wstring_offset)
    {
    retry_tag:
        try
        {
            scoped_lock<interprocess_recursive_mutex> lock(*m_rmutex_offset); //(make this atomic)

            // Delete what we locally inserted
            for (auto i : m_local_insertions)
                erase(i);
        }
        catch (interprocess_exception& e)
        { // we can get or mutex abandoned or bad alloc here.

            if ((e.get_error_code() == error_code_t::owner_dead_error) && (retry++ <= retry_limit))
                goto retry_tag;

            // silently abort on bad alloc
        }
    }
}
#endif

} // end namespace ipc
} // end namespace vo
