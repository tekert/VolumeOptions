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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cassert>
#include <fstream>

// for ini parser
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>

#include <boost/tokenizer.hpp>
#include <boost/algorithm/string.hpp>

#include "../volumeoptions/utilities.h"
#include "../volumeoptions/config.h"
#include "../volumeoptions/vo_ts3plugin.h"


namespace vo
{

/////////////////////////	Team Speak 3 Interface	//////////////////////////////////

/*
    Will load supplied settings directly
*/
VolumeOptions::VolumeOptions(const vo::volume_options_settings& settings)
    : m_someone_enabled_is_talking(false)
    , m_status(status::ENABLED)
{
    m_vo_settings = settings;
    // nothing to parse from here, audiomonitor settings will be parsed when set.

    common_init();
}

/* 
    Basic constructor, will load default settings always. 
*/
VolumeOptions::VolumeOptions()
    : m_someone_enabled_is_talking(false)
    , m_status(status::ENABLED)
{
    common_init();
}

void VolumeOptions::common_init()
{
    m_clients_talking.resize(2);
    m_channels_with_activity.resize(2);

    // Create the audio monitor and send settings to parse, it will return parsed settings.
    if (!m_paudio_monitor)
        m_paudio_monitor = AudioMonitor::create();

    m_paudio_monitor->SetSettings(m_vo_settings.monitor_settings);
}

VolumeOptions::~VolumeOptions()
{
    // Save settings on exit.
    if (!m_config_filename.empty())
        save_settings_to_file(m_config_filename);

    // AudioMonitor destructor will do a Stop and automatically restore volume.m_paudio_monitor.
    m_paudio_monitor.reset();

    dprintf("\nVolumeOptions destroyed...\n\n");
}

/*
    Sets VolumeOptions config file location, absolute path to file
*/
void VolumeOptions::set_config_file(const std::string &configFile)
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    m_config_filename = configFile;
}

/*
    Tries to open configFile and set settings
        it will create and/or update the file if some options are missing.
*/
int VolumeOptions::set_settings_from_file(const std::string &configFile, bool create_if_notfound)
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    int ret = 1; // TODO: error codes

    std::fstream in(configFile, std::fstream::in);
    if (!in)
    {
        if (!create_if_notfound)
            return -1; // file not found.

        // Create config file if it doesnt exists
        in.open(configFile, std::fstream::out | std::ios::trunc);
        if (!in)
            printf("VO_PLUGIN: Error creating config file %s\n", configFile.c_str()); // TODO: report it to log
        else
        {
            create_config_file(in);
        }

        set_config_file(configFile);
    }
    else
    {
        try
        {
            boost::property_tree::ptree pt;
            // Load config
            boost::property_tree::read_ini(configFile, pt);
            boost::property_tree::ptree orig_pt = pt;

            // will update ptree if some values where missing
            m_vo_settings = ptree_to_settings(pt);

            // Update ini file if some values were missing.
            if (orig_pt != pt) // NOTE: this comparison can cause warnings depending on included headers
                write_ini(configFile, pt);

            set_config_file(configFile);
        }
        catch (boost::property_tree::ini_parser::ini_parser_error)
        {
            assert(false);
            ret = 0; // parse error. // TODO: error codes boost.
        }
    }
    in.close();

    m_paudio_monitor->SetSettings(m_vo_settings.monitor_settings);

    return ret;
}

void VolumeOptions::create_config_file(std::fstream& in)
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    if (!in)
        return;

    // INI parser cant read values with comments on the same line
    // TODO delete this now.. ini is auto regenerated without comments..
    in <<
        "[global]\n"
        "enabled = 1\n"
        "\n"
        "\n"
        "[plugin]\n"
        "\n"
        "# ignore volume change when we talk ? default 1(true)\n"
        "exclude_own_client = 1\n"
        "\n"
        "\n"
        "\n"
        "[AudioSessions]\n"
        "\n"
        "# from 0.0 to 1.0 default 0.5(50 % )\n"
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
        "\n"
        "\n"
        "[AudioMonitor]\n"
        "\n"
        "# this should be 1 always default 1(true)\n"
        "exclude_own_process = 1\n"
        "\n"
        "# excluded_pids and included_pids takes a list of process IDs\n"
        "# excluded_process and included_process takes a list of executable names or paths\n"
        "#\n"
        "# takes a list separated by \";\"\n"
        "# in case of process names, can be anything, from full path to name to search in full path\n"
        "#\n"
        "# Example:\n"
        "# excluded_process = process1.exe; C:\\this\\path\\to\\my\\program; _player\n"
        "# excluded_pids = 432; 5; 5832\n"
        "\n"
        "# use exluded or included filters, cant use both for now.\n"
        "use_included_filter = 0\n"
        "\n"
        "excluded_pids =\n"
        "excluded_process =\n"
        "\n"
        "included_pids =\n"
        "included_process =\n"
        "\n"
        "\n"
        "# takes a list of pairs \"process:volume\" separed by \";\" NOTE: NOT IMPLEMENTED YET.\n"
        "#volume_list = mymusic.exe:0.4; firefox.exe:0.8\n";

}

void VolumeOptions::save_settings_to_file(const std::string &configFile) const
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    // Converts current class settings to ptree
    boost::property_tree::ptree pt = settings_to_ptree(m_vo_settings);

    try
    {
        boost::property_tree::write_ini(configFile, pt);
    }
    catch (boost::property_tree::ini_parser::ini_parser_error)
    {
        assert(false);
    }
}

// Shortcuts (to be more readable)
inline
volume_options_settings VolumeOptions::ptree_to_settings(boost::property_tree::ptree& pt) const
{
    return parse_ptree(pt, m_vo_settings);
}
inline
boost::property_tree::ptree VolumeOptions::settings_to_ptree(const volume_options_settings& settings) const
{
    boost::property_tree::ptree pt;
    parse_ptree(pt, m_vo_settings);

    return pt;
}

/*
    Simple template, creates the path key with origin value if it doesn't exists. else returns the value.
*/
template <typename T>
T ini_put_or_get(boost::property_tree::ptree& pt, const std::string& path, const T& origin_value)
{
    boost::optional<T> opt = pt.get_optional<T>(path.c_str());
    if (opt)
    {
        return *opt;
    }
    else // put missing config
    {
        try { pt.put(path.c_str(), origin_value); }
        catch (boost::property_tree::ptree_bad_data) { assert(false); }
    }

    return origin_value;
}

/*
    Will parse ptree to settings or origin_settings to ptree.

    If some options are missing from ptree, it will use origin_settings as default.
    ptree will be updated with missing settings.
    If ptree is empty, origin_settings can be used to parse seetings to ptree.

    returns parsed settings.
*/
volume_options_settings VolumeOptions::parse_ptree(boost::property_tree::ptree& pt,
    const volume_options_settings& origin_settings) const
{
    using boost::property_tree::ptree;

    volume_options_settings parsed_settings;

    // settings shortcuts
    vo::session_settings& ses_settings = parsed_settings.monitor_settings.ses_global_settings;
    vo::monitor_settings& mon_settings = parsed_settings.monitor_settings;
    const vo::session_settings& def_ses_settings = origin_settings.monitor_settings.ses_global_settings;
    const vo::monitor_settings& def_mon_settings = origin_settings.monitor_settings;

    // Using boost property threes, boost::optional version used to fill missing config.
    // http://www.boost.org/doc/libs/1_57_0/doc/html/boost_propertytree/accessing.html

    // ------ Plugin Settings

    bool enabled = ini_put_or_get<bool>(pt, "global.enabled", m_status == status::DISABLED ? 0 : 1);
    if (!enabled) m_status = status::DISABLED;

    // bool: do we exclude ourselfs?
    parsed_settings.exclude_own_client = ini_put_or_get<bool>(pt, "plugin.exclude_own_client", origin_settings.exclude_own_client);


    // ------ Session Settings

    // float: Global volume reduction
    ses_settings.vol_reduction = ini_put_or_get<float>(pt, "AudioSessions.vol_reduction", def_ses_settings.vol_reduction);
   
    // long long: Read a number using millisecond represetantion (long long) create milliseconds chrono type and assign it.
    std::chrono::milliseconds::rep _delay_milliseconds;
    _delay_milliseconds = ini_put_or_get<std::chrono::milliseconds::rep>(pt, "AudioSessions.vol_up_delay", def_ses_settings.vol_up_delay.count());
    ses_settings.vol_up_delay = std::chrono::milliseconds(_delay_milliseconds);

    // bool: Change vol as % or fixed
    ses_settings.treat_vol_as_percentage = ini_put_or_get<bool>(pt, "AudioSessions.vol_as_percentage", def_ses_settings.treat_vol_as_percentage);

    // bool: Change vol only to active audio sessions? recommended
    ses_settings.change_only_active_sessions = ini_put_or_get<bool>(pt, "AudioSessions.change_only_active_sessions", def_ses_settings.change_only_active_sessions);


    // ------ Monitor Settings

    // bool: Dont know why but... yep..  1 enable, 0 disable
    mon_settings.exclude_own_process = ini_put_or_get<bool>(pt, "AudioMonitor.exclude_own_process", def_mon_settings.exclude_own_process);

    // i know, this is a bit messy and error prone, but i think is readable.
    std::string included_process_list, def_included_process_list;
    std::string excluded_process_list, def_excluded_process_list;
    std::string included_pid_list, def_included_pid_list;
    std::string excluded_pid_list, def_excluded_pid_list;

    // Convert sets to list of strings separated by ; to use them in case of missing ptree value.
    parse_set(def_mon_settings.included_process, def_included_process_list);
    parse_set(def_mon_settings.excluded_process, def_excluded_process_list);
    parse_set(def_mon_settings.included_pids, def_included_pid_list);
    parse_set(def_mon_settings.excluded_pids, def_excluded_pid_list);

    // bool: Cant use both filters.
    mon_settings.use_included_filter = ini_put_or_get<bool>(pt, "AudioMonitor.use_included_filter", def_mon_settings.use_included_filter);

    // Get strings lists from ptree
    included_process_list = ini_put_or_get<std::string>(pt, "AudioMonitor.included_process", def_included_process_list);
    excluded_process_list = ini_put_or_get<std::string>(pt, "AudioMonitor.excluded_process", def_excluded_process_list);
    included_pid_list = ini_put_or_get<std::string>(pt, "AudioMonitor.included_pids", def_included_pid_list);
    excluded_pid_list = ini_put_or_get<std::string>(pt, "AudioMonitor.excluded_pids", def_excluded_pid_list);
              
    // clear to overwrite current process values
    mon_settings.included_process.clear();
    mon_settings.excluded_process.clear();
    parse_process_list(included_process_list, mon_settings.included_process);
    parse_process_list(excluded_process_list, mon_settings.excluded_process);

    // clear to overwrite current pid values
    mon_settings.included_pids.clear();
    mon_settings.excluded_pids.clear();
    parse_pid_list(included_pid_list, mon_settings.included_pids);
    parse_pid_list(excluded_pid_list, mon_settings.excluded_pids);

#ifdef _DEBUG
    dprintf("\n\n\n\n");
    for (auto& section : pt)
    {
        std::cout << "section.first= " << '[' << section.first << "]\n";
        for (auto& key : section.second)
            std::cout << "key.first="<< key.first << "    key.second.get_value<std::string>()=" << key.second.get_value<std::string>() << "\n";
    }
    dprintf("\n\n\n\n");
#endif

    return parsed_settings;
}

/*
    wstring set to string list separated by ;
*/
void parse_set(const std::set<std::wstring>& set_s, std::string& process_list)
{
    for (auto s : set_s)
    {
        process_list += wstring_to_utf8(s) + ";";
    }
}

/*
    int set to string list separated by ;
*/
void parse_set(const std::set<unsigned long>& set_l, std::string& pid_list)
{
    for (auto l : set_l)
    {
        pid_list += std::to_string(l) + ";";
    }
}

/*
    wstring list separated by ; to wstring set
*/
void parse_process_list(const std::wstring& process_list, std::set<std::wstring>& set_s)
{
    // NOTE_TODO: remove trimming if the user wants trailing spaces on names, or add "" on ini per value.
    typedef boost::tokenizer<boost::char_separator<wchar_t>, std::wstring::const_iterator, std::wstring> wtokenizer;
    boost::char_separator<wchar_t> sep(L";");
    wtokenizer processtokens(process_list, sep);
    for (auto it = processtokens.begin(); it != processtokens.end(); ++it)
    {
        std::wstring pname(*it);
        boost::algorithm::trim(pname);

        dwprintf(L"parse_process_list: insert: %s\n\n", pname.c_str());
        set_s.insert(pname);
    }
}

/*
    string list separated by ; to wstring set
*/
void parse_process_list(const std::string& process_list, std::set<std::wstring>& set_s)
{
    // NOTE_TODO: remove trimming if the user wants trailing spaces on names, or add "" on ini per value.
    boost::char_separator<char> sep(";");
    boost::tokenizer<boost::char_separator<char>> processtokens(process_list, sep);
    for (auto it = processtokens.begin(); it != processtokens.end(); ++it)
    {
        std::string pname(*it);
        boost::algorithm::trim(pname);

        dprintf("parse_process_list: insert: %s\n\n", pname.c_str());
        set_s.insert(utf8_to_wstring(pname));
    }
}

/*
    string list separed by ; to int set
*/
void parse_pid_list(const std::string& pid_list, std::set<unsigned long>& set_l)
{
    boost::char_separator<char> sep(";");
    boost::tokenizer<boost::char_separator<char>> pidtokens(pid_list, sep);

    for (auto it = pidtokens.begin(); it != pidtokens.end(); ++it)
    {
        try
        {
            std::string spid(*it);
            boost::algorithm::trim(spid);

            dprintf("parse_pid_list: insert: %s\n\n", spid.c_str());
            set_l.insert(std::stoi(spid));
        }
        catch (std::invalid_argument) {} // std::stoi TODO: return something and report it to log
        catch (std::out_of_range) {} // std::stoi TODO: return something and report it to log
    }
}

void VolumeOptions::set_settings(vo::volume_options_settings& settings)
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    // monitor_settings will be parsed, applied and updated with actual settings applied.
    m_paudio_monitor->SetSettings(settings.monitor_settings);

    m_vo_settings = settings;
}

/*
    Shortcut to get current global audio session volume change.
*/
float VolumeOptions::get_global_volume_reduction() const
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    // gets the global vol reduction.
    return m_paudio_monitor->GetVolumeReductionLevel();
}

/*
    Restores audio sessions valume back to user default using AudioMonitor lib.
*/
void VolumeOptions::restore_default_volume()
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    printf("VO_PLUGIN: Forcing restore per app user default volume.\n");

    m_paudio_monitor->Stop();
}

/* 
    Resets all data back to default 
    WARNING: if used while clients are talking we will get incorrent talking counts
        until everybody stops talking.
*/
void VolumeOptions::reset_data() // Not used
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    printf("VO_PLUGIN: Reseting talk data.\n");

    reset_all_clients_settings();
    reset_all_channels_settings();

    if (!m_clients_talking.empty())
        m_clients_talking.clear();

    if (!m_channels_with_activity.empty())
        m_channels_with_activity.clear();

    VolumeOptions::restore_default_volume();
}

void VolumeOptions::set_status(const status newstatus)
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

vo::volume_options_settings VolumeOptions::get_current_settings() const
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    return m_vo_settings;
}

VolumeOptions::status VolumeOptions::get_status() const
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    return m_status;
}

VolumeOptions::status VolumeOptions::get_channel_status(const uniqueServerID_t uniqueServerID, const channelID_t nonunique_channelID) const
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    uniqueChannelID_t uniqueChannelID(get_unique_channelid(uniqueServerID, nonunique_channelID));

    if (m_ignored_channels.count(uniqueChannelID))
        return DISABLED;

    return ENABLED;
}

VolumeOptions::status VolumeOptions::get_client_status(const uniqueClientID_t clientID) const
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    if (m_ignored_clients.count(clientID))
        return DISABLED;

    return ENABLED;
}

/*
    Erases all saved data on ignored channels
*/
void VolumeOptions::reset_all_channels_settings()
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    // if nothing to reset, return
    if (m_ignored_channels.empty())
    {
        dprintf("VO_PLUGIN: All channels at default status.\n");
        return;
    }

    m_ignored_channels.clear();

    // Now move all disabled channels currently with activity back to enabled.
    if (!m_channels_with_activity[DISABLED].empty())
    {
        // move all channels
        for (auto it : m_channels_with_activity[DISABLED])
        {
            m_channels_with_activity[ENABLED][it.first] =
                std::move(m_channels_with_activity[DISABLED][it.first]);
        }

        m_channels_with_activity[DISABLED].clear();
    }

    dprintf("VO_PLUGIN: All channels settings cleared.\n");

    // Update statuses
    apply_status();
}

/*
    Erases all saved data on ignored clients
*/
void VolumeOptions::reset_all_clients_settings()
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    // if nothing to reset, return
    if (m_ignored_clients.empty())
    {
        dprintf("VO_PLUGIN: All clients at default status.\n");
        return;
    }

    m_ignored_clients.clear();

    // Now move all currently disabled talking clients back to enabled.
    if (!m_clients_talking[DISABLED].empty())
    {
        // move all clients
        for (auto client : m_clients_talking[DISABLED])
        {
            m_clients_talking[ENABLED].insert(client);
        }

        m_channels_with_activity[DISABLED].clear();
    }

    dprintf("VO_PLUGIN: All clients settings cleared.\n");

    // Update statuses
    apply_status();

}

/*
    TS3 doesnt provide a unique channel id because it always belongs to a server, here we make a unique id
        from these two elements, unique virtual server id plus local channel id.
*/
inline VolumeOptions::uniqueChannelID_t VolumeOptions::get_unique_channelid(const uniqueServerID_t& uniqueServerID,
    const channelID_t& nonunique_channelID) const
{
    // combine unique serverID with nonunique_channelid as a string to make a uniqueChannelID (somewhat)
    return uniqueServerID + "-" + std::to_string(nonunique_channelID); // Profiler only hot zone ~6%
}

/*
    Marks channels as disabled for auto volume change when volume options is running

    We need the unique server ID from where this channel is.
*/
void VolumeOptions::set_channel_status(const uniqueServerID_t uniqueServerID, const channelID_t channelID,
    const status s)
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    uniqueChannelID_t uniqueChannelID(get_unique_channelid(uniqueServerID, channelID));

    // Move channels from containers to tag them if they have activity (someone inside the channel is talking)

    if (s == status::DISABLED)
    {
        // if already ignored return
        if (m_ignored_channels.count(uniqueChannelID))
        {
            printf("VO_PLUGIN: Channel %s Status: Already Disabled\n", uniqueChannelID.c_str());
            return;
        }

        m_ignored_channels.insert(uniqueChannelID);

        // if channel currently has activity move it and all his clients to disabled.
        if (m_channels_with_activity[ENABLED].count(uniqueChannelID))
        {
            // move it.
            m_channels_with_activity[DISABLED][uniqueChannelID] =
                std::move(m_channels_with_activity[ENABLED][uniqueChannelID]);
            m_channels_with_activity[ENABLED].erase(uniqueChannelID);
        }
    }
    if (s == status::ENABLED)
    {
        // if already enabled return
        if (!m_ignored_channels.count(uniqueChannelID))
        {
            printf("VO_PLUGIN: Channel %s Status: Already Enabled\n", uniqueChannelID.c_str());
            return;
        }

        m_ignored_channels.erase(uniqueChannelID);

        // Now move the channel and and all his clients back to enabled if currently has activity.
        if (m_channels_with_activity[DISABLED].count(uniqueChannelID))
        {
            // move it.
            m_channels_with_activity[ENABLED][uniqueChannelID] =
                std::move(m_channels_with_activity[DISABLED][uniqueChannelID]);
            m_channels_with_activity[DISABLED].erase(uniqueChannelID);
        }
    }

    printf("VO_PLUGIN: Channel %s Status: %s\n", uniqueChannelID.c_str(),
        s == status::DISABLED ? "Disabled" : "Enabled");

    // Update statuses
    apply_status();
}

/*
    Marks clients as disabled for auto volume changes when volumeoptions is running.
*/
void VolumeOptions::set_client_status(const uniqueClientID_t uniqueClientID, const status s)
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    // Switch clients from containers to tag them if they are talking.

    if (s == status::DISABLED)
    {
        // if already ignored return
        if (m_ignored_clients.count(uniqueClientID))
        {
            printf("VO_PLUGIN: Client %s Status: Already Disabled\n", uniqueClientID.c_str());
            return;
        }

        m_ignored_clients.insert(uniqueClientID);

        // Move client to disabled status if currently talking.
        if (m_clients_talking[ENABLED].count(uniqueClientID))
        {
            // move it.
            m_clients_talking[ENABLED].erase(uniqueClientID);
            m_clients_talking[DISABLED].insert(uniqueClientID);
        }
    }
    if (s == status::ENABLED)
    {
        // if already enabled return
        if (!m_ignored_clients.count(uniqueClientID))
        {
            printf("VO_PLUGIN: Client %s Status: Already Enabled\n", uniqueClientID.c_str());
            return;
        }

        m_ignored_clients.erase(uniqueClientID);

        // Now move client back to enabled if currently talking
        if (m_clients_talking[DISABLED].count(uniqueClientID))
        {
            // move it.
            m_clients_talking[DISABLED].erase(uniqueClientID);
            m_clients_talking[ENABLED].insert(uniqueClientID);
        }
    }

    printf("VO_PLUGIN: Client %s Status: %s\n", uniqueClientID.c_str(),
        s == status::DISABLED ? "Disabled" : "Enabled");

    // Update statuses
    apply_status();
}

/*
    Starts or stops audio monitor based on ts3 talking statuses.

    If none of the enabled clients/channels are talking turn off audio monitor.
*/
int VolumeOptions::apply_status()
{
    int r = 1;

    // if last client non disabled stoped talking, restore sounds. 
    if ( m_clients_talking[ENABLED].empty() || m_channels_with_activity[ENABLED].empty() ) 
    {
        if (m_status == status::ENABLED)
        {
            if (m_paudio_monitor->GetStatus() != AudioMonitor::monitor_status_t::PAUSED) // so we dont repeat it.
            {
                dprintf("VO_PLUGIN: Audio Monitor Paused, restoring Sessions to user default volume...\n");
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
                    dprintf("VO_PLUGIN: Audio Monitor Active. starting/resuming audio sessions volume monitor...\n");
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

    talk_status     ->  true if currently talking, false otherwise
    uniqueServerID  ->  TS3 unique virtual server ID
    channelID       ->  Server local Channel ID (unique per server)
    uniqueClientID  ->  TS3 client unique ID
    ownclient       ->  optional TODO: remove it and add own client to ignored list.

    We use two sets:
    m_ignored_clients and m_ignored_channels -> stores marked clients and channels.
    m_clients_talking and m_channels_with_activity -> stores clientes and channels with someone currently talking.

    To make it easy and not deal with TS3 callbacks we simply track clients talking and store them, and when they
        stop talking we delete them.
    NOTE: When clients stops talking because they are moved or etc, ts3 onTalkStatusChange can contain the destination
        channel, not the origin, we correct that case here.
*/
int VolumeOptions::process_talk(const bool talk_status, const uniqueServerID_t uniqueServerID,
    const channelID_t channelID, const uniqueClientID_t uniqueClientID, const bool ownclient)
{
    int r = 1;

    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    // We mark channelIDs as unique combining its virtual server ID.
    uniqueChannelID_t uniqueChannelID(get_unique_channelid(uniqueServerID, channelID)); // Profiler only hot zone 6%
    
    // if this is mighty ourselfs talking, ignore after we stop talking to update.
    // TODO if user ignores himself well get incorrect count, fix it. revise this.
    if ((ownclient) && (m_vo_settings.exclude_own_client) && !m_clients_talking[ENABLED].count(uniqueClientID))
    {
        dprintf("VO_PLUGIN: We are talking.. do nothing\n");
        return r;
    }

    // NOTE: We assume TS3 will always send talk_status false when other clients disconnects, changes channel or etc.
    if (talk_status)
    {
        // Update client containers
        if (m_ignored_clients.count(uniqueClientID))
            m_clients_talking[DISABLED].insert(uniqueClientID);
        else
            m_clients_talking[ENABLED].insert(uniqueClientID);

        // Update channel containers
        if (m_ignored_channels.count(uniqueChannelID))
            m_channels_with_activity[DISABLED][uniqueChannelID].insert(uniqueClientID);
        else
            m_channels_with_activity[ENABLED][uniqueChannelID].insert(uniqueClientID);

#ifdef _DEBUG
        // Care with this debug comments not to create a key, use .at()
        size_t enabled_size = 0, disabled_size = 0;
        try { enabled_size = m_channels_with_activity.at(ENABLED).at(uniqueChannelID).size(); }
        catch (std::out_of_range) {}
        try { disabled_size = m_channels_with_activity.at(DISABLED).at(uniqueChannelID).size(); }
        catch (std::out_of_range) {}
        dprintf("VO_PLUGIN: Update: m_channels_with_activity[DISABLED][%s].size()= %llu\n",
            uniqueChannelID.c_str(), enabled_size);
        dprintf("VO_PLUGIN: Update: m_channels_with_activity[ENABLED][%s].size()= %llu\n",
            uniqueChannelID.c_str(), disabled_size);
#endif

    }
    else
    {
        // Delete them directly, we dont know here if cause of stop was disconnection, channel change etc
        if (m_ignored_clients.count(uniqueClientID))
            m_clients_talking[DISABLED].erase(uniqueClientID);
        else
            m_clients_talking[ENABLED].erase(uniqueClientID);


        // Substract client from channel count, if empty delete it.
        // TS3FIXNOTE: When a client is moved from a channel the talk status false has the new channel, not the old..
        // SELFNOTE: (i dont want to use ts3 callbacks for every case, a pain to mantain, concentrate all cases here)
        uniqueChannelID_t channelID_origin = uniqueChannelID;
        status channelID_origin_status;
        for (int istatus = 0; istatus < m_channels_with_activity.size(); istatus++)
        {   // 0 = status::DISABLED 1 = status::ENABLED
            channelID_origin_status = static_cast<status>(istatus);
            // low overhead, usualy a client is in as many channels as servers.
            for (auto it_cinfo : m_channels_with_activity[istatus])
            {
                if (it_cinfo.second.count(uniqueClientID))
                { 
                    channelID_origin = it_cinfo.first; 
                    goto done; // break nested with goto
                }
            }
        }
        done:
        // Now we got the real channel from where the client stopped talking, remove the client.
        m_channels_with_activity[channelID_origin_status][channelID_origin].erase(uniqueClientID);
        // if this was the last client from the channel talking delete the channel.
        if (m_channels_with_activity[channelID_origin_status][channelID_origin].empty())
            m_channels_with_activity[channelID_origin_status].erase(channelID_origin);

#ifdef _DEBUG
        // Care with this debug comments not to create a key, use .at()
        size_t enabled_size = 0, disabled_size = 0;
        try { enabled_size = m_channels_with_activity.at(ENABLED).at(uniqueChannelID).size(); }
        catch (std::out_of_range) {}
        try { disabled_size = m_channels_with_activity.at(DISABLED).at(uniqueChannelID).size(); }
        catch (std::out_of_range) {}
        dprintf("VO_PLUGIN: Update: m_channels_with_activity[ENABLED][%s].size()= %llu\n", uniqueChannelID.c_str(), enabled_size);
        dprintf("VO_PLUGIN: Update: m_channels_with_activity[DISABLED][%s].size()= %llu\n", uniqueChannelID.c_str(), disabled_size);
#endif

    }

#ifdef _DEBUG
    // Care with this debug comments not to create a key, use .at()
    size_t enabled_size = 0, disabled_size = 0;
    try { enabled_size = m_clients_talking.at(ENABLED).size();}
    catch (std::out_of_range) {}
    try { disabled_size = m_clients_talking.at(DISABLED).size();}
    catch (std::out_of_range) {}
    dprintf("VO_PLUGIN: Total Users currently talking (enabled): %llu\n", enabled_size);
    dprintf("VO_PLUGIN: Total Users currently talking (disabled): %llu\n\n", disabled_size);

    enabled_size = 0; disabled_size = 0;
    try { enabled_size = m_channels_with_activity.at(ENABLED).size();}
    catch (std::out_of_range) {}
    try { disabled_size = m_channels_with_activity.at(DISABLED).size();}
    catch (std::out_of_range) {}
    dprintf("VO_PLUGIN: Total Channels with activity (enabled): %llu\n", enabled_size);
    dprintf("VO_PLUGIN: Total Channels with activity (disabled): %llu\n\n", disabled_size);
#endif

    // Update audio monitor status
    apply_status();

    return r; // TODO error codes
}

} // end namespace vo
