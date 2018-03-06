#include "connection.hpp"
#include "socket.hpp"
#include <telnetpp/telnetpp.hpp>
#include <telnetpp/byte_converter.hpp>
#include <telnetpp/options/echo/server.hpp>
#include <telnetpp/options/mccp/codec.hpp>
#include <telnetpp/options/mccp/server.hpp>
#include <telnetpp/options/mccp/zlib/compressor.hpp>
#include <telnetpp/options/mccp/zlib/decompressor.hpp>
#include <telnetpp/options/naws/client.hpp>
#include <telnetpp/options/suppress_ga/server.hpp>
#include <telnetpp/options/terminal_type/client.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/placeholders.hpp>
#include <deque>
#include <string>
#include <utility>

namespace ma {

// ==========================================================================
// CONNECTION::IMPLEMENTATION STRUCTURE
// ==========================================================================
struct connection::impl
    : public std::enable_shared_from_this<impl>
{
    // ======================================================================
    // CONSTRUCTOR
    // ======================================================================
    impl(std::shared_ptr<ma::socket> const &socket)
      : socket_(socket),
        telnet_session_(
            [this](auto &&text) -> std::vector<telnetpp::token>
            {
                this->on_text(text);
                return {};
            }),
        telnet_mccp_codec_(
            std::make_shared<telnetpp::options::mccp::zlib::compressor>(),
            std::make_shared<telnetpp::options::mccp::zlib::decompressor>())
    {
        telnet_echo_server_.set_activatable();
        telnet_session_.install(telnet_echo_server_);
        
        telnet_suppress_ga_server_.set_activatable();
        telnet_session_.install(telnet_suppress_ga_server_);
         
        telnet_naws_client_.set_activatable();
        telnet_naws_client_.on_window_size_changed.connect(
            [this](auto &&width, auto &&height) -> std::vector<telnetpp::token>
            {
                this->on_window_size_changed(width, height);
                return {};
            });
        telnet_session_.install(telnet_naws_client_);

        telnet_terminal_type_client_.set_activatable();
        telnet_terminal_type_client_.on_terminal_type.connect(
            [this](auto &&type) -> std::vector<telnetpp::token>
            {
                this->on_terminal_type_detected(type);
                return {};
            });
        telnet_terminal_type_client_.on_state_changed.connect(
            [this](auto &&state) -> std::vector<telnetpp::token>
            {
                if (telnet_terminal_type_client_.is_active())
                {
                    return telnet_terminal_type_client_.request_terminal_type();
                }
                
                return {};
            });
        telnet_session_.install(telnet_terminal_type_client_);

        telnet_mccp_server_.set_activatable();
        write(telnet_session_.send(telnet_mccp_server_.begin_compression()));
        telnet_session_.install(telnet_mccp_server_);
        
        // Begin the keepalive process.  This sends regular heartbeats to the
        // client to help guard against his network settings timing him out
        // due to lack of activity.
        keepalive_timer_ =
            std::make_shared<boost::asio::deadline_timer>(
                std::ref(socket_->get_io_service()));
        schedule_keepalive();
        
        // Send the required activations.
        write(telnet_session_.send(telnet_echo_server_.activate()));
        write(telnet_session_.send(telnet_suppress_ga_server_.activate()));
        write(telnet_session_.send(telnet_naws_client_.activate()));
        write(telnet_session_.send(telnet_terminal_type_client_.activate()));
        write(telnet_session_.send(telnet_mccp_server_.activate()));
    }

    // ======================================================================
    // START
    // ======================================================================
    void start()
    {
        schedule_next_read();
    }
    
    // ======================================================================
    // WRITE
    // ======================================================================
    void write(std::vector<telnetpp::stream_token> const &data)
    {
        auto const &compressed_data = telnet_mccp_codec_.send(data);
        auto const &stream = telnet_byte_converter_.send(compressed_data);
        
        if (stream.size() != 0)
        {
            socket_->write({stream.begin(), stream.end()});
        }
        
    }

    // ======================================================================
    // SCHEDULE_NEXT_READ
    // ======================================================================
    void schedule_next_read()
    {
        if (!socket_->is_alive())
        {
            return;
        }

        auto available = socket_->available();
        auto amount = available 
                    ? *available 
                    : ma::socket::input_size_type{1};
                    
        socket_->async_read(
            amount,
            [this, amount](auto &&data)
            {
                this->on_data(data);
            });
    }

    // ======================================================================
    // ON_DATA
    // ======================================================================
    void on_data(std::vector<byte> const &data)
    {
        write(telnet_session_.send(
            telnet_session_.receive({data.begin(), data.end()})));
            
        schedule_next_read();
    }
    
    // ======================================================================
    // ON_KEEPALIVE
    // ======================================================================
    void on_keepalive(boost::system::error_code const &error)
    {
        if (!error && socket_->is_alive())
        {
            write(telnet_session_.send({
                    telnetpp::element(telnetpp::command(telnetpp::nop))
                }));

            schedule_keepalive();
        }
    }

    // ======================================================================
    // SCHEDULE_KEEPALIVE
    // ======================================================================
    void schedule_keepalive()
    {
        keepalive_timer_->expires_from_now(boost::posix_time::seconds(30));
        keepalive_timer_->async_wait(
            [this](auto const &error_code)
            {
                this->on_keepalive(error_code);
            });
    }

    // ======================================================================
    // ON_TEXT
    // ======================================================================
    void on_text(std::string const &text)
    {
        if (on_data_read_)
        {
            on_data_read_(text);
        }
    }

    // ======================================================================
    // ON_WINDOW_SIZE_CHANGED
    // ======================================================================
    void on_window_size_changed(std::uint16_t width, std::uint16_t height)
    {
        if (on_window_size_changed_)
        {
            on_window_size_changed_(width, height);
        }
    }

    // ======================================================================
    // ON_TERMINAL_TYPE_DETECTED
    // ======================================================================
    void on_terminal_type_detected(std::string const &type)
    {
        terminal_type_ = type;
        announce_terminal_type();
    }

    // ======================================================================
    // ANNOUNCE_TERMINAL_TYPE
    // ======================================================================
    void announce_terminal_type()
    {
        for (auto const &callback : terminal_type_requests_)
        {
            callback(terminal_type_);
        }

        terminal_type_requests_.clear();
    }
    
    std::shared_ptr<ma::socket>                          socket_;
    std::vector<byte>                                    unparsed_bytes_;
    
    std::function<void (std::string const &)>            on_data_read_;
    telnetpp::session                                    telnet_session_;
    telnetpp::options::echo::server                      telnet_echo_server_;
    telnetpp::options::suppress_ga::server               telnet_suppress_ga_server_;
    telnetpp::options::mccp::server                      telnet_mccp_server_;
    telnetpp::options::mccp::codec                       telnet_mccp_codec_;
    telnetpp::options::naws::client                      telnet_naws_client_;
    telnetpp::options::terminal_type::client             telnet_terminal_type_client_;
    
    telnetpp::byte_converter                             telnet_byte_converter_;
    
    std::function<void (std::uint16_t, std::uint16_t)>   on_window_size_changed_;
    std::shared_ptr<boost::asio::deadline_timer>         keepalive_timer_;

    std::string                                          terminal_type_;
    std::vector<std::function<void (std::string)>>       terminal_type_requests_;
};

// ==========================================================================
// CONSTRUCTOR
// ==========================================================================
connection::connection(std::shared_ptr<ma::socket> const &socket)
    : pimpl_(std::make_shared<impl>(socket))
{
}

// ==========================================================================
// DESTRUCTOR
// ==========================================================================
connection::~connection()
{
    disconnect();
}

// ==========================================================================
// START
// ==========================================================================
void connection::start()
{
    pimpl_->start();
}

// ==========================================================================
// WRITE
// ==========================================================================
void connection::write(std::string const &data)
{
    pimpl_->write(pimpl_->telnet_session_.send({
        telnetpp::element(data)
    }));
}

// ==========================================================================
// ON_DATA_READ
// ==========================================================================
void connection::on_data_read(
    std::function<void (std::string const &)> const &callback)
{
    pimpl_->on_data_read_ = callback;
}

// ==========================================================================
// ON_WINDOW_SIZE_CHANGED
// ==========================================================================
void connection::on_window_size_changed(
    std::function<void (std::uint16_t, std::uint16_t)> const &callback)
{
    pimpl_->on_window_size_changed_ = callback;
}

// ==========================================================================
// ON_SOCKET_DEATH
// ==========================================================================
void connection::on_socket_death(std::function<void ()> const &callback)
{
    pimpl_->socket_->on_death(callback);
}

// ==========================================================================
// DISCONNECT
// ==========================================================================
void connection::disconnect()
{
    if (pimpl_->keepalive_timer_ != nullptr)
    {
        boost::system::error_code unused_error_code;
        pimpl_->keepalive_timer_->cancel(unused_error_code);
    }

    if (pimpl_->socket_ != nullptr)
    {
        pimpl_->socket_->close();
        pimpl_->socket_.reset();
    }
}

// ==========================================================================
// ASYNC_GET_TERMINAL_TYPE
// ==========================================================================
void connection::async_get_terminal_type(
    std::function<void (std::string const &)> const &callback)
{
    pimpl_->terminal_type_requests_.push_back(callback);
}

}
