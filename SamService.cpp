#include "SamService.h"
#include <iostream>

namespace SAM {

SamService::SamService(net::io_context& io_ctx, 
					   const std::string& sam_host, uint16_t sam_port)
	: io_ctx_(io_ctx), sam_host_(sam_host), sam_port_(sam_port) {
	// std::cout << "[SamService] Created for SAM bridge at " << sam_host_ << ":" << sam_port_ << std::endl;
}

SamService::SamService(net::io_context& io_ctx,
                      const std::string& sam_host, uint16_t sam_port,
                      SAM::Transport transport,
                      std::shared_ptr<net::ssl::context> ssl_ctx)
    : io_ctx_(io_ctx), sam_host_(sam_host), sam_port_(sam_port), transport_(transport), ssl_ctx_(std::move(ssl_ctx))
{
}

SamService::~SamService() {
	// std::cout << "[SamService] Destroying." << std::endl;
	shutdown();
}

void SamService::shutdown() {
	if (m_controlConnection && m_controlConnection->isOpen()) {
		// std::cout << "[SamService] Closing control connection." << std::endl;
		m_controlConnection->closeSocket();
	}
	m_controlConnection = nullptr;
}

bool SamService::isOpen() {
	return m_controlConnection && m_controlConnection->isOpen();
}

net::any_io_executor SamService::get_executor() {
	return io_ctx_.get_executor();
}

net::awaitable<EstablishSessionResult> SamService::establishControlSession(
	const std::string& nickname,
	const std::string& private_key_b64_or_transient,
	const std::string& signature_type_if_key,
	const std::map<std::string, std::string>& options) {
	
	EstablishSessionResult result;
	result.created_session_id = nickname; // Store intended ID

	if (m_controlConnection && m_controlConnection->isOpen()) {
		// Or, if same nickname, assume it's already established. For now, force re-establish.
		SPDLOG_INFO("Control connection already exists. Closing to re-establish for {}", nickname);
		m_controlConnection->closeSocket();
	}
    if (transport_ == SAM::Transport::SSL)
        m_controlConnection = std::make_shared<SamConnection>(io_ctx_, transport_, ssl_ctx_);
    else
        m_controlConnection = std::make_shared<SamConnection>(io_ctx_);
	
	try {
		bool connected = co_await m_controlConnection->connect(sam_host_, sam_port_, std::chrono::seconds(10));
		if (!connected) {
			result.error_message = "P1: Failed to connect to SAM bridge.";
			throw std::runtime_error(result.error_message);
		}

		SAM::ParsedMessage hello_reply = co_await m_controlConnection->performHello(std::chrono::seconds(5));
		if (hello_reply.result != SAM::ResultCode::OK) {
			SPDLOG_ERROR("HELLO failed: {}", hello_reply.original_message);
			result.error_message = "P1: HELLO failed: " + hello_reply.original_message;
			throw std::runtime_error(result.error_message);
		}

		std::string session_cmd = "SESSION CREATE STYLE=STREAM ID=" + nickname +
								  " DESTINATION=" + private_key_b64_or_transient;
		if (private_key_b64_or_transient != "TRANSIENT" && !signature_type_if_key.empty()) {
			session_cmd += " SIGNATURE_TYPE=" + signature_type_if_key;
		}
		for (const auto& opt : options) { session_cmd += " " + opt.first + "=" + opt.second; }

		//SPDLOG_INFO("Sending SESSION CREATE command, name = {}", nickname);
		auto send_time = std::chrono::steady_clock::now();
		SAM::ParsedMessage session_status = co_await m_controlConnection->sendCommandAndWaitReply(session_cmd, std::chrono::seconds(3*60)); // Longer timeout
		//SPDLOG_INFO("Received SESSION STATUS reply, msg = {}", session_status.original_message);
		auto recv_time = std::chrono::steady_clock::now();
		result.session_creation_duration = std::chrono::duration_cast<std::chrono::milliseconds>(recv_time - send_time);
		
		SPDLOG_INFO("SESSION CREATE command sent and received in {} ms, status = {}", 
				result.session_creation_duration.count(), 
				session_status.result == SAM::ResultCode::OK ? "OK" : "ERROR");
		
		if (session_status.type != SAM::MessageType::SESSION_STATUS || session_status.result != SAM::ResultCode::OK) {
			SPDLOG_ERROR("SESSION CREATE failed: {}", session_status.original_message);
			result.error_message = "P1: SESSION CREATE failed: " + session_status.original_message;
			throw std::runtime_error(result.error_message);
		}
		
		if (session_status.result == SAM::ResultCode::OK && result.session_creation_duration < std::chrono::seconds(2)) {
			result.maybe_unreliable = true;
			SPDLOG_WARN("SESSION CREATE command sent and received in {} ms, status = OK, but duration is too short", 
				result.session_creation_duration.count());
		}

		result.raw_sam_destination_reply = session_status.destination_field;
		if (result.raw_sam_destination_reply.empty()) {
			SPDLOG_ERROR("DESTINATION field empty in SESSION STATUS reply.");
			 result.error_message = "P1: DESTINATION field empty in SESSION STATUS reply.";
			 throw std::runtime_error(result.error_message);
		}
		
		result.local_b32_address = I2PIdentityUtils::getB32AddressFromSamDestinationReply(
			result.raw_sam_destination_reply, 
			(private_key_b64_or_transient == "TRANSIENT")
		);
		// Check if parsing failed (getB32AddressFromSamDestinationReply might return original + warning)
		if (result.local_b32_address.find("(Error:") != std::string::npos || result.local_b32_address.find("(Warning:") != std::string::npos ) {
			SPDLOG_ERROR("Warning/Error parsing local destination: {}", result.local_b32_address);
			 // Decide if this is fatal for result.success
		}
		
		m_establishedControlSessionId = nickname; // Store the successfully created session ID
		result.success = true;
		SPDLOG_INFO("Control SAM session '{}' established. Local Address: {}", m_establishedControlSessionId, result.local_b32_address);
		
		// The m_controlConnection is kept alive.
	} catch (const std::exception& e) {
		if (result.error_message.empty()) result.error_message = "P1 Exception: " + std::string(e.what());
		SPDLOG_ERROR("Exception: {}", result.error_message);
		if (m_controlConnection && m_controlConnection->isOpen()) m_controlConnection->closeSocket();
		m_controlConnection = nullptr; 
		result.success = false;
	}
	co_return result;
}

net::awaitable<SetupStreamResult> SamService::acceptStreamViaNewConnection(
	const std::string& control_session_id) {
	
	SetupStreamResult result;
    std::shared_ptr<SamConnection> data_connection;
    if (transport_ == SAM::Transport::SSL)
        data_connection = std::make_shared<SamConnection>(io_ctx_, transport_, ssl_ctx_);
    else
        data_connection = std::make_shared<SamConnection>(io_ctx_);
	result.data_connection = data_connection; // Store early for cleanup in case of partial success

	try {
		bool connected = co_await data_connection->connect(sam_host_, sam_port_, std::chrono::seconds(10));
		if (!connected) { throw std::runtime_error("Acceptor P2: Failed to connect."); }

		SAM::ParsedMessage hello_reply = co_await data_connection->performHello(std::chrono::seconds(5));
		if (hello_reply.result != SAM::ResultCode::OK) {
			SPDLOG_ERROR("Acceptor P2: HELLO failed: {}", hello_reply.original_message);
			throw std::runtime_error("Acceptor P2: HELLO failed: " + hello_reply.original_message);
		}
		
		// 使用连接的统一发送接口以支持 SSL/TCP 两种传输
		std::string accept_cmd = "STREAM ACCEPT ID=" + control_session_id + " SILENT=false";
		SAM::ParsedMessage accept_status_parsed = co_await data_connection->sendCommandAndWaitReply(
			accept_cmd, std::chrono::seconds(120)); // 首次 STREAM STATUS 等待更长
		SPDLOG_INFO("STREAM ACCEPT reply, msg = {}", accept_status_parsed.original_message);

		if (accept_status_parsed.type != SAM::MessageType::STREAM_STATUS || accept_status_parsed.result != SAM::ResultCode::OK) {
			SPDLOG_ERROR("Acceptor P2: STREAM ACCEPT status error: {}", accept_status_parsed.original_message);
			throw std::runtime_error("Acceptor P2: STREAM ACCEPT status error: " + accept_status_parsed.original_message);
		}

		// 兼容两种网关：有些会在同一行返回 FROM_DESTINATION，有些会在下一行返回
		if (!accept_status_parsed.destination_field.empty())
		{
			result.remote_peer_b32_address = I2PIdentityUtils::getB32AddressFromSamDestinationReply(
				accept_status_parsed.destination_field, false);
		}
		else
		{
			// 等待后续的 FROM_DESTINATION 行（保持超长等待以接入客户端）
			// std::cout << "[SamService DEBUG] Acceptor waiting for FROM_DESTINATION line..." << std::endl;
			std::string from_dest_line = co_await data_connection->readLine(std::chrono::hours(24*7));
			if (from_dest_line.empty()) { throw std::runtime_error("Acceptor P2: FROM_DESTINATION line empty."); }
			// std::cout << "[SamService DEBUG] Acceptor got FROM_DESTINATION line: " << from_dest_line;
			result.remote_peer_b32_address = I2PIdentityUtils::getB32AddressFromSamDestinationReply(from_dest_line, false);
		}
		if (result.remote_peer_b32_address.find("(Error:") != std::string::npos || 
		 	result.remote_peer_b32_address.find("(Warning:") != std::string::npos || 
			result.remote_peer_b32_address.length() < 50 ) {
			const std::string raw_from_destination = accept_status_parsed.destination_field.empty() ? std::string("<extra line>") : accept_status_parsed.destination_field;
			SPDLOG_ERROR("Acceptor P2: Invalid FROM_DESTINATION received: {}", raw_from_destination);
			throw std::runtime_error("Acceptor P2: Invalid FROM_DESTINATION received: " + raw_from_destination);
		}

		result.success = true;
		data_connection->setState(SamConnection::ConnectionState::DATA_STREAM_MODE);
		SPDLOG_INFO("Accepted client {} for session {} on new data connection.", result.remote_peer_b32_address, control_session_id);

	} catch (const std::exception& e) {
		result.error_message = "Acceptor P2 Exception: " + std::string(e.what());
		SPDLOG_ERROR("Exception: {}", result.error_message);
		if (data_connection && data_connection->isOpen()) data_connection->closeSocket();
		result.data_connection = nullptr; // Nullify on error
		result.success = false;
	}
	co_return result;
}

net::awaitable<SetupStreamResult> SamService::connectToPeerViaNewConnection(
	const std::string& control_session_id, // This client's own SAM session ID
	const std::string& target_peer_i2p_address_b32,
	const std::map<std::string, std::string>& stream_connect_options) {
	
	SetupStreamResult result;
	result.remote_peer_b32_address = target_peer_i2p_address_b32; // We know who we are connecting to
    std::shared_ptr<SamConnection> data_connection;
    if (transport_ == SAM::Transport::SSL)
        data_connection = std::make_shared<SamConnection>(io_ctx_, transport_, ssl_ctx_);
    else
        data_connection = std::make_shared<SamConnection>(io_ctx_);
	result.data_connection = data_connection;

	try {
		bool connected = co_await data_connection->connect(sam_host_, sam_port_, std::chrono::seconds(10));
		if (!connected) { throw std::runtime_error("Connector P2: Failed to connect.");}

		SAM::ParsedMessage hello_reply = co_await data_connection->performHello(std::chrono::seconds(5));
		if (hello_reply.result != SAM::ResultCode::OK) {
			SPDLOG_ERROR("Connector P2: HELLO failed: {}", hello_reply.original_message);
			throw std::runtime_error("Connector P2: HELLO failed: " + hello_reply.original_message);
		}

		std::string connect_cmd = "STREAM CONNECT ID=" + control_session_id + 
								  " DESTINATION=" + target_peer_i2p_address_b32 + 
								  " SILENT=false";
		for (const auto& opt : stream_connect_options) { connect_cmd += " " + opt.first + "=" + opt.second; }
		
		SAM::ParsedMessage connect_status = co_await data_connection->sendCommandAndWaitReply(connect_cmd, std::chrono::seconds(90)); 
		SPDLOG_INFO("STREAM CONNECT to {} reply, msg = {}", target_peer_i2p_address_b32, connect_status.original_message);
		
		if (connect_status.type != SAM::MessageType::STREAM_STATUS || connect_status.result != SAM::ResultCode::OK) {
			SPDLOG_ERROR("Connector P2: STREAM CONNECT failed: {}", connect_status.original_message);
			throw std::runtime_error("Connector P2: STREAM CONNECT failed: " + connect_status.original_message);
		}
		
		result.success = true;
		data_connection->setState(SamConnection::ConnectionState::DATA_STREAM_MODE);
		SPDLOG_INFO("Connected to peer {} via client session {} on new data connection.", target_peer_i2p_address_b32, control_session_id);

	} catch (const std::exception& e) {
		result.error_message = "Connector P2 Exception: " + std::string(e.what());
		SPDLOG_ERROR("Exception: {}", result.error_message);
		if (data_connection && data_connection->isOpen()) data_connection->closeSocket();
		result.data_connection = nullptr;
		result.success = false;
	}
	co_return result;
}

} // namespace SAM