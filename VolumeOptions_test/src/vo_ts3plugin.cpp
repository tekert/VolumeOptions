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


#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996) // conflict with ptree comparison boost::multi_index std::_Equal1
#endif 
#include <string>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cassert>
#include <fstream>
#include <iostream>

#include "stdio.h"
#include "stdlib.h"

// for ini parser
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
 
#include <boost/tokenizer.hpp>
#include <boost/algorithm/string.hpp>

#include "../volumeoptions/utilities.h"
#include "../volumeoptions/config.h"
#include "../volumeoptions/vo_ts3plugin.h"


namespace vo {


/////////////////////////	Team Speak 3 Interface	//////////////////////////////////

/*
    Will load supplied settings directly
*/
VolumeOptions::VolumeOptions(const vo::volume_options_settings& settings)
    : VolumeOptions()
{
    m_config.vo_settings = settings;
    // nothing to parse from here, audiomonitor settings will be parsed when set.
}

/* 
    Basic constructor, will load default settings always. 
*/
VolumeOptions::VolumeOptions()
    : m_someone_enabled_is_talking(false)
    , m_status(status::ENABLED)
{

    m_clients_talking.resize(2); // will hold VolumeOptions::status, 0 or 1
    m_channels_with_activity.resize(2); // will hold VolumeOptions::status, 0 or 1
}

VolumeOptions::~VolumeOptions()
{
    // Save settings on exit.
    if (!m_config_filename.empty())
        save_config(m_config);

    // AudioMonitor destructor will do a Stop and automatically restore volume.
    m_audio_monitors.clear();

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
    Tries to open configFile and set settings.

    It will create and/or update the file if some options are missing.
*/
int VolumeOptions::load_config_file(const std::string &configFile, bool create_if_notfound)
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
            set_config_file(configFile);
            save_config(m_config);
        }
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
            m_config = ptree_to_config(pt);

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

    // Load parsed or default config
    load_config(m_config);

    return ret;
}

/*
    Saves config to set filename
*/
void VolumeOptions::save_config(config_settings_t& config) const
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    // Converts current class settings to ptree
    boost::property_tree::ptree pt = config_to_ptree(config);

    try
    {
        boost::property_tree::write_ini(m_config_filename, pt);
    }
    catch (boost::property_tree::ini_parser::ini_parser_error)
    {
        assert(false);
    }
}

// Shortcuts (to be more readable)
inline
VolumeOptions::config_settings_t VolumeOptions::ptree_to_config(boost::property_tree::ptree& pt) const
{
    return parse_ptree(pt, m_config); // use m_config as default for missing config
}

inline
boost::property_tree::ptree VolumeOptions::config_to_ptree(const VolumeOptions::config_settings_t& settings) const
{
    boost::property_tree::ptree pt;
    parse_ptree(pt, m_config); // use m_config as origin for all missing config (pt is updated)

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

// stores or retrieves a set of strings from ptree (the set it parsed from a list of strings separated by semicolon)
// can only we used for: string, wstring or unsigned long
template <typename T>
std::set<T> ini_put_or_get_set(boost::property_tree::ptree& pt, const std::string& path,
    const std::set<T>& origin_set)
{
    std::set<T> parsed_set;

    // strings list to retrieve and strings list to use for default case
    std::string string_list, origin_string_list;
    // Convert set to list of strings separated by ; to use them in case of missing ptree value.
    parse_set(origin_set, origin_string_list);
    // Get strings lists from ptree, origin_string_list will be used if value is missing from ptree.
    string_list = ini_put_or_get<std::string>(pt, path, origin_string_list);
    // Parse string list separated by semicolon to set of strings or ints
    parse_string_list(string_list, parsed_set);

    return parsed_set;
}

/*
    Will parse ptree to settings or origin_config to ptree.

    If some options are missing from ptree, it will use origin_config as default.
    ptree will be updated with missing settings.
    If ptree is empty, origin_config can be used to parse seetings to ptree.

    returns parsed settings.
*/
VolumeOptions::config_settings_t VolumeOptions::parse_ptree(boost::property_tree::ptree& pt,
    const config_settings_t& origin_config) const
{
    using boost::property_tree::ptree;

    config_settings_t parsed_config;

    // settings shortcuts
    vo::session_settings& ses_settings = parsed_config.vo_settings.monitor_settings.ses_global_settings;
    vo::monitor_settings& mon_settings = parsed_config.vo_settings.monitor_settings;
    vo::volume_options_settings& vo_settings = parsed_config.vo_settings;
    const vo::session_settings& def_ses_settings = origin_config.vo_settings.monitor_settings.ses_global_settings;
    const vo::monitor_settings& def_mon_settings = origin_config.vo_settings.monitor_settings;
    const vo::volume_options_settings& def_vo_settings = origin_config.vo_settings;

    // Using boost property threes, boost::optional version used to fill missing config.
    // http://www.boost.org/doc/libs/1_57_0/doc/html/boost_propertytree/accessing.html

    // ------ Plugin Settings

    bool enabled = ini_put_or_get<bool>(pt, "global.enabled", m_status == status::DISABLED ? 0 : 1);
    if (!enabled) m_status = status::DISABLED;

    // bool: do we exclude ourselfs?
    vo_settings.exclude_own_client = ini_put_or_get<bool>(pt, "plugin.exclude_own_client", def_vo_settings.exclude_own_client);

    // Parse output device list to set of strings ('default' string means default device )
    parsed_config.selected_devices = ini_put_or_get_set<std::string>(pt, "plugin.monitor_output_devices", origin_config.selected_devices);


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

    // bool: Cant use both filters.
    mon_settings.use_included_filter = ini_put_or_get<bool>(pt, "AudioMonitor.use_included_filter", def_mon_settings.use_included_filter);

    // Retrieve set of string or int in case of pids (ini uses a list of strings separated by semicolon)
    mon_settings.included_process = ini_put_or_get_set<std::wstring>(pt, "AudioMonitor.included_process", def_mon_settings.included_process);
    mon_settings.excluded_process = ini_put_or_get_set<std::wstring>(pt, "AudioMonitor.excluded_process", def_mon_settings.excluded_process);
    mon_settings.included_pids = ini_put_or_get_set<unsigned long>(pt, "AudioMonitor.included_pids", def_mon_settings.included_pids);
    mon_settings.excluded_pids = ini_put_or_get_set<unsigned long>(pt, "AudioMonitor.excluded_pids", def_mon_settings.excluded_pids);

#ifdef _DEBUG
    dprintf("\n\n\n Parsed values from ptree:\n");
    for (auto& section : pt)
    {
        std::cout << "section.first= " << '[' << section.first << "]\n";
        for (auto& key : section.second)
            std::cout << "key.first="<< key.first << "    key.second.get_value<std::string>()=" << key.second.get_value<std::string>() << "\n";
    }
    dprintf("\n\n\n");
#endif

    return parsed_config;
}

/*
    wstring set to string list separated by ;
*/
void parse_set(const std::set<std::wstring>& set_s, std::string& list)
{
    for (auto s : set_s)
        list += wstring_to_utf8(s) + ";";
}

/*
    string set to string list separated by ;
*/
void parse_set(const std::set<std::string>& set_s, std::string& list)
{
    for (auto s : set_s)
        list += s + ";";
}

/*
    int set to string list separated by ;
*/
void parse_set(const std::set<unsigned long>& set_l, std::string& list)
{
    for (auto l : set_l)
        list += std::to_string(l) + ";";
}

/*
    wstring list separated by ; to wstring set
*/
void parse_string_list(const std::wstring& process_list, std::set<std::wstring>& set_s)
{
    // NOTE_TODO: remove trimming if the user wants trailing spaces on names, or add "" on ini per value.
    typedef boost::tokenizer<boost::char_separator<wchar_t>, std::wstring::const_iterator, std::wstring> wtokenizer;
    boost::char_separator<wchar_t> sep(L";");
    wtokenizer processtokens(process_list, sep);
    for (auto it = processtokens.begin(); it != processtokens.end(); ++it)
    {
        std::wstring pname(*it);
        boost::algorithm::trim(pname);

        dwprintf(L"parse_process_list: insert: %s\n", pname.c_str());
        set_s.insert(pname);
    }
}

/*
    string list separated by ; to wstring set
*/
void parse_string_list(const std::string& process_list, std::set<std::wstring>& set_s)
{
    // NOTE_TODO: remove trimming if the user wants trailing spaces on names, or add "" on ini per value.
    boost::char_separator<char> sep(";");
    boost::tokenizer<boost::char_separator<char>> processtokens(process_list, sep);
    for (auto it = processtokens.begin(); it != processtokens.end(); ++it)
    {
        std::string pname(*it);
        boost::algorithm::trim(pname);

        dprintf("parse_string_list: insert: %s\n", pname.c_str());
        set_s.insert(utf8_to_wstring(pname));
    }
}

/*
    string list separated by ; to string set
*/
void parse_string_list(const std::string& process_list, std::set<std::string>& set_s)
{
    // NOTE_TODO: remove trimming if the user wants trailing spaces on names, or add "" on ini per value.
    boost::char_separator<char> sep(";");
    boost::tokenizer<boost::char_separator<char>> processtokens(process_list, sep);
    for (auto it = processtokens.begin(); it != processtokens.end(); ++it)
    {
        std::string pname(*it);
        boost::algorithm::trim(pname);

        dprintf("parse_string_list: insert: %s\n", pname.c_str());
        set_s.insert(pname);
    }
}

/*
    string list separed by ; to int set
*/
void parse_string_list(const std::string& pid_list, std::set<unsigned long>& set_l)
{
    boost::char_separator<char> sep(";");
    boost::tokenizer<boost::char_separator<char>> pidtokens(pid_list, sep);

    for (auto it = pidtokens.begin(); it != pidtokens.end(); ++it)
    {
        try
        {
            std::string spid(*it);
            boost::algorithm::trim(spid);

            dprintf("parse_int_list: insert: %s\n", spid.c_str());
            set_l.insert(std::stoi(spid));
        }
        catch (std::invalid_argument) {} // std::stoi TODO: return something and report it to log
        catch (std::out_of_range) {} // std::stoi TODO: return something and report it to log
    }
}

/*
    TODO: rewrite and redesign this
*/
void VolumeOptions::set_settings(monitor_settings& settings, const std::string& device)
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    try { m_audio_monitors.at(device)->SetSettings(settings); }
    catch (std::out_of_range) {
        printf("VO_PLUGIN: set_settings()  Device %s not found.\n", device.c_str());
    }
}

/*
    Sets settings for VolumeOptions
    Applies settings for current and new audio monitors.
*/
void VolumeOptions::set_settings(volume_options_settings& settings)
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    m_config.vo_settings = settings;

    // TODO: individual settings per monitor
    // TODO: remove this when individual settings are done.
    for (auto m : m_audio_monitors)
        m.second->SetSettings(m_config.vo_settings.monitor_settings);
}

/*
    Load class state to resume VolumeOptions
*/
void VolumeOptions::load_config(VolumeOptions::config_settings_t& config)
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    m_config = config;

    set_settings(m_config.vo_settings);

    // Start AudioMonitors for configured devices.
    for (auto d : m_config.selected_devices)
    {
#ifdef _WIN32
        // default device id has to be empty when adding AudioMonitor
        if (d == "default")
        {
            if (!add_device_monitor(L""))
                m_config.selected_devices.erase("default");
        }
        else
        {
            if (!add_device_monitor(utf8_to_wstring(d)))
                m_config.selected_devices.erase(d);
        }
#endif
    }
}

#ifdef _WIN32

// TODO: load device from name or device id. make two methods
// TODO2: maybe rewrite and move this to DeviceManager

// Returns true y already added or added
// 'wdevice_id' must be empty to add current default device
bool VolumeOptions::add_device_monitor(const std::wstring& wdevice_id)
{
    bool added = false;

    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    // VolumeOptions internal container handles strings only
    // In this case we store IDs as keys, empty key if we store default device.
    const std::string device = wstring_to_utf8(wdevice_id);

    // If we want to re add the default device check if its the same as current system default
    if (device.empty() && m_audio_monitors.count(""))
    {
        // Get current system default output endpoint (ID -> name)
        std::pair<std::wstring, std::wstring> default_endpoint;
        AudioMonitor::GetDefaultEndpoint(default_endpoint);

        // Get current monitored default endpoint.
        std::wstring using_defid = m_audio_monitors[""]->GetDeviceID();

        // Compare, if current system default is different from monitored default
        // TODO: maybe auto handle this in a different class with system callbacks.
        //      if not, with this.. we have to re add the default every time it changes.
        if (using_defid == default_endpoint.first)
        {
            dwprintf(L"VO_PLUGIN: Already monitoring this default device:\n - Name: %s\n\n", 
                AudioMonitor::GetDeviceName(using_defid).c_str());
            return true; // already monitoring system default
        }
        else
        {
            dwprintf(L"VO_PLUGIN: Replacing default device for new one:\n%s\nfor:\n%s\n\n",
                AudioMonitor::GetDeviceName(using_defid).c_str(),
                AudioMonitor::GetDeviceName(default_endpoint.first).c_str());

            // delete old default to overwrite
            m_audio_monitors.erase("");
        }
    }
    else
    {   // If we are already monitoring this device, return
        if (m_audio_monitors.count(device))
        {
            dwprintf(L"VO_PLUGIN: Already monitoring this device:\n - Name: %s\n\n",
                AudioMonitor::GetDeviceName(wdevice_id).c_str());
            return true;
        }
    }

    // Create the audio monitor and send settings to parse, it will return parsed settings.
    std::shared_ptr<AudioMonitor> paudio_monitor = AudioMonitor::create(wdevice_id);
    AudioMonitor::monitor_error_t error = paudio_monitor->GetLastError();

    if (error == AudioMonitor::monitor_error_t::OK)
    {
        // TODO: individual settings per monitor
        paudio_monitor->SetSettings(m_config.vo_settings.monitor_settings);

        m_audio_monitors[device] = paudio_monitor;

        std::wstring used_id = paudio_monitor->GetDeviceID();
        wprintf(L"VO_PLUGIN: Added Device ID:\n%s\n - Name: %s\n\n", used_id.c_str(),
            AudioMonitor::GetDeviceName(used_id).c_str());

        added = true;
    }
    else
    {
        if (error == AudioMonitor::monitor_error_t::DEVICEID_IN_USE)
            added = true;

        if (error == AudioMonitor::monitor_error_t::DEVICE_NOT_FOUND)
            wprintf(L"VO_PLUGIN: Error Device ID %s doesn't exists on the system\n",
            wdevice_id.c_str());

        if (error == AudioMonitor::monitor_error_t::IOTHREAD_START_ERROR)
        {
            assert(false);
            wprintf(L"VO_PLUGIN: ERROR AudioMonitor main thread can't start\n");
        }
    }

    if (added)
    {
        // Update internal config.
        if (device.empty())
            m_config.selected_devices.insert("default");
        else
            m_config.selected_devices.insert(device);
    }

    return added;
}

void VolumeOptions::remove_device_monitor(const std::wstring& wdevice_id)
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    std::string device = wstring_to_utf8(wdevice_id);

    if (m_audio_monitors.count(device))
    {
        std::wstring used_id = m_audio_monitors[device]->GetDeviceID();
        wprintf(L"VO_PLUGIN: Removed Device ID: %s\n - Name: %s\n\n", used_id.c_str(),
            AudioMonitor::GetDeviceName(used_id).c_str());

        m_audio_monitors.erase(device);
    }

    // Update internal config.
    if (device.empty())
        m_config.selected_devices.erase("default");
    else
        m_config.selected_devices.erase(device);
}

// These return current monitored device IDs, even for default device.
void VolumeOptions::get_monitored_devices(std::set<std::wstring>& current_deviceids)
{
    for (auto m : m_audio_monitors)
        current_deviceids.insert(m.second->GetDeviceID());
}
#endif

// These two return current selected devices for monitoring
// in case of default returns 'default' string as key
void VolumeOptions::get_selected_devices(std::set<std::wstring>& selected_devices)
{
    for (auto d : m_config.selected_devices)
        selected_devices.insert(utf8_to_wstring(d));
}
void VolumeOptions::get_selected_devices(std::set<std::string>& selected_devices)
{
    for (auto d : m_config.selected_devices)
        selected_devices.insert(d);
}

/*
    Shortcut to get current global audio session volume change.
*/
float VolumeOptions::get_global_volume_reduction() const
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    // gets the global vol reduction.
    return m_config.vo_settings.monitor_settings.ses_global_settings.vol_reduction;
}

/*
    Restores audio sessions valume back to user default using AudioMonitor lib.
*/
void VolumeOptions::restore_default_volume()
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    printf("VO_PLUGIN: Forcing restore per app user default volume.\n");

    for (auto m : m_audio_monitors)
        m.second->Stop();
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
    {
        for (auto m : m_audio_monitors)
            m.second->Start();
    }

    // Stop AudioMonitor only if someone non disabled is currently talking
    if (m_someone_enabled_is_talking && (newstatus == status::DISABLED) && (m_status == status::ENABLED))
    {
        for (auto m : m_audio_monitors)
            m.second->Stop();
    }

    m_status = newstatus;
}

vo::volume_options_settings VolumeOptions::get_current_settings() const
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    return m_config.vo_settings;
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

void VolumeOptions::resume_monitors()
{
    for (auto m : m_audio_monitors)
    {
        if (m.second->GetStatus() != AudioMonitor::monitor_status_t::RUNNING) // so we dont repeat it.
        {
            dprintf("VO_PLUGIN: Audio Monitor Active. starting/resuming audio sessions volume monitor...\n");
            m.second->Start();
        }
    }
}

void VolumeOptions::pause_monitors()
{
    for (auto m : m_audio_monitors)
    {
        if (m.second->GetStatus() != AudioMonitor::monitor_status_t::PAUSED) // so we dont repeat it.
        {
            dprintf("VO_PLUGIN: Audio Monitor Paused, restoring Sessions to user default volume...\n");
            m.second->Pause();
            //m.second->Stop();
        }
    }
}

/*
    Starts or stops audio monitors based on ts3 talking statuses.

    If none of the enabled clients/channels are talking turn off audio monitor.
*/
int VolumeOptions::apply_status()
{
    int r = 1;

    // if last client non disabled stoped talking, restore sounds. 
    if (m_clients_talking[ENABLED].empty() || m_channels_with_activity[ENABLED].empty()) 
    {
        if (m_status == status::ENABLED)
            pause_monitors();

        m_someone_enabled_is_talking = false; // excluding disabled
    }
    else // someone non disabled is talking
    {
        // if someone non disabled talked while audio monitor was down, start it
        if (!m_someone_enabled_is_talking)
        {
            if (m_status == status::ENABLED)
                resume_monitors();

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
    if ((ownclient) && (m_config.vo_settings.exclude_own_client) && !m_clients_talking[ENABLED].count(uniqueClientID))
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
        for (std::size_t istatus = 0; istatus < m_channels_with_activity.size(); istatus++)
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
