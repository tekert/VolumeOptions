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

	void save_default_volume();
	void restore_default_volume();
	void set_volume_reduction(const float v);
	float get_volume_reduction();

	void reset_data(); // not used.

private:
	AudioSessions m_as;

	DWORD m_cpid; // current process id, to filter. TODO: better filtering

	// Default user setting to reduce volume
	float m_vol_reduction;

	std::stack<bool> m_calls; // to count concurrent users talking
	bool m_quiet; // if noone is talking, this is true

	std::recursive_mutex m_mutex;
};

// C++11 Standard conversions
std::wstring utf8_to_wstring(const std::string& str);
std::string wstring_to_utf8(const std::wstring& str);



#endif