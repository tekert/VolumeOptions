

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <codecvt>
#include <fstream>

#include "../volumeoptions/sound_plugin.h"


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



////////////////////////////////////Team Speak Interface///////////////////////////////////////////


VolumeOptions::VolumeOptions(const float v, const std::string &sconfigPath)
: m_as()
{
	m_cpid = GetCurrentProcessId();
	m_quiet = true;
	m_vol_reduction = v;

	VolumeOptions::save_default_volume();

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
}

void VolumeOptions::set_volume_reduction(const float v)
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	m_vol_reduction = v;
}

float VolumeOptions::get_volume_reduction()
{
	return m_vol_reduction;
}

void VolumeOptions::restore_default_volume()
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	printf("VO: Restoring per app user default volume\n");

	m_as.RestoreSessions();
}

void VolumeOptions::save_default_volume()
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	printf("VO: Saving per app user default volume\n");

	m_as.ProcessSessions(m_cpid);
}

void VolumeOptions::reset_data()
{
	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	printf("VO: Reseting call data\n");
	while (!m_calls.empty())
		m_calls.pop();

	VolumeOptions::restore_default_volume();
}


int VolumeOptions::process_talk(const bool talk_status)
{
	HRESULT hr = S_OK;

	std::lock_guard<std::recursive_mutex> guard(m_mutex);

	// Count the number os users currently talking using a stack.
	// NOTE: we asume TS3Client always sends events in logical order per client
	if (talk_status)
		m_calls.push(1);
	else
		m_calls.pop();

	printf("VO: Update Users currently talking: %d\n", m_calls.size());

	// if last client stoped talking, restore sounds.
	if (m_calls.empty())
	{
		printf("VO: Restoring saved audio volume\n");
		hr = m_as.RestoreSessions();
		m_quiet = true;
	}

	// if someone talked while the channel was quiet, redudce volume (else was already lowered)
	if (!m_calls.empty() && m_quiet)
	{
		printf("VO: Saving and changing audio volume\n");
		hr = m_as.ProcessSessions(m_cpid, true, m_vol_reduction);
		m_quiet = false;
	}

	return 1; // TODO error codes
}
