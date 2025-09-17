#include <iostream>
#include <fstream>
#include <string>
#include <algorithm>
#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp> 
#include "SamService.h"       // Our new service class
#include "SamConnection.h"    // For std::shared_ptr<SamConnection> type
#include "SamMessageParser.h" // For enums (though not strictly needed in main)
#include <spdlog/spdlog.h>

net::io_context client_io_ctx; // Renamed global io_context
std::atomic<bool> client_running(true);  // Renamed global running flag
std::shared_ptr<SAM::SamService> g_app_sam_service = nullptr; // Global for signal handler

void app_server_signal_handler(const boost::system::error_code& error, int signal_number) {
	if (error == net::error::operation_aborted) return;
	if (client_running) {
		SPDLOG_INFO("Signal {} received. Shutdown...", signal_number);
		client_running.store(false); 
		if (g_app_sam_service) { // SamService manages its own control connection
			net::post(client_io_ctx, []{ if(g_app_sam_service) g_app_sam_service->shutdown(); });
		}
		if(!client_io_ctx.stopped()) net::post(client_io_ctx, []{ if(!client_io_ctx.stopped()) client_io_ctx.stop();});
	}
}

// Main server coroutine
net::awaitable<void> echo_client_application_logic(
	const std::string& sam_host, uint16_t sam_port,
	const std::string& client_nickname, const std::string& client_private_key, const std::string& client_sig_type,
	const std::string& target_peer_i2p_address_b32
) {
	g_app_sam_service = std::make_shared<SAM::SamService>(client_io_ctx, sam_host, sam_port);
	auto active_streams_count = std::make_shared<std::atomic<int>>(0);
	SAM::EstablishSessionResult control_session_info;
	SAM::SetupStreamResult connect_res;
	//std::map<std::string, std::string> options = {{"i2p.streaming.profile", "INTERACTIVE"}, {"inbound.length", "2"}, {"outbound.length", "2"}};
	
	try {
		control_session_info = co_await g_app_sam_service->establishControlSession(
			client_nickname, client_private_key, client_sig_type
		);
		if (!control_session_info.success) {
			SPDLOG_ERROR("Failed to establish client's control SAM session: {}", control_session_info.error_message);
			co_return; 
		}
		SPDLOG_INFO("Client control session '{}' established. Local I2P Address: {}", control_session_info.created_session_id, control_session_info.local_b32_address);

		try {
			connect_res = co_await g_app_sam_service->connectToPeerViaNewConnection(
				control_session_info.created_session_id, target_peer_i2p_address_b32
			);
		} catch (const std::exception& e) {
			SPDLOG_ERROR("Exception during connectToPeerViaNewConnection: {}", e.what());
			co_return;
		}

		if (!connect_res.success || !connect_res.data_connection) {
			SPDLOG_ERROR("Failed to connect to peer: {}", connect_res.error_message);
			co_return;
		}
		
		// Read response from peer
		std::array<char, 16384> data_buffer;
		std::size_t bytes_read = 0;
		boost::system::error_code read_ec;
					
		while (connect_res.data_connection->isOpen() && client_running) {
			// Read line from stdin
			std::string line;
			
			std::cout << "echo_client> ";
			std::getline(std::cin, line);
			
			if (!client_running) break; // Check after blocking getline

			if (line == "exit" || line == "quit") {
				client_running = false;
				break;
			}
			if (line.empty()) continue;
			
			if (line.substr(0, 4) == "big ") 
			{
				int size = std::stoi(line.substr(4));
				line = std::string(size * 1024, 'A');
			}
			
			// Send line to peer
			co_await connect_res.data_connection->streamWrite(boost::asio::buffer(line));
			//std::cout << "[AppLogic] Sent to peer: " << line << std::endl;
			
			try {
				bytes_read = co_await connect_res.data_connection->streamRead(boost::asio::buffer(data_buffer), std::chrono::minutes(5));
			} catch (const boost::system::system_error& e) {
				 read_ec = e.code(); 
			}

			if (read_ec == boost::asio::error::eof || bytes_read == 0 && read_ec != boost::asio::error::timed_out) {
				SPDLOG_INFO("Peer closed (EOF or 0 bytes)."); break;
			}
			if (read_ec == boost::asio::error::timed_out) {
				SPDLOG_INFO("Read timeout. Closing stream."); break;
			}
			if (read_ec == boost::asio::error::operation_aborted) {
				SPDLOG_INFO("Read aborted."); break;
			}
			if (read_ec) {
				SPDLOG_ERROR("Read error: {}", read_ec.message()); break;
			}
			
			//std::string received_text(data_buffer.data(), bytes_read);
			SPDLOG_INFO("Rcvd {} bytes from peer", bytes_read);
		}
	} catch (const std::exception& e) { 
		SPDLOG_ERROR("Main server coroutine exception: {}", e.what()); 
	}

	SPDLOG_INFO("Manager loop exited. Close data connection...");
	if (connect_res.data_connection && connect_res.data_connection->isOpen()) {
		connect_res.data_connection->closeSocket();
	}

	if (g_app_sam_service) g_app_sam_service->shutdown();
	g_app_sam_service = nullptr;
	SPDLOG_INFO("Client coroutine finished.");
	if (!client_io_ctx.stopped()) client_io_ctx.stop();
	co_return;
}


int main(int argc, char* argv[]) {
	std::string SAM_HOST_CFG = "gate.peerpoker.site"; 
	uint16_t SAM_PORT_CFG = 19969;      
	std::string CLIENT_NICKNAME_CFG = "I2PECHO"; 
	std::string CLIENT_KEY_B64_CFG = "YOUR_BASE64_ENCODED_PRIVATE_KEY_STRING_HERE"; 
	std::string CLIENT_SIG_TYPE_CFG = "EdDSA_SHA512_Ed25519";
	std::string TARGET_PEER_I2P_ADDRESS_B32_CFG = "YOUR_TARGET_PEER_I2P_ADDRESS_B32_HERE";
	
	if (argc != 3) {
		SPDLOG_ERROR("Usage: {} <private_key_file_path> <target_peer_i2p_address_b32>", argv[0]);
		return 1;
	}
	
	try {
		std::ifstream key_file(argv[1]);
		if (!key_file.is_open()) {
			SPDLOG_ERROR("Failed to open key file: {}", argv[1]);
			return 1;
		}
			
		auto private_key = std::string(
			std::istreambuf_iterator<char>(key_file),
			std::istreambuf_iterator<char>()
		);
		// 清理可能的换行符
		private_key.erase(std::remove(private_key.begin(), private_key.end(), '\n'), private_key.end());
		private_key.erase(std::remove(private_key.begin(), private_key.end(), '\r'), private_key.end());
		
		CLIENT_KEY_B64_CFG = private_key;
	} catch (const std::exception& e) {
		SPDLOG_ERROR("Error reading key file: {}", e.what());
		return 1;
	}
	
	TARGET_PEER_I2P_ADDRESS_B32_CFG = argv[2];
	CLIENT_NICKNAME_CFG = CLIENT_NICKNAME_CFG + "_" + I2PIdentityUtils::genRandomName();

	if (CLIENT_KEY_B64_CFG == "YOUR_BASE64_ENCODED_PRIVATE_KEY_STRING_HERE") {
		SPDLOG_ERROR("FATAL ERROR: Please replace YOUR_BASE64_ENCODED_PRIVATE_KEY_STRING_HERE in echo_client.cpp");
		return 1;
	}
	
	try {
		net::signal_set signals(client_io_ctx, SIGINT, SIGTERM);
		signals.async_wait(&app_server_signal_handler);

		SPDLOG_INFO("Spawning main echo client application logic coroutine.");
		net::co_spawn(client_io_ctx, 
			echo_client_application_logic(SAM_HOST_CFG, SAM_PORT_CFG, 
										  CLIENT_NICKNAME_CFG, CLIENT_KEY_B64_CFG, CLIENT_SIG_TYPE_CFG,
										  TARGET_PEER_I2P_ADDRESS_B32_CFG),
			[](std::exception_ptr p) {
				if (p) {
					try { std::rethrow_exception(p); } 
					catch (const std::exception& e) {
						SPDLOG_ERROR("Main client coroutine exited with exception: {}", e.what());
					}
				} else {
					SPDLOG_INFO("Main client coroutine completed.");
				}
				if (!client_io_ctx.stopped()) client_io_ctx.stop();
			}
		);

		SPDLOG_INFO("Running client_io_ctx...");
		client_io_ctx.run(); 
		SPDLOG_INFO("client_io_ctx.run() finished.");

	} catch (const std::exception& e) {
		SPDLOG_ERROR("Unhandled exception during setup or run: {}", e.what());
		if (!client_io_ctx.stopped()) client_io_ctx.stop();
		return 1;
	}
	
	g_app_sam_service = nullptr; 
	SPDLOG_INFO("Program exiting.");
	return 0;
}