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

#ifndef VO_SETTINGS_H
#define VO_SETTINGS_H

#include <string>
#include <set>


namespace vo {

	// Library configurable settings
	struct monitor_settings
	{
		monitor_settings()
			: exclude_own_process(true)
			, vol_reduction(0.5f) 
			, use_included_filter(false)
			, treat_vol_as_percentage(true)
			, reduce_only_active_sessions(true)
		{}

		std::set<unsigned long> excluded_pids;		// process id blacklist
		std::set<std::wstring> excluded_process;	// process names blacklist
		std::set<unsigned long>	included_pids;		// process id whitelist
		std::set<std::wstring> included_process;	// process names whitelist

		std::map<std::wstring, unsigned long> selective_vol; //TODO: selective vol per process.

		bool reduce_only_active_sessions;
		bool treat_vol_as_percentage;
		bool use_included_filter; // cant use both, blacklist or whitelist
		bool exclude_own_process;
		float vol_reduction; // 0.0 to 1.0
	};

	struct client_settings
	{

	};
}
#endif