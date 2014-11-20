#include "../volumeoptions/logger.hpp"

#if 0

logging::textfile_log_writer log_inst("execution.log");

// LOG OUTPUT IMPLEMENTATIONS

#include <cassert>

namespace logging
{
    textfile_log_writer::textfile_log_writer(const std::string& filepath)
        : m_filename(filepath)
    {
        open(true);
    }

    textfile_log_writer::~textfile_log_writer()
    {
        close();
    }

    void textfile_log_writer::open(bool truncate)
    {
        if (m_log_file.is_open()) return;
        m_log_file.close();
        m_log_file.clear();
        m_log_file.open(m_filename.c_str(), truncate ? std::ios_base::trunc : std::ios_base::app);
        assert(m_log_file.is_open() == true);
        if (!m_log_file.good())
            std::cerr << "Failed to open logfile %s: %s\n" << m_filename.c_str() << errno;
    }

    /*
    template<typename...pt>
    void textfile_log_writer::write(const std::string& log_header, std::string&&...args)
    {
        std::stringstream print;
        print_impl(std::forward<std::stringstream>(print), std::forward<Args>(args)...);

        open(false);
        m_log_file << log_header << print.str() << std::endl;
        m_log_file.flush();
    }*/

    void textfile_log_writer::write(const std::string& msg)
    {
        open(false);
        m_log_file << msg << std::endl;
        m_log_file.flush();
    }

    void textfile_log_writer::close()
    {
        if (m_log_file)
        {
            m_log_file.close();
        }
    }

}
#endif