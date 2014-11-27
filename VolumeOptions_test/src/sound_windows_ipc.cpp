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

/*
    See notes on header to understand all this.

    throws boost::interproces::interprocess_exception on managed segment creation error.
*/
SharedMemoryManager::SharedMemoryManager()
    : m_max_slots(50)
{
    using namespace boost::interprocess;

    // Use windows managed shared mem in this case
    // Creates our "windows" managed shared segment.

    // SELFNOTE: make sure is big enogh, resizing is not easy.
    //
    // Pages in windows are typicaly 4k bytes 'dwPageSize' (mapped_region::get_page_size() is wrong about "Page size", 
    //  it not the same as 'dwAllocationGranularity' "Page granularity" ).
    //  dwAllocationGranularity is "The granularity with which virtual memory is allocated". used by VirtualAlloc.
    // either way, 128KiB is safe to have room, dwAllocationGranularity is typically 64KiB
    offset_t block_size = 128 * 1024;

    // Find a free slot number for our personal windows shared memory. (we will be the writer as explained in header)
    try
    {
        if (!create_free_managed_smem(m_managed_shared_memory_base_name, block_size)) // throws on error
            throw std::exception("no more free slots"); // TODO error codes.

        m_rmutex = m_managed_shm->find_or_construct<interprocess_recursive_mutex>
            (m_interp_recursive_mutex_name.c_str())();

        // Finally, create our set
        m_msm_up_devices_set = std::make_unique<SharedStringSet>(m_managed_shm, m_opened_managed_sm_name);
    }
    catch (interprocess_exception& e)
    {
        std::cerr << e.what() << '\n';
        throw;
    }

}

SharedMemoryManager::~SharedMemoryManager()
{
    using namespace boost::interprocess;

    // unlink our resources inside the shared mem first, before destructing our shared mem.
    m_msm_up_devices_set.reset();

}

std::string SharedMemoryManager::get_manager_name()
{
    return m_opened_managed_sm_name;
}

std::shared_ptr<boost::interprocess::managed_windows_shared_memory> SharedMemoryManager::get_manager()
{
    return m_managed_shm;
}

/*
    Opens or created a named managed shared memory block, [Not used on this design]

    throws boost::interprocess::interprocess_exception  on error.
*/
void SharedMemoryManager::open_create_managed_smem(const std::string& name, boost::interprocess::offset_t block_size)
{
    using namespace boost::interprocess;

    try
    {
        m_managed_shm.reset(new managed_windows_shared_memory(open_only, name.c_str()));
        dprintf("IPC SharedMem: OPENED - %s \n", name.c_str());
    }
    catch (interprocess_exception)
    {
        try
        {
            //std::cerr << e.what() << "    ->  Creating a new managed_sharedmem then."<< '\n';
            m_managed_shm.reset(
                new managed_windows_shared_memory(open_or_create, name.c_str(), block_size));
            dprintf("IPC SharedMem: OPENED-CREATED - %s \n", name.c_str());
        }
        catch (interprocess_exception& e)
        {
            std::cerr << e.what() << '\n';
            throw;
        }
    }

    m_opened_managed_sm_name = name;
}

/*
    Finds a free name using numbers from 0 to m_max_slots as sufix for base_name and creates
        a managed shared memory block there.

    Returns true if a slot was found and shared mem was created
    Returns false if no slots free, shared mem not created.
    throws boost::interprocess::interprocess_exception on not handled error.
*/
bool SharedMemoryManager::create_free_managed_smem(const std::string& base_name, boost::interprocess::offset_t block_size)
{
    using namespace boost::interprocess;

    std::string test_name;
    for (unsigned short free_slot = 0; free_slot != m_max_slots; free_slot++)
    {
        try
        {
            test_name = base_name + "-" + std::to_string(free_slot);
            m_managed_shm.reset(
                new managed_windows_shared_memory(create_only, test_name.c_str(), block_size));
            dprintf("IPC SharedMem: CREATED - %s\n", test_name.c_str());

            m_opened_managed_sm_name = test_name;
            break;
        }
        catch (boost::interprocess::interprocess_exception& e)
        {
            if (e.get_error_code() == error_code_t::already_exists_error)
                continue;

            if (free_slot == m_max_slots)
            {
                std::cerr << e.what() << '\n';
                return false;
            }

            // else throw error.
            throw;
        }
    }

    assert(block_size == m_managed_shm->get_size());

    return true;
}

/*
    Here comes all the uglyness described on header, at least it works better on windows.

    Search all individual shared memory blocks belonging to other processes and search the set for a matching
        wstring.

    Return true if found in any process.

    NOTE: we take advange of the fact that unreferenced shared memory on windows gets deleted, so we dont have to
        worry about abandoned mutexes or corrupted objects etc. more info on header.
*/
bool SharedMemoryManager::find_device(const std::wstring& deviceid)
{
    using namespace boost::interprocess;

    unsigned int retry = 0;
retry:
    vo::high_resolution_clock::time_point now = vo::high_resolution_clock::now();

    try
    {
        scoped_lock<interprocess_recursive_mutex> lock(*m_rmutex);

        std::string sm_test_name;
        // Search for opened blocks of shared memory (ugly yep, but effective)
        for (unsigned short free_slot = 0; free_slot != m_max_slots; free_slot++)
        {
            try
            {
                std::unique_ptr<SharedStringSet> upset;
                std::shared_ptr<managed_windows_shared_memory> spmsm;
                SharedStringSet* pset_offset = nullptr;

                // yeah... i know.
                sm_test_name = m_managed_shared_memory_base_name + "-" + std::to_string(free_slot);
                if (sm_test_name != m_opened_managed_sm_name)
                {
                    // try this name
                    spmsm.reset((new managed_windows_shared_memory(open_only, sm_test_name.c_str())));
                    // opens the set if it exists in this managed memory, if not, its created empty.
                    // it can throw if a mutex is abandoned inside.
                    upset = std::make_unique<SharedStringSet>(spmsm, sm_test_name.c_str());

                    pset_offset = upset.get();
                }
                else // its ours, already opened.
                    pset_offset = m_msm_up_devices_set.get();

                if (pset_offset->exists(deviceid))
                {
                    dwprintf(L"Found %s", deviceid.c_str());
                    dprintf(" AT %s\n", sm_test_name.c_str());
                    return true;
                }
            }
            catch (boost::interprocess::interprocess_exception& e)
            {
                if ((e.get_error_code() == error_code_t::timeout_when_locking_error) // for interprocess mutexes
                    || (e.get_error_code() == error_code_t::timeout_when_waiting_error) // for interprocess conditions
                    || (e.get_error_code() == error_code_t::owner_dead_error)) // for windows native mutexes (boost 1.57)
                {
                    std::cerr << e.what() << std::endl;
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
            goto retry; // instead of caling the same function recursive..
    }

    vo::high_resolution_clock::time_point later = vo::high_resolution_clock::now();
    dprintf("find took: %llu microsec\n", std::chrono::duration_cast<std::chrono::microseconds>(later - now).count());

    return false;
}

bool SharedMemoryManager::remove_device(const std::wstring& deviceid)
{
    using namespace boost::interprocess;

    unsigned int retry = 0;
retry:
    try
    {
        scoped_lock<interprocess_recursive_mutex> lock(*m_rmutex);

        return m_msm_up_devices_set->erase(deviceid);
    }
    catch (interprocess_exception& e)
    {
        // the only catch here is our main mutex gets abandoned (error_code_t::owner_dead_error)
        std::cerr << e.what() << "  ecode: " << e.get_error_code() << "  native_code: "
            << e.get_native_error() << std::endl;

        if ((e.get_error_code() == error_code_t::owner_dead_error) && (retry++ <= m_max_retry))
            goto retry; // instead of caling the same function recursive..
    }

    return false;
}

bool SharedMemoryManager::set_device(const std::wstring& deviceid)
{
    using namespace boost::interprocess;

    unsigned int retry = 0;
retry:
    try
    {
        scoped_lock<interprocess_recursive_mutex> lock(*m_rmutex);

        if (find_device(deviceid))
            return false;

        return m_msm_up_devices_set->insert(deviceid);
    }
    catch (interprocess_exception& e)
    {
        // the only catch here is our main mutex gets abandoned (error_code_t::owner_dead_error)
        std::cerr << e.what() << "  ecode: " << e.get_error_code() << "  native_code: "
            << e.get_native_error() << std::endl;

        if ((e.get_error_code() == error_code_t::owner_dead_error) && (retry++ <= m_max_retry))
            goto retry; // instead of caling the same function recursive..
    }

    return false;
}

} // end namespace ::ipc::win











/*
    Creates or opens a set of wstrings in managed shared memory, the name of the set will have 
        _prefix_name as prefix.

    InterProcess threads safe , local threads NOT safe.

    Destruction will delete locally inserted elements only.

    NOTE: is intended to use the name of managed shared memory as _prefix_name.

    throws boost::interprocess::interprocess_exception on error. (bad alloc or abandoned mutex code 22)
*/
SharedStringSet::SharedStringSet(std::shared_ptr<boost::interprocess::managed_windows_shared_memory> sp_managed_shm,
    const std::string& _prefix_name)
    : m_wp_managed_shm(sp_managed_shm)
    , m_we_created_set(false)
    , m_convertible_void_alloc(sp_managed_shm ? sp_managed_shm->get_segment_manager() : 0)
{
    using namespace boost::interprocess;

    assert(sp_managed_shm);

    offset_t block_size = sp_managed_shm->get_size();

    m_device_set_name = _prefix_name + "-device_set";
    m_imutex_name = _prefix_name + "-imtx";
    m_icond_name = _prefix_name + "-icond";
    try
    {
        // Create a set in shared memory, we need to know if we created or found it, do it atomically
        auto atomic_construct = std::bind(&SharedStringSet::construct_objects_atomic, this, sp_managed_shm);
        // NOTE: the internal mutex of this method can get abandoned too, let it throw back to our SharedMemoryManager
        sp_managed_shm->atomic_func(atomic_construct);

        // NOTE: care use different names! or will compile a run with silent problems on windows.
        // Construct anonymous sync primitives inside our managed "windows" sharedmem, not outside 
        //      or we'll have to delete it.
        m_msmp_mutex = sp_managed_shm->find_or_construct<interprocess_recursive_mutex>(m_imutex_name.c_str())();
        //m_msmp_cond = sp_managed_shm->find_or_construct<interprocess_condition>(m_icond_name.c_str())();
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
void SharedStringSet::construct_objects_atomic(
    std::shared_ptr<boost::interprocess::managed_windows_shared_memory> sp_managed_shm)
{
    using namespace boost::interprocess;

    if (!sp_managed_shm) // this shouldn't happen
        throw std::exception("construct_objects_atomic: managed shared memory pointer null");

    // Try to find first if we are creating it or referencing it, atomic
    try
    {
        // Construct our sharedmem compatible set inside our managed sharedmem with a name.
        m_set_wstring_offset = sp_managed_shm->construct<shm_wstring_set>
            //(object name), (first ctor parameter, second ctor parameter)
            (m_device_set_name.c_str())(std::less<shm_wstring>(), m_convertible_void_alloc); // <wstringallocator>

        // NOTE: Doc says it returns 0 if exists, either way i leave a copy of the handle code here too.
        // if the set already exists, find it, we assume no errors here.
        if (!m_set_wstring_offset)
        {
            m_set_wstring_offset = sp_managed_shm->find<shm_wstring_set>
                (m_device_set_name.c_str()).first;
        }
        else
            m_we_created_set = true;
    }
    // NOTE: boost doc is wrong, throws if already exists, it doesnt return 0 as it says in code.
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

SharedStringSet::~SharedStringSet()
{
    //! not outside: named_mutex::remove("mtx-GUID");
    //! not outside: named_condition::remove("cnd-GUID");
    using namespace boost::interprocess;

    std::shared_ptr<managed_windows_shared_memory> sp_managed_shm = m_wp_managed_shm.lock();
    if (!sp_managed_shm)
        return;

    if (m_set_wstring_offset)
    {
        //scoped_lock<interprocess_recursive_mutex> lock(*m_msmp_mutex); //(make this atomic)

        // Delete what we locally inserted
        for (auto i : m_local_device_count)
            erase(i);
    }

    // Dont delete mutex, we dont know who is goign to use it
}

bool SharedStringSet::insert(const std::wstring& deviceid)
{
    using namespace boost::interprocess;

    std::shared_ptr<managed_windows_shared_memory> m_managed_shm = m_wp_managed_shm.lock();
    if (!m_managed_shm) return false;

    // If using generic default interprocess mutexes:
    //      define BOOST_INTERPROCESS_ENABLE_TIMEOUT_WHEN_LOCKING and his MS counterpart
    //      and check for abandonement using inteprocess_exeption error_code_t::timeout_when_locking_error
    //      assume abandonement, mutex will get locked after throw without chance to reclaim it or transfer ownership.
    //      (currently boost lib doesnt support it on generic mutex)
    //
    // If using native windows interprocess mutex: (currently using those)
    //      locking an abandoned mutex will throw error_code_t::owner_dead_error, it will unlock the mutex
    //      after throw so we can reclaim it, but object state will be undefined, we currently use separate
    //      shared mems segments for that, so we have options.
    //
    //  Either way, managed memory internal mutex can get abandoned too, so alloc will throw code 22 using
    //      generic mutex or code 24(owner_dead_error) usign native windows mutex, in that case let
    //      throw unchain back to our SharedMemoryManager.
    scoped_lock<interprocess_recursive_mutex> lock(*m_msmp_mutex);

    try
    {
        m_set_wstring_offset->emplace(deviceid.c_str(), m_convertible_void_alloc);
        m_local_device_count.insert(deviceid);
    }
    catch (boost::interprocess::bad_alloc &e)
    {
        std::cerr << "managed shared mememory bad_alloc " << e.what() << '\n';
        return false;
    }

    return true;
}

bool SharedStringSet::erase(const std::wstring& deviceid)
{
    using namespace boost::interprocess;

    std::shared_ptr<managed_windows_shared_memory> m_managed_shm = m_wp_managed_shm.lock();
    if (!m_managed_shm) return false;

    // *See insert comment.
    scoped_lock<interprocess_recursive_mutex> lock(*m_msmp_mutex);

    try
    {
        shm_wstring shm_deviceid(deviceid.c_str(), m_convertible_void_alloc);

        size_t sz = m_set_wstring_offset->erase(shm_deviceid);
        if (sz)
        {
            return true;
        }
    }
    catch (boost::interprocess::bad_alloc &e)
    {
        std::cerr << "managed shared mememory bad_alloc " << e.what() << '\n';
    }

    return false;
}

bool SharedStringSet::exists(const std::wstring& deviceid) const
{
    using namespace boost::interprocess;

    std::shared_ptr<managed_windows_shared_memory> m_managed_shm = m_wp_managed_shm.lock();
    if (!m_managed_shm) return false;

    // *See insert comment.
    scoped_lock<interprocess_recursive_mutex> lock(*m_msmp_mutex);

    try
    {
        shm_wstring shm_deviceid(deviceid.c_str(), m_convertible_void_alloc);

        if (m_set_wstring_offset->count(shm_deviceid))
        {
            return true;
        }
    }
    catch (boost::interprocess::bad_alloc &e)
    {
        std::cerr << "managed shared mememory bad_alloc " << e.what() << '\n';
    }

    return false;
}

std::string SharedStringSet::get_set_name() const
{
    return m_device_set_name;
}

} // end namespace ipc
} // end namespace vo
