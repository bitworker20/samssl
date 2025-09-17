#pragma once

#include <string>
#include <vector>
#include <map> // Included for completeness, though not heavily used in current simple parser

namespace SAM {

	enum class MessageType {
		HELLO_REPLY,
		SESSION_STATUS,
		STREAM_STATUS,
		NAMING_REPLY,
		DEST_REPLY,
		UNKNOWN_OR_ERROR 
	};

	enum class ResultCode {
		OK,
		DUPLICATED_DEST,
		DUPLICATED_ID,
		I2P_ERROR,
		INVALID_ID,
		INVALID_KEY,
		CANT_REACH_PEER,
		TIMEOUT,
		NOVERSION,
		KEY_NOT_FOUND,
		ALREADY_ACCEPTING,
		FAILED, 
		UNKNOWN 
	};

	struct ParsedMessage {
		MessageType type = MessageType::UNKNOWN_OR_ERROR;
		ResultCode result = ResultCode::UNKNOWN;
		std::string original_message; 

		std::string message_text; 

		// NAMING_REPLY specific
		std::string name;
		std::string value;

		// DEST_REPLY specific
		std::string pub_key;
		std::string priv_key;

		// SESSION_STATUS (OK) -> our destination
		// STREAM_STATUS (OK) -> peer's FROM_DESTINATION
		std::string destination_field; 
	};

class SamMessageParser {
public:
	SamMessageParser() {}
	~SamMessageParser() {}

	SAM::ParsedMessage parse(const std::string& sam_reply_line) const;

private:
	std::vector<std::string> splitString(const std::string& str, char delimiter) const;
	std::string getValueForKey(const std::string& full_line, const std::string& key) const;
};

} // namespace SAM
