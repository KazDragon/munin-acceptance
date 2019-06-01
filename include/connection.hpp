#pragma once

#include "core.hpp"
#include <serverpp/core.hpp>
#include <functional>
#include <memory>

namespace serverpp {
class tcp_socket;
}

namespace ma {

//* =========================================================================
/// \brief An connection to a socket that abstracts away details about the
/// protocols used.
//* =========================================================================
class connection
{
public :
    //* =====================================================================
    /// \brief Create a connection object that uses the passed socket as
    /// a communications point, and calls the passed function whenever data
    /// is received.
    //* =====================================================================
    explicit connection(serverpp::tcp_socket &&socket);

    //* =====================================================================
    /// \brief Move constructor
    //* =====================================================================
    connection(connection &&other) noexcept;

    //* =====================================================================
    /// \brief Destructor.
    //* =====================================================================
    ~connection();

    //* =====================================================================
    /// \brief Move assignment
    //* =====================================================================
    connection &operator=(connection &&other) noexcept;

    //* =====================================================================
    /// \brief Returns whether the endpoint of the connection is still
    ///        alive.
    //* =====================================================================
    bool is_alive() const;

    //* =====================================================================
    /// \brief Asynchronously reads from the connection.
    ///
    /// A single read may yield zero or more callbacks to the data 
    /// continuation.  This is because parts or all of the data may be
    /// consumed by Telnet handling.  Therefore, a second continuation is
    /// provided to show that the requested read has been completed and a
    /// new read request may be issued.
    //* =====================================================================
    void async_read(
        std::function<void (serverpp::bytes)> const &data_continuation,
        std::function<void ()> const &read_complete_continuation);

    //* =====================================================================
    /// \brief Writes to the connection.
    //* =====================================================================
    void write(serverpp::bytes data);

    //* =====================================================================
    /// \brief Requests terminal type of the connection, calling the
    ///        supplied continuation with the results.
    //* =====================================================================
    void async_get_terminal_type(
        std::function<void (std::string const &)> const &continuation);

    //* =====================================================================
    /// \brief Set a function to be called when the window size changes.
    //* =====================================================================
    void on_window_size_changed(
        std::function<void (std::uint16_t, std::uint16_t)> const &continuation);

private :
    struct impl;
    std::unique_ptr<impl> pimpl_;
};

}
