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


// For team speak
class VolumeOptions
{
public:

	VolumeOptions(const float v, const std::string &sconfigPath);
	~VolumeOptions();

	int process_talk(const bool talk_status);

	void restore_default_volume();
	void set_volume_reduction(const float v);
	float get_volume_reduction();
	void reset_data(); /* not used*/

private:
	std::shared_ptr<AudioMonitor> m_paudio_monitor;

	DWORD m_cpid; // current process id, to filter. TODO: settings class

	float m_vol_reduction; // Default user setting to reduce volume

	std::stack<bool> m_calls; // to count concurrent users talking
	bool m_quiet; // if no one is talking, this is true

	std::recursive_mutex m_mutex; /* not realy needed, teams speak sdk uses 1 thread per plugin on callbacks */
};

// C++11 Standard conversions
std::wstring utf8_to_wstring(const std::string& str);
std::string wstring_to_utf8(const std::wstring& str);



#endif