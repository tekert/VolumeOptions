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

#include <boost/interprocess/sync/scoped_lock.hpp>

#include "../volumeoptions/sound_windows_ipc.h"

using namespace ipc;

SoundDevicesSet::SoundDevicesSet()
    : m_we_created_shared_mem(false)
{
    using namespace boost::interprocess;

    // Use windows managed shared mem in this case
    // Create or open our "windows" managed shared segment if exists.

    // SELFNOTE: make sure is big enogh for a set with ~20 strings max, resizing is not easy.
    //
    // wchar_t DeviceIDs max and min size: 110 bytes, char string (ansi only): 55 bytes
    //  then: 110*20 wstrings plus the object control data and depending if its debug or not, rounded to 16bytes
    //  so be safe lets assume 160 for every string and for the set, key and control block another 128+128
    //  thats a total of: ((160+128) * 20elemnts_max) + 128 = ~5888 bytes.
    //
    // Pages in windows are typicaly 4k bytes 'dwPageSize' (mapped_region::get_page_size() is wrong about "Page size", 
    //  it not the same as 'dwAllocationGranularity' "Page granularity" ).
    //  dwAllocationGranularity is "The granularity with which virtual memory is allocated". used by VirtualAlloc.
    // either way, 64KiB is safe to have room, dwAllocationGranularity is typicaly 64KiB
    offset_t block_size = 128 * 1024;

    try
    {
        try
        {
            m_managed_shm = managed_windows_shared_memory(open_only, "win_managed_mem-"VO_GUID_STRING);

            dprintf("IPC Set: win_managed_mem-GUID OPEN\n");
        }
        catch (boost::interprocess::interprocess_exception)
        {
            //std::cerr << e.what() << "    ->  Creating a new managed_sharedmem then."<< '\n';
            m_managed_shm = managed_windows_shared_memory(open_or_create, "win_managed_mem-"VO_GUID_STRING, block_size);
            m_we_created_shared_mem = true;

            dprintf("IPC Set: win_managed_mem-GUID OPEN-CREATED\n");
        }
    }
    catch (boost::interprocess::interprocess_exception& e)
    {
        std::cerr << e.what() << '\n';
        return;
    }
    block_size = m_managed_shm.get_size();


    // Create a set in shared memory

    // An allocator convertible to any allocator<T, segment_manager_t> type
    void_shm_allocator alloc_inst(m_managed_shm.get_segment_manager());
    //! not needed: wchar_t_shm_alocator  wcharallocator(segment.get_segment_manager());
    //! not needed: wstring_shm_allocator wstringallocator(segment.get_segment_manager());

    try
    {
        // Construct our sharedmem compatible set inside our managed sharedmem with a name.
        m_msmpSetWstring = m_managed_shm.find_or_construct<shm_wstring_set>
            //(object name), (first ctor parameter, second ctor parameter)
            ("deviceids_set-"VO_GUID_STRING)(std::less<shm_wstring>(), alloc_inst); // <wstringallocator>

        // NOTE: care use different names! or will compile a run with silent problems on windows.
        // Construct anonymous sync primitives inside our managed "windows" sharedmem, not outside 
        //      or we'll have to delete it.
        m_msmp_mutex = m_managed_shm.find_or_construct<interprocess_mutex>("imtx-"VO_GUID_STRING)();
        m_msmp_cond = m_managed_shm.find_or_construct<interprocess_condition>("icond-"VO_GUID_STRING)();
        //! Not outside: named_mutex named_mtx{ open_or_create, "mtx-GUID" };
        //! Not outside: named_condition named_cnd{ open_or_create, "cnd-GUID" };
    }
    catch (boost::interprocess::bad_alloc &e)
    {
        std::cerr << "managed shared mememory bad_alloc " << e.what() << '\n';
    }

    dprintf("IPC Set: sucessfully constructed.\n");
}
#include <thread>
SoundDevicesSet::~SoundDevicesSet()
{
    //! not outside: named_mutex::remove("mtx-GUID");
    //! not outside: named_condition::remove("cnd-GUID");
    using namespace boost::interprocess;
    //

    if (m_msmpSetWstring)
    {

        // scoped_lock<interprocess_mutex> lock(*m_msmp_mutex);

        // Delete what we locally inserted
        void_shm_allocator alloc_inst(m_managed_shm.get_segment_manager());
        for (auto i : m_local_device_count)
        {
            try
            {
                shm_wstring shm_deviceid(i.c_str(), alloc_inst);
                m_msmpSetWstring->erase(shm_deviceid);
            }
            catch (boost::interprocess::bad_alloc &e)
            {
                std::cerr << "managed shared mememory bad_alloc " << e.what() << '\n';
            }
        }
    }
}

void SoundDevicesSet::insert_device(const std::wstring& deviceid)
{
    using namespace boost::interprocess;

    scoped_lock<interprocess_mutex> lock(*m_msmp_mutex);

    void_shm_allocator alloc_inst(m_managed_shm.get_segment_manager());
    try
    {
        m_msmpSetWstring->emplace(deviceid.c_str(), alloc_inst);
        m_local_device_count.insert(deviceid);
    }
    catch (boost::interprocess::bad_alloc &e)
    {
        std::cerr << "managed shared mememory bad_alloc " << e.what() << '\n';
    }

}

bool SoundDevicesSet::remove_device(const std::wstring& deviceid)
{
    using namespace boost::interprocess;

    // Important block with the same mutex here too, this set should be atomic
    scoped_lock<interprocess_mutex> lock(*m_msmp_mutex);

    void_shm_allocator alloc_inst(m_managed_shm.get_segment_manager());
    shm_wstring shm_deviceid(deviceid.c_str(), alloc_inst);

    size_t sz = m_msmpSetWstring->erase(shm_deviceid);
    if (sz)
    {
        return true;
    }

    return false;
}

bool SoundDevicesSet::is_device_set(const std::wstring& deviceid)
{
    using namespace boost::interprocess;

    // Important block with the same mutex here too, all operations on this set should be atomic
    scoped_lock<interprocess_mutex> lock(*m_msmp_mutex);

    void_shm_allocator alloc_inst(m_managed_shm.get_segment_manager());
    shm_wstring shm_deviceid(deviceid.c_str(), alloc_inst);

    if (m_msmpSetWstring->count(shm_deviceid))
    {
        return true;
    }

    return false;
}

