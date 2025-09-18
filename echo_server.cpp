#include <iostream>
#include <fstream>
#include <string>
#include <algorithm>
#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include "SamService.h"		  // Our new service class
#include "SamConnection.h"	  // For std::shared_ptr<SamConnection> type
#include "SamMessageParser.h" // For enums (though not strictly needed in main)
#include <spdlog/spdlog.h>

net::io_context server_io_ctx_main;						 // Renamed global io_context
volatile bool server_main_running = true;				 // Renamed global running flag
std::shared_ptr<SAM::SamService> g_app_sam_service = nullptr; // Global for signal handler

void app_server_signal_handler(const boost::system::error_code &error, int signal_number)
{
	if (error == net::error::operation_aborted)
		return;
	if (server_main_running)
	{
		SPDLOG_INFO("Signal {} received. Shutdown...", signal_number);
		server_main_running = false;
		if (g_app_sam_service)
		{ // SamService manages its own control connection
			net::post(server_io_ctx_main, []
					  { if(g_app_sam_service) g_app_sam_service->shutdown(); });
		}
		if (!server_io_ctx_main.stopped())
			net::post(server_io_ctx_main, []
					  { if(!server_io_ctx_main.stopped()) server_io_ctx_main.stop(); });
	}
}

// process_echo_stream_with_connection: (This function remains IDENTICAL to the one in your previous working echo_server.cpp)
// It takes std::shared_ptr<SamConnection> data_conn_sptr and std::string remote_peer_addr
// I will omit its full code here for brevity, assuming you have it.
net::awaitable<void> process_echo_stream_with_connection(
	std::shared_ptr<SAM::SamConnection> data_conn_sptr,
	std::string remote_peer_addr)
{
	SAM::SamConnection &data_conn = *data_conn_sptr;
	std::array<char, 8192> data_buffer;
	SPDLOG_INFO("Stream with {} started on conn {}", remote_peer_addr, (void*)data_conn_sptr.get());
	try
	{
		while (server_main_running && data_conn.isOpen() && data_conn.getState() == SAM::SamConnection::ConnectionState::DATA_STREAM_MODE)
		{
			std::size_t bytes_read = 0;
			boost::system::error_code read_ec;
			try
			{
				bytes_read = co_await data_conn.streamRead(boost::asio::buffer(data_buffer), std::chrono::minutes(10)); // Example timeout
			}
			catch (const boost::system::system_error &e)
			{
				read_ec = e.code();
			}

			if (!server_main_running)
				break;
			if (read_ec == boost::asio::error::eof || bytes_read == 0 && read_ec != boost::asio::error::timed_out)
			{ // EOF or 0 bytes if not timeout
				std::cout << "[EchoLogicApp] Peer " << remote_peer_addr << " closed (EOF or 0 bytes)." << std::endl;
				break;
			}
			if (read_ec == boost::asio::error::timed_out)
			{
				std::cout << "[EchoLogicApp] Read timeout for " << remote_peer_addr << ". Closing stream." << std::endl;
				break;
			}
			if (read_ec == boost::asio::error::operation_aborted)
			{
				std::cout << "[EchoLogicApp] Read aborted for " << remote_peer_addr << "." << std::endl;
				break;
			}
			if (read_ec)
			{
				std::cerr << "[EchoLogicApp] Read error from " << remote_peer_addr << ": " << read_ec.message() << std::endl;
				break;
			}

			std::string received_text(data_buffer.data(), bytes_read);
			SPDLOG_INFO("Rcvd {} bytes", bytes_read);

			co_await data_conn.streamWrite(boost::asio::buffer(received_text));
			//SPDLOG_INFO("Echoed to {}.", remote_peer_addr);
		}
	}
	catch (const std::exception &e)
	{
		std::cerr << "[EchoLogicApp] Exception with " << remote_peer_addr << ": " << e.what() << std::endl;
	}

	std::cout << "[EchoLogicApp] Stream with " << remote_peer_addr << " finished on conn " << data_conn_sptr.get() << "." << std::endl;
	if (data_conn.isOpen())
		data_conn.closeSocket();
	co_return;
}

// Main server coroutine
net::awaitable<void> echo_server_application_logic(
	const std::string &sam_host, uint16_t sam_port,
	const std::string &server_nickname, const std::string &server_private_key, const std::string &server_sig_type,
	int max_concurrent_streams = 5)
{
	g_app_sam_service = std::make_shared<SAM::SamService>(server_io_ctx_main, sam_host, sam_port);
	auto active_streams_count = std::make_shared<std::atomic<int>>(0);
	SAM::EstablishSessionResult control_session_info;

	//std::map<std::string, std::string> options = {{"i2p.streaming.profile", "INTERACTIVE"}, {"inbound.length", "2"}, {"outbound.length", "2"}};
	try
	{
		control_session_info = co_await g_app_sam_service->establishControlSession(
			server_nickname, server_private_key, server_sig_type);
		if (!control_session_info.success)
		{
			SPDLOG_ERROR("Failed to establish server's control SAM session: {}", control_session_info.error_message);
			co_return;
		}
		SPDLOG_INFO("Server control session '{}' established. Local I2P Address: {}", control_session_info.created_session_id, control_session_info.local_b32_address);
		SPDLOG_INFO("Ready to spawn stream acceptor workers (max {}).", max_concurrent_streams);

		while (server_main_running)
		{
			if (!g_app_sam_service)
			{ // Should be set
				SPDLOG_ERROR("SamService instance lost. Exiting.");
				break;
			}
			// Check if control connection is still up (SamService might need an IsControlSessionActive() method)
			// For now, we assume if establishControlSession succeeded, it's active until shutdown() or error.

			if (active_streams_count->load() < max_concurrent_streams)
			{
				active_streams_count->fetch_add(1);
				net::co_spawn(
					server_io_ctx_main,
					[sam_svc_cap = g_app_sam_service, // Capture SamService
					 main_sid = control_session_info.created_session_id,
					 active_c = active_streams_count]() -> net::awaitable<void>
					{
						// std::cout << "[AcceptorSM] Worker started for main session '" << main_sid << "'." << std::endl;
						SAM::SetupStreamResult accept_res;
						try
						{
							accept_res = co_await sam_svc_cap->acceptStreamViaNewConnection(main_sid);
						}
						catch (const std::exception &e_accept_worker)
						{
							SPDLOG_ERROR("Worker exception during accept: {}", e_accept_worker.what());
							accept_res.success = false;
						}

						if (!server_main_running)
						{ /* Worker sees shutdown */
						}
						else if (accept_res.success && accept_res.data_connection)
						{
							SPDLOG_INFO("Accepted I2P stream from: {}", accept_res.remote_peer_b32_address);
							try
							{
								co_await process_echo_stream_with_connection(accept_res.data_connection, accept_res.remote_peer_b32_address);
							}
							catch (const std::exception &e_echo_worker)
							{
								SPDLOG_ERROR("Worker exception during echo processing for {}: {}", accept_res.remote_peer_b32_address, e_echo_worker.what());
								if (accept_res.data_connection && accept_res.data_connection->isOpen())
								{
									accept_res.data_connection->closeSocket(); // Ensure data conn closed on echo error
								}
							}
						}
						else
						{
							SPDLOG_ERROR("Worker failed to accept stream: {}", accept_res.error_message);
							// data_connection in accept_res should be null or closed by acceptStreamViaNewConnection on failure
						}
						active_c->fetch_sub(1);
						// std::cout << "[AcceptorSM] Worker finished for main session '" << main_sid << "'." << std::endl;
						co_return;
					},
					boost::asio::detached);
			}
			else
			{
				net::steady_timer wait_timer(server_io_ctx_main, std::chrono::milliseconds(200));
				co_await wait_timer.async_wait(net::use_awaitable);
			}
			if (!server_main_running)
				break;
		} // end while
	}
	catch (const std::exception &e)
	{
		SPDLOG_ERROR("Main server coroutine exception: {}", e.what());
	}

	SPDLOG_INFO("Manager loop exited. Waiting for active workers...");
	while (active_streams_count->load() > 0 && !server_io_ctx_main.stopped())
	{
		net::steady_timer final_wait(server_io_ctx_main, std::chrono::milliseconds(500));
		co_await final_wait.async_wait(net::use_awaitable);
	}
	SPDLOG_INFO("All workers believed to be finished.");

	if (g_app_sam_service)
		g_app_sam_service->shutdown();
	g_app_sam_service = nullptr;
	SPDLOG_INFO("Main server coroutine finished.");
	if (!server_io_ctx_main.stopped())
		server_io_ctx_main.stop();
	co_return;
}

int main(int argc, char *argv[])
{
	std::string SAM_HOST_CFG = "localhost";//"peerpoker.site";
	uint16_t SAM_PORT_CFG = 7656;
	std::string SERVER_NICKNAME_CFG = "I2PECHO";
	std::string SERVER_KEY_B64_CFG = "YOUR_BASE64_ENCODED_PRIVATE_KEY_STRING_HERE";
	std::string SERVER_SIG_TYPE_CFG = "EdDSA_SHA512_Ed25519";
	int MAX_CLIENTS_CFG = 2;

	if (argc > 1 ) {
		if (std::string(argv[1]) != "TRANSIENT") {
			try	{
				std::ifstream key_file(argv[1]);
				if (!key_file.is_open())
				{
					std::cerr << "Failed to open key file: " << argv[1] << std::endl;
					return 1;
				}

				auto private_key = std::string(
					std::istreambuf_iterator<char>(key_file),
					std::istreambuf_iterator<char>());
				// 清理可能的换行符
				private_key.erase(std::remove(private_key.begin(), private_key.end(), '\n'), private_key.end());
				private_key.erase(std::remove(private_key.begin(), private_key.end(), '\r'), private_key.end());

				SERVER_KEY_B64_CFG = private_key;
			}
			catch (const std::exception &e)
			{
				std::cerr << "Error reading key file: " << e.what() << std::endl;
				return 1;
			}
		} else {
			SERVER_KEY_B64_CFG = "TRANSIENT";
		}
	}

	if (SERVER_KEY_B64_CFG == "YOUR_BASE64_ENCODED_PRIVATE_KEY_STRING_HERE")
	{
		std::cerr << "FATAL ERROR: Please replace YOUR_BASE64_ENCODED_PRIVATE_KEY_STRING_HERE in echo_server.cpp" << std::endl;
		return 1;
	}

	SERVER_NICKNAME_CFG = SERVER_NICKNAME_CFG + "_" + I2PIdentityUtils::genRandomName();

	try
	{
		net::signal_set signals(server_io_ctx_main, SIGINT, SIGTERM);
		signals.async_wait(&app_server_signal_handler);

		SPDLOG_INFO("Spawning main echo server application logic coroutine.");
		net::co_spawn(server_io_ctx_main,
					  echo_server_application_logic(SAM_HOST_CFG, SAM_PORT_CFG,
													SERVER_NICKNAME_CFG, SERVER_KEY_B64_CFG, SERVER_SIG_TYPE_CFG,
													MAX_CLIENTS_CFG),
					  [](std::exception_ptr p)
					  {
						  if (p)
						  {
							  try
							  {
								  std::rethrow_exception(p);
							  }
							  catch (const std::exception &e)
							  {
								  SPDLOG_ERROR("Main server coroutine exited with exception: {}", e.what());
							  }
						  }
						  else
						  {
							  SPDLOG_INFO("Main server coroutine completed.");
						  }
						  if (!server_io_ctx_main.stopped())
							  server_io_ctx_main.stop();
					  });

		SPDLOG_INFO("Running server_io_ctx_main...");
		server_io_ctx_main.run();
		SPDLOG_INFO("server_io_ctx_main.run() finished.");
	}
	catch (const std::exception &e)
	{
		SPDLOG_ERROR("Unhandled exception during setup or run: {}", e.what());
		if (!server_io_ctx_main.stopped())
			server_io_ctx_main.stop();
		return 1;
	}

	g_app_sam_service = nullptr;
	SPDLOG_INFO("Program exiting.");
	return 0;
}