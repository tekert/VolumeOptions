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

#ifndef SOUND_PLUGIN_H
#define SOUND_PLUGIN_H

#include <iostream>
#include <stack>
#include <mutex>

#ifdef _WIN32
#include "sounds_windows.h"
#endif


struct volume_options_settings
{
    volume_options_settings()
        : excude_own_client(true)
    {}

    vo::monitor_settings monitor_settings;

    // add extra settings for your inteface.
    bool excude_own_client;
};


// Client Interface for Team Speak 3
class VolumeOptions
{
public:

    VolumeOptions(const volume_options_settings& settings, const std::string &sconfigPath);
    ~VolumeOptions();

    enum status { DISABLED = 0, ENABLED};

    // talk status, true if talking, false if not talking anymore. optional ownclient = true if we are talking
    int process_talk(const bool talk_status, unsigned __int64 channelID, unsigned __int64 clientID,
        bool ownclient = false);

    volume_options_settings get_current_settings() const; // TODO: minimize copies
    void set_settings(volume_options_settings& settings);

    void restore_default_volume();
    float get_global_volume_reduction() const;
    void reset_data(); /* not used*/
    void set_status(status s);
    void set_channel_status(unsigned __int64 selectedItemID, status s);
    void set_client_status(unsigned __int64 channelID, status s);

private:

    void create_config_file(std::fstream& in);
    int parse_config(std::fstream& in);

    std::shared_ptr<AudioMonitor> m_paudio_monitor;

    volume_options_settings m_vo_settings;
    std::unordered_map<unsigned __int64, bool> m_disabled_channels;
    std::unordered_map<unsigned __int64, bool> m_disabled_clients;

    status m_status;

    std::stack<bool> m_calls; // to count concurrent users talking
    bool m_quiet; // if no one is talking, this is true

    /* not realy needed, teams speak sdk uses 1 thread per plugin on callbacks */
    mutable std::recursive_mutex m_mutex;
};

// C++11 Standard conversions
std::wstring utf8_to_wstring(const std::string& str);
std::string wstring_to_utf8(const std::wstring& str);



#endif