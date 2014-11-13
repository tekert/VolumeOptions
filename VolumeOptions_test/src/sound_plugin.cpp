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



/////////////////////////	Team Speak Interface	//////////////////////////////////


VolumeOptions::VolumeOptions(const float v, const std::string &sconfigPath)
{
    vo::monitor_settings settings;

    //m_paudio_monitor = std::make_shared<AudioMonitor>(settings);
    m_paudio_monitor = AudioMonitor::create(settings);
    //m_paudio_monitor = std::shared_ptr<AudioMonitor>(new AudioMonitor(settings));

#ifdef VO_ENABLE_EVENTS
    m_paudio_monitor->InitEvents();
#endif
#ifdef _DEBUG
    m_paudio_monitor->Refresh();
#endif
    m_quiet = true;
    set_volume_reduction(v);

    // Create config file
    std::string configFile(sconfigPath + "\\volumeoptions_plugin.ini");
    std::fstream in(configFile);
    if (!in)
        in.open(configFile, std::fstream::out);
    if (!in)
        printf("VO: Error creating config file %s\n", configFile.c_str());

    in.close();
}

VolumeOptions::~VolumeOptions()
{
    VolumeOptions::restore_default_volume();

    m_paudio_monitor.reset();
}

void VolumeOptions::set_volume_reduction(const float v)
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    m_vol_reduction = v;

    if (m_vol_reduction < 0)
        m_vol_reduction = 0; // disabled

    if (m_vol_reduction > 1.0) // mute volume basically
        m_vol_reduction = 1.0;
}

float VolumeOptions::get_volume_reduction()
{
    return m_vol_reduction;
}

void VolumeOptions::restore_default_volume()
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    printf("VO_PLUGIN: Restoring per app user default volume\n");

    m_paudio_monitor->Stop();
}

void VolumeOptions::reset_data() // Utility, not realy used.
{
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    printf("VO_PLUGIN: Reseting call data\n");
    while (!m_calls.empty())
        m_calls.pop();

    VolumeOptions::restore_default_volume();
}

int VolumeOptions::process_talk(const bool talk_status)
{
    int r = 1;

    std::lock_guard<std::recursive_mutex> guard(m_mutex);

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
        printf("VO_PLUGIN: Monitoring Sessions Stopped, Restoring Sessions to default state...\n");
        //m_paudio_monitor->Stop();
        r = m_paudio_monitor->Pause();
        m_quiet = true;
    }

    // if someone talked while the channel was quiet, redudce volume (else was already lowered)
    if (!m_calls.empty() && m_quiet)
    {
        printf("\nVO_PLUGIN: Monitoring Sessions Active\n");
        r = m_paudio_monitor->Start();
        m_quiet = false;
    }

    return r; // TODO error codes
}
