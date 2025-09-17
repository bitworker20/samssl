#pragma once

#include <string>
#include <memory>
#include <map>
#include <boost/asio.hpp>
#include "SamConnection.h"    // Our base connection class
#include "SamMessageParser.h" // For result structs/enums
#include "I2PIdentityUtils.h" // For address parsing

namespace net = boost::asio;

namespace SAM {
	
// Result for establishing the main SAM session
struct EstablishSessionResult {
	bool success = false;
	std::string created_session_id;
	std::string local_b32_address;       // Parsed .b32.i2p address
	std::string raw_sam_destination_reply; // Raw DESTINATION= field from SAM
	std::string error_message;
	bool maybe_unreliable = false;
	std::chrono::milliseconds session_creation_duration;
	// The control connection is managed internally by SamService if persistent
};

// Result for accepting or connecting a stream via a new data connection
struct SetupStreamResult {
	bool success = false;
	std::string remote_peer_b32_address; // Parsed .b32.i2p address of the peer
	std::shared_ptr<SamConnection> data_connection; // The connection for data transfer
	std::string error_message;
};

class SamService : public std::enable_shared_from_this<SamService> {
public:
	SamService(net::io_context& io_ctx, 
			   const std::string& sam_host, uint16_t sam_port);
	~SamService();

	// Establishes the main control SAM session.
	// This session's ID will be used for subsequent stream operations.
	// The underlying SamConnection for control might be kept alive.
	net::awaitable<EstablishSessionResult> establishControlSession(
		const std::string& nickname,
		const std::string& private_key_b64_or_transient, // "TRANSIENT" or actual key
		const std::string& signature_type_if_key, // Empty if transient
		const std::map<std::string, std::string>& options = {
			{"i2p.streaming.profile", "INTERACTIVE"}, 
			{"inbound.length", "1"}, 
			{"outbound.length", "1"}}
	);

	// For a Listener/Server: uses an established control_session_id to accept an incoming stream.
	// This will create a new TCP connection to SAM for the accept and data phases.
	net::awaitable<SetupStreamResult> acceptStreamViaNewConnection(
		const std::string& control_session_id // The ID from EstablishSessionResult
	);

	// For an Initiator/Client: uses an established control_session_id to connect to a peer.
	// This will create a new TCP connection to SAM for the connect and data phases.
	net::awaitable<SetupStreamResult> connectToPeerViaNewConnection(
		const std::string& control_session_id, // Client's own session ID (from its establishControlSession)
		const std::string& target_peer_i2p_address_b32, // Target server's .b32.i2p address
		const std::map<std::string, std::string>& stream_connect_options = {
			{"i2p.streaming.profile", "INTERACTIVE"}, 
			{"inbound.length", "1"}, 
			{"outbound.length", "1"}}
	);
	
	void shutdown(); // Closes the main control connection if it's open
	bool isOpen();
	
	net::any_io_executor get_executor();
private:
	net::io_context& io_ctx_;
	std::string sam_host_;
	uint16_t sam_port_;
	SamMessageParser parser_; // A parser instance for the service if needed, though SamConnection has its own

	// Connection for the main SAM session (SESSION CREATE)
	std::shared_ptr<SamConnection> m_controlConnection; 
	std::string m_establishedControlSessionId; // Stored after successful establishControlSession
};

} // namespace SAM