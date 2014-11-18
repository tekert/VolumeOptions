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

    * Neither the name of [project] nor the names of its
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <codecvt>
#include <fstream>

// for ini parser
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>

#include <boost/tokenizer.hpp>
#include <boost/algorithm/string.hpp>

#include "../volumeoptions/vo_config.h"
#include "../volumeoptions/sound_plugin.h"


/*  Utilities	*/

// convert UTF-8 string to wstring
inline std::wstring utf8_to_wstring(const std::string& str)
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>> myconv;
    return myconv.from_bytes(str);
}

// convert wstring to UTF-8 string
inline std::string wstring_to_utf8(const std::wstring& str)
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>> myconv;
    return myconv.to_bytes(str);
}



/////////////////////////	Team Speak 3 Interface	//////////////////////////////////


VolumeOptions::VolumeOptions(const vo::volume_options_settings& settings, const std::string &sconfigPath)
    : m_someone_enabled_is_talking(false)
    , m_status(status::ENABLED)
{
    // Parse your settings, monitor_settings are parsed when aplying to monitor.
    m_vo_settings = settings;

    // Create config file
    std::string configFile(sconfigPath + "\\volumeoptions_plugin.ini");
    std::fstream in(configFile, std::fstream::in);
    if (!in)
    {
        in.open(configFile, std::fstream::out | std::ios::trunc);
        if (!in)
            printf("VO_PLUGIN: Error creating config file %s\n", configFile.c_str());
        else
        {
            create_config_file(in);
        }
    }
    else
    {
        int ok = parse_config(in); // 0 on error.
        if (!ok)
        {
            printf("VO_PLUGIN: Error parsing ini file. using default values (delete file to recreate)\n");
            // TODO use TS3 client log.
        }
    }
    in.close();

    // Create the audio monitor
    //m_paudio_monitor = std::make_shared<AudioMonitor>(m_vo_settings.monitor_settings);
    m_paudio_monitor = AudioMonitor::create(m_vo_settings.monitor_settings);
    //m_paudio_monitor = std::shared_ptr<AudioMonitor>(new AudioMonitor(m_vo_settings.monitor_settings));

#ifdef VO_ENABLE_EVENTS
    m_paudio_monitor->InitEvents(); // TODO: dont needed anymore after creation. analize. originaly was because we needed a shared_ptr first, and after io_service init.
#endif
#ifdef _DEBUG
    m_paudio_monitor->Refresh(); // To debug current sessions enum quickly. not used much now.
#endif

}

VolumeOptions::~VolumeOptions()
{
    VolumeOptions::restore_default_volume();

    m_paudio_monitor.reset();
}


void VolumeOptions::create_config_file(std::fstream& in)
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    if (!in)
        return;

    // INI parser cant read values with comments on the same line
    in <<
        "[global]\n"
        "enabled = 1\n"
        "\n"
        "# from 0.0 to 1.0 default 0.5(50%)\n"
        "vol_reduction = 0.5\n"
        "\n"
        "# as milliseconds default 400ms\n"
        "vol_up_delay = 400\n"
        "\n"
        "# 0 = take vol as fixed level, 1 = take vol as % default 1(true)\n"
        "vol_as_percentage = 1\n"
        "\n"
        "# recommended on \"1\" use \"0\" only in special cases default 1(true)\n"
        "change_only_active_sessions = 1\n"
        "\n"
        "# this should be 1 always default 1(true)\n"
        "exclude_own_process = 1\n"
        "\n"
        "# change volume when we talk? default 1(true)\n"
        "exclude_own_client = 1\n"
        "\n"
        "# excluded_pids and included_pids takes a list of process IDs\n"
        "# excluded_process and included_process takes a list of executable names or paths\n"
        "#\n"
        "# takes a list separated by ;\n"
        "# in case of process names, can be anything, from full path to name to search in full path\n"
        "#\n"
        "# Example:\n"
        "# excluded_process = process1.exe ; C:\\this\\path\\to\\my\\program; _player\n"
        "# excluded_pids = 432;5; 5832\n"
        "\n"
        "# use exluded or included filters, cant use both for now.\n"
        "use_included_filter = 0\n"
        "\n"
        "excluded_pids = \n"
        "excluded_process = \n"
        "\n"
        "included_pids = \n"
        "included_process = \n"
        "\n"
        "\n"
        "[sessions]\n"
        "\n"
        "# takes a list of pairs \"process:volume\" separed by \";\" NOTE: NOT IMPLEMENTED YET.\n"
        "#volume_list = mymusic.exe:0.4; firefox.exe:0.8\n";
}

int VolumeOptions::parse_config(std::fstream& in)
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    const vo::volume_options_settings default_settings;

    using boost::property_tree::ptree;
    ptree pt;

    try
    {
        read_ini(in, pt);
    }
    catch (...) // TODO what throws?
    {
        return 0;
    }

    // Using boost property threes, for ini files we use get default-value version (no throw).
    // http://www.boost.org/doc/libs/1_57_0/doc/html/boost_propertytree/accessing.html

    // TODO: fill missing settings on ini file. use thow version of get.

    int enabled = pt.get("global.enabled", 1);
    if (!enabled) m_status = status::DISABLED;

    // AudioMonitor will validate these values when AudioMonitor::SetSettings is called

    // TODO: make Global section, AudioMonitor Section, AudioSession section and TS3Plugin section

    // float: Global volume reduction
    m_vo_settings.monitor_settings.ses_global_settings.vol_reduction = 
        pt.get("global.vol_reduction", default_settings.monitor_settings.ses_global_settings.vol_reduction);

    // long long: Read a number using millisecond represetantion (long long) create milliseconds chrono type and assign it.
    std::chrono::milliseconds::rep _delay_milliseconds =
        pt.get("global.vol_up_delay", default_settings.monitor_settings.ses_global_settings.vol_up_delay.count());
    std::chrono::milliseconds delay_milliseconds(_delay_milliseconds);
    m_vo_settings.monitor_settings.ses_global_settings.vol_up_delay = delay_milliseconds;

    // bool: Change vol as % or fixed
    m_vo_settings.monitor_settings.ses_global_settings.treat_vol_as_percentage = 
        pt.get("global.vol_as_percentage", default_settings.monitor_settings.ses_global_settings.treat_vol_as_percentage);

    // bool: Change vol only to active audio sessions? recommended
    m_vo_settings.monitor_settings.ses_global_settings.change_only_active_sessions = 
        pt.get("global.change_only_active_sessions", default_settings.monitor_settings.ses_global_settings.change_only_active_sessions);

    // bool: Dont know why but... yep..  1 enable, 0 disable
    m_vo_settings.monitor_settings.exclude_own_process = 
        pt.get("global.exclude_own_process", default_settings.monitor_settings.exclude_own_process);

    // bool: do we exclude ourselfs?
    m_vo_settings.exclude_own_client =
        pt.get("global.exclude_own_client", default_settings.exclude_own_client);
        
    std::string pid_list;
    std::string process_list;

    // bool: Cant use both filters.
    int use_included_filter = pt.get("global.use_included_filter", default_settings.monitor_settings.use_included_filter);

    if (use_included_filter)
    {
        pid_list = pt.get("global.included_pids", "");
        process_list = pt.get("global.included_process", "");
    }
    else
    {
        pid_list = pt.get("global.excluded_pids", "");
        process_list = pt.get("global.excluded_process", "");
    }
    
    // NOTE_TODO: remove trimming if the user wants trailing spaces on names, or add "" on ini per value.
    boost::char_separator<char> sep(";");
    boost::tokenizer<boost::char_separator<char>> processtokens(process_list, sep);
    boost::tokenizer<boost::char_separator<char>> pidtokens(process_list, sep);
    for (auto it = processtokens.begin(); it != processtokens.end(); ++it)
    {
        std::string pname(*it);
        boost::algorithm::trim(pname);
        if (use_included_filter)
            m_vo_settings.monitor_settings.included_process.insert(utf8_to_wstring(pname));
        else
            m_vo_settings.monitor_settings.excluded_process.insert(utf8_to_wstring(pname));
    }
    for (auto it = pidtokens.begin(); it != pidtokens.end(); ++it)
    {
        try
        {
            std::string spid(*it);
            boost::algorithm::trim(spid);
            if (use_included_filter)
                m_vo_settings.monitor_settings.included_pids.insert(std::stoi(spid));
            else
                m_vo_settings.monitor_settings.excluded_pids.insert(std::stoi(spid));
        }
        catch (std::invalid_argument) // std::stoi TODO: return something to report it to log
        { }
        catch (std::out_of_range) // std::stoi TODO: return something to report it to log
        { }
    }


#ifdef _DEBUG
    printf("\n\n\n\n");
    for (auto& section : pt)
    {
        std::cout << "section.first= " << '[' << section.first << "]\n";
        for (auto& key : section.second)
            std::cout << "key.first="<< key.first << "    key.second.get_value<std::string>()=" << key.second.get_value<std::string>() << "\n";
    }
    printf("\n\n\n\n");
#endif

    return 1;
}

void VolumeOptions::set_settings(vo::volume_options_settings& settings)
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    // monitor_settings will be parsed, applied and updated with actual settings applied.
    m_paudio_monitor->SetSettings(settings.monitor_settings);
}

float VolumeOptions::get_global_volume_reduction() const
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    // gets the global vol reduction.
    return m_paudio_monitor->GetVolumeReductionLevel();
}

void VolumeOptions::restore_default_volume()
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    printf("VO_PLUGIN: Restoring per app user default volume\n");

    m_paudio_monitor->Stop();
}

void VolumeOptions::reset_data() // TODO: uhm
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    printf("VO_PLUGIN: Reseting talk data\n");
    while (!m_clients_talking.empty())
        m_clients_talking.clear();

    VolumeOptions::restore_default_volume();
}

void VolumeOptions::set_status(status newstatus)
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    printf("VO_PLUGIN: VO Status: %s\n", newstatus == status::DISABLED ? "Disabled" : "Enabled");

    // Reenable AudioMonitor only if someone non disabled is currently talking
    if (m_someone_enabled_is_talking && (newstatus == status::ENABLED) && (m_status == status::DISABLED))
        m_paudio_monitor->Start();

    // Stop AudioMonitor only if someone non disabled is currently talking
    if (m_someone_enabled_is_talking && (newstatus == status::DISABLED) && (m_status == status::ENABLED))
        m_paudio_monitor->Stop();

    m_status = newstatus;
}

void VolumeOptions::set_channel_status(uint64_t channelID, status s)
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    printf("VO_PLUGIN: Channel %llu Status: %s\n", channelID, s == status::DISABLED ? "Disabled" : "Enabled");

    if (s == status::DISABLED)
    {
        if (!m_disabled_channels.count(channelID))
        {
            m_disabled_channels.insert(channelID);

            // add channel to performance set if its disabled and has activity
            if (m_channels_with_activity.count(channelID))
                m_disabled_channels_with_activity.insert(channelID);
        }
    }
    if (s == status::ENABLED)
    {
        m_disabled_channels.erase(channelID);

        // remove channel from performance set if it was disabled and had activity
        if (m_channels_with_activity.count(channelID))
            m_disabled_channels_with_activity.erase(channelID);
    }

    // Update statuses
    apply_status();
}

void VolumeOptions::set_client_status(uint64_t clientID, status s)
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    printf("VO_PLUGIN: Client %llu Status: %s\n", clientID, s == status::DISABLED ? "Disabled" : "Enabled");

    if (s == status::DISABLED)
    {
        if (!m_disabled_clients.count(clientID))
        {
            m_disabled_clients.insert(clientID);

            // if disabled client is currently talking
            if (m_clients_talking.count(clientID))
                m_disabled_clients_talking.insert(clientID);
        }
    }
    if (s == status::ENABLED)
    {
        m_disabled_clients.erase(clientID);

        // if disabled client is currently talking
        if (m_clients_talking.count(clientID))
            m_disabled_clients_talking.erase(clientID);
    }

    // Update statuses
    apply_status();
}

vo::volume_options_settings VolumeOptions::get_current_settings() const
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    return m_vo_settings;
}

/*
    Starts or stops audio monitor based on ts3 talking statuses.

    We use three sets:
    m_clients_talking has all the clients (disabled and non disabled) currently talking.
    m_disabled_clients has all the clients that should not affect apps volume.
    m_disabled_clients_talking has all the disabled clients currenlty talking.

    Notice we can compute 'm_disabled_clients_talking' from the other first two sets, but we need to know when
        all the not disabled clients are quiet(not talking), and compute it every time someone talks, 
        to increase performance we use another set with that info 'm_disabled_clients_talking'. So we can just simply
        compare sizes: if (m_clients_talking.size() - m_disabled_clients_talking.size()) == 0 then we got it fast.
    Same thing with channels.
*/
int VolumeOptions::apply_status()
{
    int r = 1;

    // if last client non disabled stoped talking, restore sounds. 
    // (I did it with another talking set for performance, so we dont have to search wich m_clients_talking are
    //      disabled every time, same for channels)
    if (m_clients_talking.empty() ||
        (m_clients_talking.size() == m_disabled_clients_talking.size()) ||
        (m_channels_with_activity.size() == m_disabled_channels_with_activity.size()) )
    {
        if (m_status == status::ENABLED)
        {
            if (m_paudio_monitor->GetStatus() != AudioMonitor::monitor_status_t::PAUSED) // so we dont repeat it.
            {
                printf("VO_PLUGIN: Monitoring Sessions Stopped, Restoring Sessions to default state...\n");
                r = m_paudio_monitor->Pause();
                //m_paudio_monitor->Stop();
            }
        }
        m_someone_enabled_is_talking = false; // excluding disabled
    }
    else // someone non disabled is talking
    {
        // if someone non disabled talked while audio monitor was down, start it
        if (!m_someone_enabled_is_talking)
        {
            if (m_status == status::ENABLED)
            {
                if (m_paudio_monitor->GetStatus() != AudioMonitor::monitor_status_t::RUNNING) // so we dont repeat it.
                {
                    printf("VO_PLUGIN: Monitoring Sessions Active.\n");
                    r = m_paudio_monitor->Start();
                }
            }
            m_someone_enabled_is_talking = true;
        }
    }
    return r;
}

/*
    Handler for TS3 onTalkStatusChangeEvent
*/
int VolumeOptions::process_talk(const bool talk_status, uint64_t channelID, uint64_t clientID,
    bool ownclient)
{
    int r = 1;

    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    // if this is mighty ourselfs talking, ignore after we stop talking to update.
    // TODO if user ignores himself well get incorrect count, fix it.
    if ((ownclient) && (m_vo_settings.exclude_own_client) && !m_clients_talking.count(clientID))
    {
        dprintf("VO_PLUGIN: We are talking.. do nothing\n");
        return r;
    }

    // NOTE: We assume TS3 will always send talk_status false when other clients disconnects, changes channel or etc.
    // Lines marked with /**/ are performance sets used to search quickly wich disabled clients/channels
    //      dont count for starting our audio change monitor see ::apply_status() for info.
    if (talk_status)
    {
        m_clients_talking.insert(clientID);

        // If this client currently talking should not affect volume, add it to another set for performance.
        if (m_disabled_clients.count(clientID))             /**/
            m_disabled_clients_talking.insert(clientID);    /**/


        // if this is the first talk activity in the channel (for performance, so we dont repeat this)
        if (!m_channels_with_activity.count(channelID))
        {
            if (m_disabled_channels.count(channelID))                   /**/
                m_disabled_channels_with_activity.insert(channelID);    /**/
        }
        //
        m_channels_with_activity[channelID].insert(clientID);
        dprintf("VO_PLUGIN: Update: m_channels_with_activity[%llu].size()= %llu\n", channelID, m_channels_with_activity[channelID].size());
    }
    else
    {
        m_clients_talking.erase(clientID);

        // also remove it from here if it was disabled. (we use another set for performance, see ::apply_status())
        if (m_disabled_clients.count(clientID))             /**/
            m_disabled_clients_talking.erase(clientID);     /**/


        // substract (pop) from channel count, if empty delete it.
        dprintf("VO_PLUGIN: Update: m_channels_with_activity[%llu].size()= %llu\n", channelID, m_channels_with_activity[channelID].size());
        // damn TS3.. when a client is moved from a channel the talk status false has the new channel, not the old.. rewrite this fix later.
        channelIDtype moved_from_channel_fix = channelID;
        for (auto it : m_channels_with_activity)
        {
            if (it.second.count(clientID)) { moved_from_channel_fix = it.first; break; }
        }
        m_channels_with_activity[moved_from_channel_fix].erase(clientID);
        if (m_channels_with_activity[moved_from_channel_fix].empty())
        {
            m_channels_with_activity.erase(moved_from_channel_fix);
            if (m_disabled_channels.count(moved_from_channel_fix))               /**/
                m_disabled_channels_with_activity.erase(moved_from_channel_fix); /**/
        }
    }

    dprintf("VO_PLUGIN: Update: Users currently talking (including disabled): %llu\n", m_clients_talking.size());
    dprintf("VO_PLUGIN: Update: Users currently talking (excluding disabled): %llu\n\n", 
        m_clients_talking.size() - m_disabled_clients_talking.size());
    dprintf("VO_PLUGIN: Update: Channels with activity (including disabled): %llu\n", m_channels_with_activity.size());
    dprintf("VO_PLUGIN: Update: Channels with activity (excluding disabled): %llu\n\n",
        m_channels_with_activity.size() - m_disabled_channels_with_activity.size());

    // Update audio monitor status
    apply_status();

    return r; // TODO error codes
}