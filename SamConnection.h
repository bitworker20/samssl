#pragma once

#include <string>
#include <memory>
#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ssl.hpp>
#include "SamMessageParser.h" // For ParsedMessage
#include "SamTransport.h"
#include <spdlog/spdlog.h>

namespace net = boost::asio;
using SteadyClock = std::chrono::steady_clock;
using cancellation_signal_ptr = std::shared_ptr<net::cancellation_signal>;

namespace SAM {
	
class SamConnection : public std::enable_shared_from_this<SamConnection>
{
public:
	enum class ConnectionState
	{
		DISCONNECTED,
		CONNECTING,
		CONNECTED_NO_HELLO, // TCP connected, HELLO not yet done/verified
		HELLO_OK,           // HELLO successful, ready for other commands
		DATA_STREAM_MODE,   // STREAM ACCEPT/CONNECT was OK, socket is now for data
		CLOSING,
		CLOSED,
		ERROR_STATE
	};

		SamConnection(net::io_context &io_ctx);
		SamConnection(net::io_context &io_ctx, SAM::Transport transport, std::shared_ptr<net::ssl::context> ssl_ctx);
	~SamConnection();

	net::awaitable<bool> connect(const std::string &host, uint16_t port, 
		SteadyClock::duration timeout = std::chrono::seconds(10));
	net::awaitable<SAM::ParsedMessage> performHello(
		SteadyClock::duration timeout = std::chrono::seconds(5));

	net::awaitable<SAM::ParsedMessage> sendCommandAndWaitReply(const std::string &command, 
		SteadyClock::duration reply_timeout = std::chrono::seconds(10));
	net::awaitable<std::string> readLine(SteadyClock::duration timeout);

	// For data transfer phase
	net::awaitable<std::size_t> streamRead(net::mutable_buffer buffer, 
			SteadyClock::duration timeout = std::chrono::minutes(5));
	net::awaitable<void> streamWrite(net::const_buffer buffer, 
			SteadyClock::duration timeout = std::chrono::seconds(30));

	void closeSocket(); // Synchronous close
	bool isOpen() const;
	ConnectionState getState() const { return current_state_; }
	void setState(ConnectionState new_state); // Allow external state setting if needed by manager

		net::ip::tcp::socket &rawSocket() { return socket_; } // Expose lowest layer socket
	net::any_io_executor get_executor() { return io_ctx_.get_executor(); }
	void cancel_read_operations();
private:
	net::io_context &io_ctx_;
	net::ip::tcp::socket socket_;
		SAM::Transport transport_ = SAM::Transport::TCP;
		std::shared_ptr<net::ssl::context> ssl_ctx_;
		std::unique_ptr<net::ssl::stream<net::ip::tcp::socket&>> ssl_stream_ref_;
	SAM::SamMessageParser parser_; // Each connection might parse its own replies
	net::streambuf read_streambuf_;
	ConnectionState current_state_ = ConnectionState::DISCONNECTED;
	net::steady_timer cancel_timer_;
	net::strand<net::any_io_executor> write_strand_;
};	

} // namespace SAM