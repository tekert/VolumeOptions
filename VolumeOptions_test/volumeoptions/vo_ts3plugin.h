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

#include <iostream>
#include <stack>
#include <mutex>
#include <unordered_set>
#include <string>

#include <boost/property_tree/ptree.hpp>

#include "stdint.h"

#ifdef _WIN32
#include "../volumeoptions/audiomonitor_wasapi.h"
#endif
#include "../volumeoptions/vo_settings.h"

namespace vo {

// Client Interface for Team Speak 3
class VolumeOptions
{
public:

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

    vo::volume_options_settings get_current_settings() const;
    void set_settings(vo::volume_options_settings& settings);
    int set_settings_from_file(const std::string &configIniFile, bool create_if_notfound = false);
    void set_config_file(const std::string &configFile);
    void save_settings_to_file(const std::string &configFile) const;

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
    inline uniqueChannelID_t get_unique_channelid(const uniqueServerID_t& uniqueServerID,
        const channelID_t& nonunique_channelID) const;

private:

    void common_init();

    void create_config_file(std::fstream& in);
    volume_options_settings parse_ptree(boost::property_tree::ptree& pt,
        const volume_options_settings& origin_settings = volume_options_settings()) const;
    inline volume_options_settings ptree_to_settings(boost::property_tree::ptree& pt) const;
    inline boost::property_tree::ptree settings_to_ptree(const volume_options_settings& settings) const;

    int apply_status(); // starts or stops audio monitor based on ts3 talking statuses.

    std::shared_ptr<AudioMonitor> m_paudio_monitor;

    vo::volume_options_settings m_vo_settings;

    /* current disabled and enabled clients talking */
    std::unordered_map<status, std::unordered_set<uniqueClientID_t>> m_clients_talking;
    /* clients marked as disabled */
    std::unordered_set<uniqueClientID_t> m_ignored_clients;

    typedef std::unordered_map<uniqueChannelID_t, std::unordered_set<uniqueClientID_t>> channel_info;
    /* current enabled and disabled channels with activity (someone talking in it) */ //TODO use vector?
    std::unordered_map<status, channel_info> m_channels_with_activity;
    /* channels marked as disabled */
    std::unordered_set<uniqueChannelID_t> m_ignored_channels;

    mutable status m_status;
    bool m_someone_enabled_is_talking;

    std::string m_configfile_name;

    /* not realy needed, teams speak sdk uses 1 thread per plugin on callbacks */
    mutable std::recursive_mutex m_mutex;
};


// free functions to help parsing of process and pid names in a string separated by ";"

// strings list separed by ";"  to  set
void parse_process_list(const std::string& process_list, std::set<std::wstring>& set_s);
void parse_process_list(const std::wstring& process_list, std::set<std::wstring>& set_s);
void parse_pid_list(const std::string& pid_list, std::set<unsigned long>& set_l);

// set  to  strings list separed by ";"
void parse_set(const std::set<std::wstring>& set_s, std::string& process_list);
void parse_set(const std::set<unsigned long>& set_l, std::string& pid_list);


} // end namespace vo


#endif