#include "SamConnection.h"
#include <iostream>
#include <boost/asio/experimental/awaitable_operators.hpp> // For operator||
#include <boost/asio/post.hpp>

namespace SAM {

class debug_scope
{
public:
	debug_scope(const std::string& msg) : msg_(msg) 
	{
		SPDLOG_INFO("DEBUG ENTER: {}", msg_);
	}
	~debug_scope() {
		SPDLOG_INFO("DEBUG EXIT: {}", msg_);
	}
private:
	std::string msg_;
};

SamConnection::SamConnection(net::io_context &io_ctx)
	: io_ctx_(io_ctx), socket_(io_ctx), parser_(), cancel_timer_(io_ctx), 
	  write_strand_(net::make_strand(io_ctx))
{ 	// parser_ is default constructed
	// std::cout << "[SamConnection:" << this << "] Created." << std::endl;
}

SamConnection::~SamConnection()
{
	if (socket_.is_open())
	{
		// std::cerr << "[SamConnection:" << this << "] Destructor: Socket still open. Forcing close." << std::endl;
		closeSocket();
	}
	// std::cout << "[SamConnection:" << this << "] Destroyed." << std::endl;
}

void SamConnection::setState(ConnectionState new_state)
{
	// std::cout << "[SamConnection:" << this << " DEBUG] State: "
	//           << static_cast<int>(current_state_) << " -> "
	//           << static_cast<int>(new_state) << std::endl;
	current_state_ = new_state;
}

void SamConnection::cancel_read_operations()
{
	SPDLOG_INFO("SamConnection: cancel_read_operations called, cancel the timer.");
	//closeSocket();
	cancel_timer_.cancel();
}

bool SamConnection::isOpen() const
{
	return socket_.is_open() &&
		   (current_state_ != ConnectionState::CLOSED) &&
		   (current_state_ != ConnectionState::DISCONNECTED) &&
		   (current_state_ != ConnectionState::ERROR_STATE);
}

net::awaitable<bool> SamConnection::connect(
	const std::string &host, uint16_t port, SteadyClock::duration timeout)
{
	if (current_state_ != ConnectionState::DISCONNECTED && current_state_ != ConnectionState::CLOSED)
	{
		SPDLOG_ERROR("Connect called in invalid state: {}", static_cast<int>(current_state_));
		co_return false;
	}
	setState(ConnectionState::CONNECTING);
	try
	{
		net::ip::tcp::resolver resolver(io_ctx_);
		// Use co_spawn for resolve if it needs to be explicitly on io_ctx for some reason,
		// but async_resolve itself returns an awaitable when used with use_awaitable.
		auto endpoints = co_await resolver.async_resolve(host, std::to_string(port), net::use_awaitable);

		// std::cout << "[SamConnection:" << this << "] Connecting to " << host << ":" << port << "..." << std::endl;

		net::steady_timer connect_timer(io_ctx_);
		connect_timer.expires_after(timeout);

		using namespace net::experimental::awaitable_operators;
		auto result_variant = co_await (
			net::async_connect(socket_, endpoints, net::use_awaitable) ||
			connect_timer.async_wait(net::use_awaitable));

		if (result_variant.index() == 1)
		{ // Timer expired
			SPDLOG_ERROR("Timeout connecting to {}:{}", host, port);
			boost::system::error_code ec;
			socket_.close(ec);                       // Ensure socket is closed
			setState(ConnectionState::DISCONNECTED); // Or ERROR_STATE if timeout is considered an error
			co_return false;
		}
		// If index is 0, connect succeeded. An exception would have been thrown for other connect errors.

		setState(ConnectionState::CONNECTED_NO_HELLO);
		SPDLOG_INFO("Connected to {}:{}", host, port);
		co_return true;
	}
	catch (const boost::system::system_error &e)
	{
		SPDLOG_ERROR("System error during connect: {}", e.what());
		closeSocket();
		co_return false;
	}
	catch (const std::exception &e)
	{
		SPDLOG_ERROR("Exception during connect: {}", e.what());
		closeSocket();
		co_return false;
	}
}

net::awaitable<SAM::ParsedMessage> SamConnection::performHello(SteadyClock::duration timeout)
{
	if (current_state_ != ConnectionState::CONNECTED_NO_HELLO)
	{
		SPDLOG_ERROR("PerformHello called in invalid state: {}", static_cast<int>(current_state_));
		throw std::runtime_error("SamConnection: Cannot perform HELLO in current state.");
	}
	SAM::ParsedMessage parsed_reply;
	try
	{
		std::string hello_cmd = "HELLO VERSION MIN=3.1 MAX=3.2\n";
		co_await net::async_write(socket_, net::buffer(hello_cmd), net::use_awaitable);
		// std::cout << "[SamConnection:" << this << " DEBUG] Sent: " << hello_cmd;
		std::string reply_str = co_await readLine(timeout);
		parsed_reply = parser_.parse(reply_str);

		if (parsed_reply.type == SAM::MessageType::HELLO_REPLY && parsed_reply.result == SAM::ResultCode::OK)
		{
			setState(ConnectionState::HELLO_OK);
			SPDLOG_INFO("HELLO successful.");
		}
		else
		{
			SPDLOG_ERROR("HELLO failed: {}", parsed_reply.original_message);
			closeSocket();
			setState(ConnectionState::ERROR_STATE);
		}
	}
	catch (const std::exception &e)
	{
		SPDLOG_ERROR("Exception during HELLO: {}", e.what());
		closeSocket();
		setState(ConnectionState::ERROR_STATE);
		parsed_reply.type = SAM::MessageType::UNKNOWN_OR_ERROR;
		parsed_reply.message_text = e.what();
	}
	co_return parsed_reply;
}

net::awaitable<SAM::ParsedMessage> SamConnection::sendCommandAndWaitReply(
	const std::string &command, SteadyClock::duration reply_timeout)
{
	// Prerequisite state for most commands after HELLO
	if (current_state_ != ConnectionState::HELLO_OK)
	{
		SPDLOG_ERROR("SendCommand called in invalid state: {}", static_cast<int>(current_state_));
		throw std::runtime_error("SamConnection: Cannot send command, HELLO not completed or connection error.");
	}
	SAM::ParsedMessage parsed_reply;
	try
	{
		std::string full_command = command;
		if (full_command.empty() || full_command.back() != '\n')
		{
			full_command += '\n';
		}
		// std::cout << "[SamConnection:" << this << " DEBUG] Sending: " << command;
		co_await net::async_write(socket_, net::buffer(full_command), net::use_awaitable);
		std::string reply_str = co_await readLine(reply_timeout);
		parsed_reply = parser_.parse(reply_str);
	}
	catch (const std::exception &e)
	{
		SPDLOG_ERROR("Exception during sendCommandAndWaitReply for '{}': {}", command, e.what());
		closeSocket();
		setState(ConnectionState::ERROR_STATE);
		parsed_reply.type = SAM::MessageType::UNKNOWN_OR_ERROR;
		parsed_reply.message_text = e.what();
	}
	co_return parsed_reply;
}

net::awaitable<std::string> SamConnection::readLine(SteadyClock::duration timeout_duration)
{
	std::string line;
	//net::steady_timer timer(io_ctx_); // Timer per readLine call
	//timer.expires_after(timeout_duration);
	cancel_timer_.expires_after(timeout_duration);
	using namespace net::experimental::awaitable_operators;

	try
	{
		boost::system::error_code ec;
		auto result_variant = co_await (
			net::async_read_until(socket_, read_streambuf_, '\n', net::use_awaitable) ||
			cancel_timer_.async_wait(net::redirect_error(net::use_awaitable, ec)));

		if (result_variant.index() == 1)
		{
			if (ec == net::error::operation_aborted) {
				SPDLOG_INFO("SamConnection: readLine was cancelled via timer.");
				throw boost::system::system_error(ec, "readLine cancelled");
			} else {
				SPDLOG_ERROR("Timeout waiting for reply in readLine.");
				throw boost::system::system_error(net::error::timed_out, "SAM reply timeout in readLine");
			}
		}
		std::istream is(&read_streambuf_);
		std::getline(is, line);
		// std::cout << "[SamConnection:" << this << " DEBUG] Raw line read: '" << line << "'" << std::endl;
		co_return line;
	}
	catch (const boost::system::system_error &e)
	{
		// std::cerr << "[SamConnection:" << this << "] System error in readLine: " << e.code().message() << std::endl;
		throw;
	}
}

net::awaitable<std::size_t> SamConnection::streamRead(
	net::mutable_buffer buffer, 
	SteadyClock::duration timeout_duration) { // timeout_duration 来自 .h 文件
	
	if (current_state_ != ConnectionState::DATA_STREAM_MODE) {
		std::string err_msg = "SamConnection::streamRead - Not in DATA_STREAM_MODE. Current state: " + std::to_string(static_cast<int>(current_state_));
		SPDLOG_ERROR("{}", err_msg);
		throw boost::system::system_error(net::error::not_connected, err_msg);
	}

	// Handle "no timeout" or "infinite timeout" by directly awaiting read_some
	// A very large duration can act as pseudo-infinite for practical purposes if timer must be used.
	// Or, if timeout_duration is a special sentinel value like std::chrono::steady_clock::duration::max()
	if (timeout_duration <= SteadyClock::duration::zero() || 
		timeout_duration == SteadyClock::duration::max()) { // Treat zero/negative/max as no specific timeout
		try {
			// std::cout << "[SamConnection:" << this << " DEBUG] streamRead (no explicit timeout) waiting..." << std::endl;
			std::size_t bytes_transferred = co_await socket_.async_read_some(buffer, net::use_awaitable);
			// std::cout << "[SamConnection:" << this << " DEBUG] streamRead (no explicit timeout) got " << bytes_transferred << " bytes." << std::endl;
			if (bytes_transferred == 0 && socket_.is_open() && current_state_ == ConnectionState::DATA_STREAM_MODE) {
				SPDLOG_INFO("EOF indication from peer.");
				// This is an EOF indication from peer if socket is still open from our side.
			}
			co_return bytes_transferred;
		} catch (const boost::system::system_error& e) {
			if (e.code() != boost::asio::error::eof && e.code() != boost::asio::error::operation_aborted) {
				SPDLOG_ERROR("System error in streamRead (no timeout path): {}", e.code().message());
			}
			if (!socket_.is_open()) setState(ConnectionState::CLOSED);
			throw; 
		}
	}

	// Proceed with timer for explicit timeout
	//net::steady_timer timer(io_ctx_);
	cancel_timer_.expires_after(timeout_duration);
	
	// 在开始异步操作前检查socket状态
	if (!socket_.is_open() || current_state_ != ConnectionState::DATA_STREAM_MODE) {
		SPDLOG_INFO("SamConnection: streamRead - socket closed or invalid state, aborting");
		throw boost::system::system_error(net::error::operation_aborted, "Socket closed during streamRead");
	}

	// std::cout << "[SamConnection:" << this << " DEBUG] streamRead (with timeout " << std::chrono::duration_cast<std::chrono::milliseconds>(timeout_duration).count() << "ms) waiting..." << std::endl;
	try {
		using namespace net::experimental::awaitable_operators;
		
		boost::system::error_code ec;	
		cancel_timer_.expires_after(timeout_duration);
		//SPDLOG_INFO("SamConnection: streamRead: cancel_timer_ expires after {}ms", std::chrono::duration_cast<std::chrono::milliseconds>(timeout_duration).count());
		auto result_variant = co_await (
			socket_.async_read_some(buffer, net::use_awaitable) || 
			cancel_timer_.async_wait(net::redirect_error(net::use_awaitable, ec))
		);

		if (result_variant.index() == 1) { // Timer expired first
			if (ec == net::error::operation_aborted)
			{
				// This is a cancellation, not a timeout.
				SPDLOG_INFO("SamConnection: streamRead was cancelled via timer.");
				throw boost::system::system_error(ec, "Stream read cancelled");
			}
			else
			{
				// This is a real timeout.
				SPDLOG_WARN("SamConnection: streamRead timeout.");
				socket_.cancel(); // Best effort cancel of the read op
				throw boost::system::system_error(net::error::timed_out, "Stream read timeout");
			}
		}
		
		// Read completed, get the number of bytes from the variant
		std::size_t bytes_transferred = std::get<0>(result_variant);
		// std::cout << "[SamConnection:" << this << " DEBUG] streamRead (with timeout) got " << bytes_transferred << " bytes." << std::endl;
		if (bytes_transferred == 0 && socket_.is_open() && current_state_ == ConnectionState::DATA_STREAM_MODE) {
			SPDLOG_INFO("EOF indication from peer, Maybe peer closed the connection.");
			// EOF
		}
		co_return bytes_transferred;

	} catch (const boost::system::system_error& e) {
		// This will catch the timed_out exception from above, or other system errors from async_read_some.
		if (e.code() == net::error::operation_aborted) {
			SPDLOG_INFO("SamConnection: streamRead was cancelled as expected. Error: {}", e.what());
		} else if (e.code() != net::error::eof && 
			e.code() != net::error::timed_out) { // Don't re-log timeout if already messaged
			SPDLOG_ERROR("System error in streamRead (with timeout path): {}", e.code().message());
		} else {
			// Log timeouts and EOF for debugging clarity, but maybe as INFO or WARN
			SPDLOG_WARN("SamConnection: streamRead finished with code: {}", e.code().message());
		}
		if (!socket_.is_open()) setState(ConnectionState::CLOSED);
		throw; // Re-throw for the caller (e.g., process_echo_stream_with_connection) to handle
	}
}

net::awaitable<void> SamConnection::streamWrite(net::const_buffer buffer, 
		SteadyClock::duration timeout)
{
	if (current_state_ != ConnectionState::DATA_STREAM_MODE)
	{
		SPDLOG_ERROR("Not in DATA_STREAM_MODE. Current state: {}", static_cast<int>(current_state_));
		throw boost::system::system_error(net::error::not_connected,
				"SamConnection::streamWrite - Not in DATA_STREAM_MODE. Current state: " + 
				std::to_string(static_cast<int>(current_state_)));
	}
	
	// Handle "no timeout" case
	if (timeout <= SteadyClock::duration::zero() || 
		timeout == SteadyClock::duration::max()) {
		try {
			// Use strand to serialize write operations even without timeout
			co_await net::async_write(socket_, buffer, 
				net::bind_executor(write_strand_, net::use_awaitable));
			co_return;
		} catch (const boost::system::system_error &e) {
			SPDLOG_ERROR("Error in streamWrite (no timeout): {}", e.code().message());
			if (!socket_.is_open())
				setState(ConnectionState::CLOSED);
			throw;
		}
	}
	
	// 不可以使用cancel_timer_，
	net::steady_timer write_timer(io_ctx_);
	write_timer.expires_after(timeout);
	
	try {
		using namespace boost::asio::experimental::awaitable_operators;
		
		auto write_task = [&]() -> net::awaitable<void> {
			// Use strand to serialize write operations
			co_await net::async_write(socket_, buffer, 
				net::bind_executor(write_strand_, net::use_awaitable));
		};
		
		auto timeout_task = [&]() -> net::awaitable<void> {
			co_await write_timer.async_wait(net::use_awaitable);
			throw boost::system::system_error(boost::asio::error::timed_out, 
				"SamConnection::streamWrite timeout");
		};
		
		auto result = co_await (write_task() || timeout_task());
		
		// Cancel timer if write completed first
		write_timer.cancel();
		
		if (result.index() == 1) {
			// Timeout occurred
			throw boost::system::system_error(boost::asio::error::timed_out, 
				"SamConnection::streamWrite timeout");
		}
		
	} catch (const boost::system::system_error &e) {
		SPDLOG_WARN("SamConnection: streamWrite finished with code: {}", e.code().message());
		if (!socket_.is_open() || e.code() == boost::asio::error::timed_out)
			setState(ConnectionState::CLOSED);
		throw;
	}
	
	co_return;
}

void SamConnection::closeSocket()
{
	// This state check must be synchronous to prevent race conditions.
	if (current_state_ == ConnectionState::CLOSING ||
		current_state_ == ConnectionState::CLOSED)
	{
		SPDLOG_INFO("SamConnection: closeSocket: socket already closed or closing");
		return;
	}
	// Set state immediately to prevent new operations from starting.
	setState(ConnectionState::CLOSING);
	SPDLOG_INFO("SamConnection: State set to CLOSING. Executing close logic.");

	// Emit signal to cancel coroutines bound to it.
	cancel_timer_.cancel();

	if (socket_.is_open()) {
		boost::system::error_code ec;
		// Gracefully shut down the socket. This will cancel pending reads.
		SPDLOG_INFO("SamConnection: Executing socket shutdown and close.");
		socket_.shutdown(net::ip::tcp::socket::shutdown_both, ec);
		// Close the socket.
		socket_.close(ec);
	}

	setState(ConnectionState::CLOSED);
	// Clear any buffered data.
	read_streambuf_.consume(read_streambuf_.size());
}

} // namespace SAM