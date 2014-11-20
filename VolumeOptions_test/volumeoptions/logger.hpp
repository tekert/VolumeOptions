#ifndef LOGGER_HPP
#define LOGGER_HPP

// VolumeOptions logger

#include <iostream>
#include <memory>
#include <string>
#include <fstream>
#include <stdexcept>
#include <ctime>
#include <sstream>
#include <mutex>
#include <chrono>
#include <cstring>
#include <thread>
#include <map>
#include <queue>
#include <atomic>
#include <cassert>

#if 0
#include <mutex>
#include <string>
#include <fstream>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <thread>

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

    logger(std::string const& filename, bool append)
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


    //   print_impl(std::forward<std::stringstream>(log_header_test), std::forward<pt>(args)...);


    void print_impl(std::stringstream&& log_stream)
    {
        std::lock_guard<std::mutex> lock(write_mutex);

        static_cast<writer_type*>(this)->write(log_stream.str());
    }

    template< typename First, typename...Rest >
    void print_impl(std::stringstream&& log_stream, First&& parm1, Rest&&...parm)
    {
        log_stream << parm1;
        print_impl(std::forward<std::stringstream>(log_stream), std::move(parm)...);
    }

    template <class T>
    logger& operator<<(T const& v)
    {
        std::mutex::guard_lock l(file_mutex);
        open(false);
        log_file << get_header();

        log_file << v;
        return *this;
    }

    std::string m_filename;
};
#endif

#if 0

namespace logging
{


    /*
        We have some options modeling different logger writers:
        (The thing is, i dont like using abstract types (virtual methods) on loggers.. it doesnt seems right (1 and 2)
            for this particular case. I prefer templates (options 3 and 4)).


        1.- Define an abstract class and derive writers from that
                then make the main logger use the abstract class:

                template<typename T>
                class writer_type_interface1 {
                public:
                    virtual void write(const T&) = 0;
                };

                class file_writer1 : public writer_type_interface1<std::string> {
                public:
                    void write(const std::string &msg) { printf("%s", msg.c_str()); }
                };
                class tring_writer1 : public writer_type_interface1<std::set<int>> {
                public:
                    void write(const std::set<int> &msg) {  }
                };

                template <typename writer_type1>
                class logger1 {
                public:
                    template<typename T>
                    void print(const T &msg) { writer_type.write(msg); }
                private:
                    writer_type1 writer_type;
                };

                use:
                    logger1<file_writer1> fl1;
                    fl1.print(std::string("test"));
                    logger1<thing_writer1> tl1;
                    tl1.print(a_set);

                (downside is virtual table indirection...)

       2.- Derive directly from main logger class (polymorphic):

                class logger2 {
                public:
                    void print(std::string &msg) { write(msg); }
                protected:
                    // we cant template it
                    virtual void write(std::string &msg) = 0;
                };

                class file_writer2 : public logger2 {
                protected:
                    // we are forced to use abstract type
                    virtual void write(std::string &msg) { printf("%s", msg.c_str()); }
                };

                use:
                    logger2 *fl2 = new file_writer2();
                    fl2->print(std::string("test"));

                (problems with templates if using pure virtual and polymorphic)

        3.- Make main logger a derived class from writers using a template:

                template<typename writer_type3>
                class logger3 : public writer_type3
                {
                public:
                    template <typename t>
                    void print(t a) { this->write(a);  }
                };

                class file_writer3 {
                protected:
                    void write(std::string &msg) {  do something with msg }
                };
                class thing_writer3 {
                protected:
                    void write(std::set<int> &msg) { do something with msg }
                };

                use:
                    logger3<file_writer3> fl3;
                    fl3.print(in_this_case_an_string);
                    logger3<thing_writer3> tl3;
                    tl3.print(in_this_case_a_set<int>);

                (only downside is we loose the polymorphic characteristic of logger, upside: no virtuals)
                
        4.- Using Curiously Reoccurring Template Pattern from logger class:

                template<typename writer_type4>
                class logger4 {
                public:
                    template <typename t>
                    void print(t a) {
                        static_cast<writer_type4*>(this)->write(a);
                    }
                };

                class file_writer4 : public logger4<file_writer4> {
                public:
                    void write(std::string &msg) {  do something with msg }
                };
                class thing_writer4 : public logger4<thing_writer4> {
                public:
                    void write(std::set<int> &msg) {  do something with msg }
                };

                (no important downsides, only we cant protect write)

                use:
                    file_writer4 fl4;
                    fl4.print(std::string(""));
                    logger4<thing_writer4> tl4;
                    tl4.print(a_set);
                    logger4<file_writer4> *pfl4 = new file_writer4();
                    pfl4->print(std::string(""));
    */

    /*
        Using method 4 explained above
    */

    enum severity_type
    {
        verbose = 0,
        debug,
        error,
        warning,
    };

    /*
    * the Logger class, shall be instantiated with a specific writer_type
    */
    template<typename writer_type>
    class logger
    {
    public:
        logger();
        ~logger();

        template< severity_type severity, typename pt1, typename...pt >
        void print(pt1&& func, pt&&...args);

        void set_thread_name(const std::string& name);
        void terminate_logger();

    private:

        std::chrono::high_resolution_clock::time_point reference_epoch;

        std::mutex write_mutex;

        //Core printing functionality
        void print_impl(std::stringstream&&);
        template<typename First, typename...Rest>
        void print_impl(std::stringstream&&, First&& parm1, Rest&&...parm);

        std::map<std::thread::id, std::string> thread_name;
    };

    /*
    * Implementation which allow to write into a file
    */
    class textfile_log_writer : public logger<textfile_log_writer>
    {
        std::ofstream m_log_file;
        std::string m_filename;

        void open(bool truncate);
        void close();
    public:

        typedef std::string element_type;

        textfile_log_writer(const std::string& filename);
        ~textfile_log_writer();

        void write(const std::string& msg);
    };

    /*
    * Implementation for logger
    */

    template< typename writer_type >
    void logger< writer_type >::terminate_logger()
    {

    }

    template< typename writer_type >
    void logger< writer_type >::set_thread_name(const std::string& name)
    {
        thread_name[std::this_thread::get_id()] = name;
    }

    template< typename writer_type >
    template< severity_type severity, typename pt1, typename...pt >
    void logger< writer_type >::print(pt1&& func, pt&&...args)
    {
        std::stringstream log_header;
        std::stringstream log_header_test;

        if (0)
        {
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

            log_header << tt_s << " - ";
            log_header << std::chrono::duration_cast<std::chrono::milliseconds>(cur_time - reference_epoch).count() << "ms > ";
        }

        log_header << "[";
        switch (severity)
        {
        case severity_type::verbose:
            log_header << "";
            break;
        case severity_type::debug:
            log_header << "DEBUG/";
            break;
        case severity_type::warning:
            log_header << "WARNING/";
            break;
        case severity_type::error:
            log_header << "ERROR/";
            break;
        };

        log_header << thread_name[std::this_thread::get_id()] << "]   ";
        log_header << func << " : ";

        log_header_test.fill(' ');
        log_header_test.width(30);
        log_header_test << std::left << log_header.str();
        // header end

        print_impl(std::forward<std::stringstream>(log_header_test), std::forward<pt>(args)...);

    }

    template< typename writer_type >
    void logger< writer_type >::print_impl(std::stringstream&& log_stream)
    {
        std::lock_guard<std::mutex> lock(write_mutex);
        // CRTP pattern
        std::cout << log_stream.str() << std::endl;
        //static_cast<writer_type*>(this)->write(log_stream.str());
    }

    template< typename writer_type >
    template< typename First, typename...Rest >
    void logger< writer_type >::print_impl(std::stringstream&& log_stream, First&& parm1, Rest&&...parm)
    {
        log_stream << parm1;
        print_impl(std::forward<std::stringstream>(log_stream), std::move(parm)...);
    }

    template< typename writer_type >
    logger< writer_type >::logger()
    {
        reference_epoch = std::chrono::high_resolution_clock::now();
    }

    template< typename writer_type >
    logger< writer_type >::~logger()
    {
        static_cast<writer_type*>(this)->write("- Logger activity terminated -");
    }
}


// DEFINES

extern logging::textfile_log_writer log_inst;

#ifdef LOGGING
template <logging::severity_type severity, typename... pt>
void LOG_DIRECT(pt&&... args)
{
    log_inst.print<severity>(std::forward<pt>(args)...);
}

#define LOG(x) \
{ \
    LOG_DIRECT<logging::severity_type::verbose>(__FUNCTION__, x); \
}
#define LOG_DBG(x) \
{ \
    LOG_DIRECT<logging::severity_type::debug>(__FUNCTION__, x); \
}
#define LOG_ERR(x) \
{ \
    LOG_DIRECT<logging::severity_type::error>(__FUNCTION__, x); \
}
#define LOG_WARN(x) \
{ \
    LOG_DIRECT<logging::severity_type::warning>(__FUNCTION__, x); \
}


#else
#define LOG(...)
#define LOG_DBG(...) 
#define LOG_ERR(...)
#define LOG_WARN(...)
#endif

/*
#ifdef LOGGING_LEVEL_1
#define LOG log_inst.print< logging::severity_type::debug >
#define LOG_ERR log_inst.print< logging::severity_type::error >
#define LOG_WARN log_inst.print< logging::severity_type::warning >
#else
#define LOG(...) 
#define LOG_ERR(...)
#define LOG_WARN(...)
#endif

#ifdef LOGGING_LEVEL_2
#define ELOG log_inst.print< logging::severity_type::debug >
#define ELOG_ERR log_inst.print< logging::severity_type::error >
#define ELOG_WARN log_inst.print< logging::severity_type::warning >
#else
#define ELOG(...) 
#define ELOG_ERR(...)
#define ELOG_WARN(...)
#endif
*/

#endif

#endif
