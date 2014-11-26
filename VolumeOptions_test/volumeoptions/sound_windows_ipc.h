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

#define BOOST_INTERPROCESS_ENABLE_TIMEOUT_WHEN_LOCKING
#define BOOST_INTERPROCESS_TIMEOUT_WHEN_LOCKING_DURATION_MS 2000

#include <boost/interprocess/managed_windows_shared_memory.hpp>
#include <boost/interprocess/allocators/allocator.hpp>
#include <boost/interprocess/containers/set.hpp>
#include <boost/interprocess/containers/string.hpp>
#include <boost/interprocess/sync/interprocess_recursive_mutex.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>

#include <set>
#include <string>
#include <memory>

namespace vo {
namespace ipc {

/*
    Ok, this is tricky, we need to protect access to a shared object.
        boost managed mem is already protected for creating objects, finding objects references... you get the idea.

    The thing is.. if we use mutex to protect an object internals in a single shared memory, and the process
        terminates while holding a mutex we will get undefined state in our object, if we lock the mutex on destruction
        to erase data, we are calling for trouble too, object destruction is also called on process abort depending.
    Even worse.. the internal library alloc mutex can get abandoned, we can check if its abandoned with a defined timer
           then boost::interprocess throws if abadoned, but we cant unlock it, so the shared memory is dead basically. 

    Solution:

    ALL_PLATFORMS: use robust mutexes, it involves PID cheking, Process resource cheking, swap ownership. 
        its a little experimental/green for me.

    WINDOWS_ONLY: a simple solution that works on windows is taking advantage that the OS will remove the shared
        memory when the process terminates, that means every process should write to his own segment of shared mem
        using a easy to find/iterate name for the other process/es to find.
        We have to write our object there, not only a mutex because the object could be left in undefined state.
        This solution is only apropiate when not many access are done.
        OR just use native mutexes.

        And now we finally have this implementation, the windows way: (subject to change)

        Creates his own shared SharedStringSet in his own shared memory, and every time is
        going to insert something, it searchs for others shared mems currently open(not process terminated) and checks
        if it can find something before inserting, ugly but effective in less code, we dont need performance on this..

        Every process has:
            In his 'own' shared segment:
            * const named string object with his PID. (TODO)
            * set with his current monitored device IDs.

            (every one of the message queues uses a modification of boost message queues for windows managed mem)
            In multiple shared segments (transparent):
            * message queue for receiving messages. (WORKING)
            *** multiple message queues to broadcast or send messages. (WORKING)

    Until i can find a way to garantee object state: 
        maybe with object cheking/reconstruction and boost timed mutex
        or cheking if PID is up on short timer etc
        or a better way to do it i will use this.

*/


class SharedMemoryManager;

class SharedStringSet // TODO: template it wstring or string
{
public:
    SharedStringSet(std::shared_ptr<boost::interprocess::managed_windows_shared_memory> sp_managed_shm,
        const std::string& _managed_memory_name);
    ~SharedStringSet();

    bool insert(const std::wstring& deviceid);
    bool exists(const std::wstring& deviceid);
    bool erase(const std::wstring& deviceid);

    std::string get_set_name();

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
private:

    void construct_objects_atomic(
        std::shared_ptr<boost::interprocess::managed_windows_shared_memory> sp_managed_shm);

    // Set to store monitored deviceID wstrings globaly
    shm_wstring_set* m_msmpSetWstring = nullptr; // managed shared memory pointer to wstring set.
    std::set<std::wstring> m_local_device_count; // keeps track of local insertions to erase them from sharedmem on destruction.

    // To Sync object internals (can hang on process termination...)
    boost::interprocess::interprocess_recursive_mutex* m_msmp_mutex = nullptr;
    boost::interprocess::interprocess_condition* m_msmp_cond = nullptr;

    // where to find the object in managed memory (empty if not constructed)
    std::string m_device_set_name;
    std::string m_imutex_name;
    std::string m_icond_name;

    // References to our memory manager
    std::weak_ptr<boost::interprocess::managed_windows_shared_memory> m_wp_managed_shm;

    bool m_we_created_set;
};

namespace win
{
    class SharedMemoryManager // TODO: port messaqueue from my test module
    {
    public:
        SharedMemoryManager();
        ~SharedMemoryManager();

        bool find_device(const std::wstring& deviceid);
        bool remove_device(const std::wstring& deviceid);
        bool set_device(const std::wstring& deviceid);

        std::string get_manager_name();
        std::shared_ptr<boost::interprocess::managed_windows_shared_memory> get_manager();

    private:

        const std::string m_managed_shared_memory_base_name = "win_managed_mem-"VO_GUID_STRING;

        bool create_free_managed_smem(const std::string& base_name, boost::interprocess::offset_t block_size = 128 * 1024);
        void open_create_managed_smem(const std::string& name, boost::interprocess::offset_t block_size = 128 * 1024);

        std::unique_ptr<SharedStringSet> m_msm_up_devices_set;

        std::shared_ptr<boost::interprocess::managed_windows_shared_memory> m_managed_shm;

        std::string m_opened_managed_sm_name;

        const unsigned short m_max_slots;

        bool m_we_created_shared_mem;
    };

} // end namespace win
} // end namespace ipc
} // end namespace vo

#endif