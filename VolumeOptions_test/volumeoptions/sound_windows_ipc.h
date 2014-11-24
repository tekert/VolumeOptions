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

#include <boost/interprocess/managed_windows_shared_memory.hpp>
#include <boost/interprocess/allocators/allocator.hpp>
#include <boost/interprocess/containers/set.hpp>
#include <boost/interprocess/containers/string.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>

#include <set>
#include <string>

namespace ipc
{

/*
    Ok, this is tricky, we need to protect access to a shared object.
        boost managed mem is already protected for creating objects, finding objects references... you get the idea.

    The thing is.. if we use mutex to protect an object internals in a single shared memory, and the process terminates...
        we will get undefined state in our object, if we lock the mutex on destruction to erase data,
        we are calling for trouble too, object destruction is called on process abort depending.

    Solution:
    WINDOWS_ONLY: i dont want to use robust mutex or etc, a simple solution that works on windows is taking
        advantage that the OS will remove the shared memory when the process terminates, that means every process
        should write to his own segment of shared mem using a easy to find/iterate name for the other process/es to find.
    We have to write our object there, not only a mutex because the object could be left in undefined state.
        And now we finally have this implementation: 

        Creates his own shared Set in his own shared memory, and every time is
        going to insert something, it searchs for other shared_mem currently open(not process terminated) and checks
        if it can find something before inserting.

        Every process has:
            *const named string object with his PID.
            *set with his current monitored device IDs.

            Every one of the message queues uses my modidification of boost message queues for windows managed mem.
            *message queue for receiving messages.
            ***multiple message queues to broadcast or send messages.
*/




    class SoundDevicesSet // TODO change name to WinIPC_DevicesSet, or namespace vo::win::ipc::deviceset
    {
    public:
        SoundDevicesSet();
        ~SoundDevicesSet();


        void insert_device(const std::wstring& deviceid);
        bool is_device_set(const std::wstring& deviceid);
        bool remove_device(const std::wstring& deviceid);

    private:

        // Typedefs of allocators and containers

        // Use windows managed shared mem in this case                                      /////////////////////
        typedef boost::interprocess::managed_windows_shared_memory::segment_manager         segment_manager_t;
        typedef boost::interprocess::allocator<void, segment_manager_t>                     void_shm_allocator;
        /////////////////////
        typedef boost::interprocess::allocator<char, segment_manager_t>                     char_shm_allocator;
        typedef boost::interprocess::basic_string < char,
            std::char_traits<char>, char_shm_allocator > shm_string;
        /////////////////////
        typedef boost::interprocess::allocator<wchar_t, segment_manager_t>                  wchar_t_shm_alocator;
        typedef boost::interprocess::basic_string < wchar_t, std::char_traits<wchar_t>,
            wchar_t_shm_alocator > shm_wstring;
        /////////////////////
        typedef boost::interprocess::allocator<shm_wstring, segment_manager_t>              wstring_shm_allocator;
        typedef boost::interprocess::set < shm_wstring, std::less<shm_wstring>,
            wstring_shm_allocator > shm_wstring_set;
        /////////////////////
        boost::interprocess::managed_windows_shared_memory m_managed_shm;
        bool m_we_created_shared_mem;

        // Set to store monitored deviceID wstrings globaly
        shm_wstring_set* m_msmpSetWstring = nullptr; // managed shared memory pointer to wstring set.
        std::set<std::wstring> m_local_device_count; // keeps track of local insertions to erase them from sharedmem on destruction.

        boost::interprocess::interprocess_mutex* m_msmp_mutex = nullptr;
        boost::interprocess::interprocess_condition* m_msmp_cond = nullptr;
    };


}
#endif