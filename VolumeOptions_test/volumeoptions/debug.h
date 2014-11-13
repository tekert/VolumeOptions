
#ifndef VO_DEBUG_H
#define VO_DEBUG_H

#include "../volumeoptions/vo_config.h"

#include <mutex>
#include <string>
#include <fstream>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <thread>

#if 1
class logger
{
    // all log streams share a single file descriptor
    // and re-opens the file for each log line
    static std::ofstream log_file;
    static std::string open_filename;
    static std::mutex file_mutex;
    std::chrono::high_resolution_clock::time_point reference_epoch;
    std::unordered_map<std::thread::id, std::string> thread_name;

    void open(bool truncate)
    {
        if (open_filename == m_filename) return;
        log_file.close();
        log_file.clear();
        log_file.open(m_filename.c_str(), truncate ? std::ios_base::trunc : std::ios_base::app);
        open_filename = m_filename;
        if (!log_file.good())
            fprintf(stderr, "Failed to open logfile %s: %s\n", m_filename.c_str(), strerror(errno));
    }

public:

    ~logger()
    {
        std::lock_guard<std::mutex> l(file_mutex);
        log_file.close();
        open_filename.clear();
    }

    logger(std::string const& filename
        , int instance, bool append)
    {
        m_filename = filename;
        reference_epoch = std::chrono::high_resolution_clock::now();

        std::lock_guard<std::mutex> l(file_mutex);
        open(!append);
        log_file << "\n\n\n*** starting log ***\n";
    }

    enum severity_type
    {
        verbose = 0,
        debug,
        error,
        warning,
    };

    template< severity_type severity>
    void get_header(std::string& header)
    {
        std::stringstream log_header;
        //Prepare the header
        auto cur_time = std::chrono::high_resolution_clock::now();
        std::time_t tt = std::chrono::high_resolution_clock::to_time_t(cur_time);
#ifdef _WIN32
        char tt_s[26] = { 0 };
        errno_t err = ctime_s(tt_s, 26, &tt);
        assert(err == 0);
#else
        char* tt_s = ctime(&tt);
#endif
        tt_s[strlen(tt_s) - 1] = 0;

        log_header << " < " << tt_s << " - ";
        log_header << std::chrono::duration_cast< std::chrono::milliseconds >(cur_time - reference_epoch).count() << "ms > ";

        switch (severity)
        {
        case severity_type::verbose:
            log_header << "";
            break;
        case severity_type::debug:
            log_header << " DEBUG/";
            break;
        case severity_type::warning:
            log_header << " WARNING/";
            break;
        case severity_type::error:
            log_header << " ERROR/";
            break;
        };

        log_header << thread_name[std::this_thread::get_id()] << ", ";

        header = log_header.str();
    }

    template<typename...pt >
    void print(pt&&...args)
    {

    }


    template <class T>
    logger& operator<<(T const& v)
    {
        mutex::scoped_lock l(file_mutex);
        open(false);
        log_file << v;
        return *this;
    }

    std::string m_filename;
};
#endif


extern logger log_inst;

#define LOG_WARN3(x) \
{ \
    log_inst << LOG_WARN2(__FUNCTION__, x); \
}




#endif