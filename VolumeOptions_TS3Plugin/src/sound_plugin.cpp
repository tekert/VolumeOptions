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

#include<boost/tokenizer.hpp>
#include <boost/algorithm/string.hpp>

#include "../volumeoptions/vo_config.h"
#include "../volumeoptions/sound_plugin.h"


/*  Utilities	*/

// convert UTF-8 string to wstring
std::wstring utf8_to_wstring(const std::string& str)
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>> myconv;
    return myconv.from_bytes(str);
}

// convert wstring to UTF-8 string
std::string wstring_to_utf8(const std::wstring& str)
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>> myconv;
    return myconv.to_bytes(str);
}



/////////////////////////	Team Speak 3 Interface	//////////////////////////////////


VolumeOptions::VolumeOptions(const vo::volume_options_settings& settings, const std::string &sconfigPath)
    : m_quiet(true)
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
    m_paudio_monitor->InitEvents();
#endif
#ifdef _DEBUG
    m_paudio_monitor->Refresh();
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
    /*
    in <<
        "[global]\n"
        "enabled = 1\n"
        "vol_reduction = 0.5                # from 0.0 to 1.0\n"
        "vol_up_delay = 400                 # as milliseconds\n"
        "vol_as_percentage = 1              # 0 = take vol as fixed level, 1 = take vol as %\n"
        "change_only_active_sessions = 1    # recommended on \"1\" use \"0\" only in special cases\n"
        "exclude_own_process = 1            # this should be 1 always\n"
        "\n"
        "\n"
        "# excluded_pids and included_pids takes a list of process IDs\n"
        "# excluded_process and included_process takes a list of executable names or paths\n"
        "#\n"
        "# takes a list separated by;\n"
        "# in case of process names, can be anything, from full path to name to search in full path\n"
        "#\n"
        "# example:\n"
        "# excluded_process = \"process1.exe;C:\\this\\path\\to\\my\\program;_player\"\n"
        "# excluded_pids = 432; 5; 5832\n"
        "\n"
        "use_included_filter = 0            # use exluded or included filters, cant use both for now.\n"
        "\n"
        "excluded_pids = \"\"\n"
        "excluded_process = \"\"\n"
        "\n"
        "included_pids = \"\"\n"
        "included_process = \"\"\n"
        "\n"
        "\n"
        "[sessions]\n"
        "\n"
        "# takes a list of pairs \"process:volume\" separed by \";\" NOTE: NOT IMPLEMENTED YET.\n"
        "#volume_list = \"mymusic.exe:0.4;firefox.exe:0.8\"\n";
        */

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

    printf("VO_PLUGIN: Reseting call data\n");
    while (!m_calls.empty())
        m_calls.pop();

    VolumeOptions::restore_default_volume();
}

void VolumeOptions::set_status(status s)
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    printf("VO_PLUGIN: VO Status: %s", s == status::DISABLED ? "Disabled" : "Enabled");
    m_status = s;
}

void VolumeOptions::set_channel_status(uint64_t channelID, status s)
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    printf("VO_PLUGIN: Channel %llu Status: %s", channelID, s == status::DISABLED ? "Disabled" : "Enabled");

    if (s == status::DISABLED)
    {
        if (m_disabled_channels.find(channelID) == m_disabled_channels.end())
        {
            m_disabled_channels[channelID] = true;
        }
    }
    if (s == status::ENABLED)
    {
        m_disabled_channels.erase(channelID);
    }
}

void VolumeOptions::set_client_status(uint64_t channelID, status s)
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    printf("VO_PLUGIN: Client %llu Status: %s", channelID, s == status::DISABLED ? "Disabled" : "Enabled");

    if (s == status::DISABLED)
    {
        if (m_disabled_clients.find(channelID) == m_disabled_clients.end())
        {
            m_disabled_clients[channelID] = true;
        }
    }
    if (s == status::ENABLED)
    {
        m_disabled_clients.erase(channelID);
    }
}

vo::volume_options_settings VolumeOptions::get_current_settings() const
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    return m_vo_settings;
}

int VolumeOptions::process_talk(const bool talk_status, uint64_t channelID, uint64_t clientID,
        bool ownclient)
{
    int r = 1;

    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    // if this is mighty ourselfs talking, ignore?
    if ((ownclient) && (m_vo_settings.exclude_own_client))
    {
        printf("VO_PLUGIN: We are talking.. do nothing\n", clientID);
        return r;
    }

    // Count the number os users currently talking using a stack.
    // NOTE: we asume TS3Client always sends events in logical order per client
    if (talk_status)
        m_calls.push(1);
    else
        m_calls.pop();

    printf("VO_PLUGIN: Update Users currently talking: %d\n", m_calls.size());

    // if last client stoped talking, restore sounds.
    if (m_calls.empty())
    {
        if ((m_status == status::ENABLED) &&
            (m_disabled_channels.find(channelID) == m_disabled_channels.end()) &&
            (m_disabled_clients.find(channelID) == m_disabled_clients.end()))
        {
                printf("VO_PLUGIN: Monitoring Sessions Stopped, Restoring Sessions to default state...\n");
                r = m_paudio_monitor->Pause();
                //m_paudio_monitor->Stop();
        }
        m_quiet = true;
    }

    // if someone talked while the channel was quiet, redudce volume (else was already lowered)
    if (!m_calls.empty() && m_quiet)
    {
        if ((m_status == status::ENABLED) &&
            (m_disabled_channels.find(channelID) == m_disabled_channels.end()) &&
            (m_disabled_clients.find(channelID) == m_disabled_clients.end()))
        {
                printf("VO_PLUGIN: Monitoring Sessions Active.\n");
                r = m_paudio_monitor->Start();
        }
        m_quiet = false;
    }

    return r; // TODO error codes
}
