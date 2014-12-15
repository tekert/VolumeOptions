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

#ifndef SOUND_PLUGIN_H
#define SOUND_PLUGIN_H

#include <fstream>
#include <mutex>
#include <set>
#include <unordered_set>
#include <list>
#include <unordered_map>
#include <string>

#include "stdint.h"

#include <boost/property_tree/ptree.hpp>

#ifdef _WIN32
#include "../volumeoptions/audiomonitor_wasapi.h"
#endif
#include "../volumeoptions/vo_settings.h"

namespace vo {

// Client Interface for Team Speak 3
class VolumeOptions
{
public:

    // settings will be used as default if no settings is provided when adding monitors.
    VolumeOptions(const volume_options_settings& settings);
    VolumeOptions();
    ~VolumeOptions();

    enum status { DISABLED = 0, ENABLED};

    // TS3 Virtual Server unique ID
    typedef std::string uniqueServerID_t;

    // TS3 global client unique ID
    typedef std::string uniqueClientID_t;

    // TS3 server uniqueID plus channel ID as a string :
    //      "<uniqueServerID_t><space><channelID_t>"
    typedef std::string uniqueChannelID_t;

    // TS3 local server channel ID
    typedef uint64_t channelID_t;

    // talk status, true if talking, false if not talking anymore. optional ownclient = true if we are talking
    int process_talk(const bool talk_status, const uniqueServerID_t uniqueServerID, const channelID_t channelID,
        const uniqueClientID_t uniqueClientID, const bool ownclient = false);

#ifdef _WIN32
    bool add_device_monitor(const std::wstring& deviceid);
    void remove_device_monitor(const std::wstring& deviceid);

    void get_monitored_devices(std::set<std::wstring>& current_deviceids);
#endif

    void get_selected_devices(std::set<std::wstring>& selected_devices);
    void get_selected_devices(std::set<std::string>& selected_devices);

    volume_options_settings get_current_settings() const;
    monitor_settings get_current_settings(const std::string& device) const;
    void set_settings(volume_options_settings& settings);
    void set_settings(monitor_settings& settings, const std::string& device = "");

    int load_config_file(const std::string &configIniFile, bool create_if_notfound = false);
    void set_config_file(const std::string &configIniFile);

    void restore_default_volume();
    float get_global_volume_reduction() const;
    void reset_data(); /* not used */

    void set_status(const status s);
    status get_status() const;

    void set_channel_status(const uniqueServerID_t uniqueServerID, const channelID_t channelID, const status s);
    status get_channel_status(const uniqueServerID_t uniqueServerID, const channelID_t channelID) const;
    void reset_all_channels_settings();

    void set_client_status(const uniqueClientID_t uniqueClientID, const status s);
    status get_client_status(const uniqueClientID_t uniqueClientID) const;
    void reset_all_clients_settings();

    // returns server uniqueID plus channel ID as a string: "<uniqueServerID_t><space><channelID_t>"
    inline VolumeOptions::uniqueChannelID_t get_unique_channelid(const uniqueServerID_t& uniqueServerID,
        const channelID_t& nonunique_channelID) const;

private:

    struct config_settings_t;

    config_settings_t parse_ptree(boost::property_tree::ptree& pt,
        const config_settings_t& origin_settings = config_settings_t()) const;
    inline config_settings_t ptree_to_config(boost::property_tree::ptree& pt) const;
    inline boost::property_tree::ptree config_to_ptree(const config_settings_t& settings) const;

    // Will load or save state from config object
    void load_config(config_settings_t& config);
    void save_config(config_settings_t& config) const;

    void resume_monitors();
    void pause_monitors();
    int apply_status(); // starts or stops audio monitor based on ts3 talking statuses.

    //std::shared_ptr<AudioMonitor> m_paudio_monitor;
    // map of deviceids or output device name to audio monitor.
    std::unordered_map<std::string, std::shared_ptr<AudioMonitor>> m_audio_monitors;

    // Used when saving settings to disk
    // On load, this is used to resume selected audio monitors and settings
    struct config_settings_t
    {
        config_settings_t()
        {
            //selected_devices.insert("default");
            selected_devices.push_front("default");
        }

        // Stores current settings to save
        volume_options_settings vo_settings;

        //  std::unordered_map<std::string, vo::monitor_settings> device_monitor_settings;

        // Stores currently monitored devices to save (on windows this stores device ids)
        std::list<std::string> selected_devices;
    };
    config_settings_t m_config;

    // current disabled and enabled clients talking
    std::vector<std::unordered_set<uniqueClientID_t>> m_clients_talking; // 0 = status::DISABLED, 1 = status::ENABLED
    // clients marked as disabled
    std::unordered_set<uniqueClientID_t> m_ignored_clients;

    typedef std::unordered_map<uniqueChannelID_t, std::unordered_set<uniqueClientID_t>> channel_info;
    // current enabled and disabled channels with activity (someone talking in it)
    std::vector<channel_info> m_channels_with_activity; // 0 = status::DISABLED, 1 = status::ENABLED
    // channels marked as disabled
    std::unordered_set<uniqueChannelID_t> m_ignored_channels;

    mutable status m_status;
    bool m_someone_enabled_is_talking;

    std::string m_config_filename;

    mutable std::recursive_mutex m_mutex;
};


// free functions to help parsing of process and pid names in a string separated by ";"

// strings list separed by ";"  to  set
void parse_string_list(const std::string& process_list, std::list<std::wstring>& list_s, bool remove_duplicates = true);
void parse_string_list(const std::wstring& process_list, std::list<std::wstring>& list_s, bool remove_duplicates = true);
void parse_string_list(const std::string& process_list, std::list<std::string>& list_s, bool remove_duplicates = true);
void parse_string_list(const std::string& pid_list, std::list<unsigned long>& list_l, bool remove_duplicates = true);

// set  to  strings list separed by ";"
void parse_list(const std::list<std::wstring>& list_s, std::string& list);
void parse_list(const std::list<std::string>& list_s, std::string& list);
void parse_list(const std::list<unsigned long>& list_l, std::string& list);


} // end namespace vo


#endif