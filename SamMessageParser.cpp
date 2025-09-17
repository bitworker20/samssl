#include "SamMessageParser.h"
#include <sstream>
#include <algorithm> // For std::transform
#include <iostream>  // For potential debug prints, can be removed
#include <spdlog/spdlog.h>

// Helper to convert string to uppercase for case-insensitive comparisons of commands
static std::string SamParser_ToUpper(std::string s) { // Made static or put in anonymous namespace
	std::transform(s.begin(), s.end(), s.begin(), 
				   [](unsigned char c){ return std::toupper(c); }
				  );
	return s;
}

namespace SAM {

std::vector<std::string> SamMessageParser::splitString(const std::string& str, char delimiter) const {
	std::vector<std::string> tokens;
	if (str.empty()) return tokens;
	std::string token;
	std::istringstream tokenStream(str);
	while (std::getline(tokenStream, token, delimiter)) {
		// Add token even if empty if it's not the last one and there's a delimiter after
		// or if it's the only token (string doesn't contain delimiter)
		// This simplistic split works for SAM where multiple spaces are not usually significant between key fields
		tokens.push_back(token);
	}
	return tokens;
}

std::string SamMessageParser::getValueForKey(const std::string& full_line, const std::string& key) const {
	if (key.empty() || full_line.empty()) {
		return "";
	}
	std::string key_pattern = key + "=";
	size_t value_start_pos = full_line.find(key_pattern);

	if (value_start_pos == std::string::npos) {
		return ""; 
	}

	value_start_pos += key_pattern.length(); 

	size_t value_end_pos = full_line.find(' ', value_start_pos);
	if (value_end_pos == std::string::npos) {
		return full_line.substr(value_start_pos);
	}
	return full_line.substr(value_start_pos, value_end_pos - value_start_pos);
}


SAM::ParsedMessage SamMessageParser::parse(const std::string& sam_reply_line_in) const {
	SAM::ParsedMessage parsed_msg;
	
	std::string sam_reply_line = sam_reply_line_in;
	if (!sam_reply_line.empty() && sam_reply_line.back() == '\n') {
		sam_reply_line.pop_back();
	}
	if (!sam_reply_line.empty() && sam_reply_line.back() == '\r') {
		sam_reply_line.pop_back();
	}

	parsed_msg.original_message = sam_reply_line;

	if (sam_reply_line.empty()) {
		return parsed_msg;
	}

	std::vector<std::string> parts = splitString(sam_reply_line, ' ');

	if (parts.empty() || parts.size() < 2) {
		// std::cerr << "[ParserDebug] Not enough parts in message: '" << sam_reply_line << "'" << std::endl;
		return parsed_msg; 
	}

	std::string command1_upper = SamParser_ToUpper(parts[0]);
	std::string command2_upper = SamParser_ToUpper(parts[1]);

	if (command1_upper == "HELLO" && command2_upper == "REPLY") {
		parsed_msg.type = SAM::MessageType::HELLO_REPLY;
		std::string result_str = getValueForKey(sam_reply_line, "RESULT");
		if (result_str == "OK") parsed_msg.result = SAM::ResultCode::OK;
		else if (result_str == "NOVERSION") parsed_msg.result = SAM::ResultCode::NOVERSION;
		else if (result_str == "I2P_ERROR") parsed_msg.result = SAM::ResultCode::I2P_ERROR;
		else parsed_msg.result = SAM::ResultCode::UNKNOWN;
		parsed_msg.message_text = getValueForKey(sam_reply_line, "MESSAGE");
	}
	else if (command1_upper == "SESSION" && command2_upper == "STATUS") {
		parsed_msg.type = SAM::MessageType::SESSION_STATUS;
		std::string result_str = getValueForKey(sam_reply_line, "RESULT");
		if (result_str == "OK") parsed_msg.result = SAM::ResultCode::OK;
		else if (result_str == "DUPLICATED_ID") parsed_msg.result = SAM::ResultCode::DUPLICATED_ID;
		else if (result_str == "DUPLICATED_DEST") parsed_msg.result = SAM::ResultCode::DUPLICATED_DEST;
		else if (result_str == "I2P_ERROR") parsed_msg.result = SAM::ResultCode::I2P_ERROR;
		else if (result_str == "INVALID_KEY") parsed_msg.result = SAM::ResultCode::INVALID_KEY;
		else parsed_msg.result = SAM::ResultCode::UNKNOWN;
		parsed_msg.message_text = getValueForKey(sam_reply_line, "MESSAGE");
		if(parsed_msg.result == SAM::ResultCode::OK) {
			parsed_msg.destination_field = getValueForKey(sam_reply_line, "DESTINATION");
		}
	}
	else if (command1_upper == "STREAM" && command2_upper == "STATUS") {
		parsed_msg.type = SAM::MessageType::STREAM_STATUS;
		std::string result_str = getValueForKey(sam_reply_line, "RESULT");
		if (result_str == "OK") parsed_msg.result = SAM::ResultCode::OK;
		else if (result_str == "CANT_REACH_PEER") parsed_msg.result = SAM::ResultCode::CANT_REACH_PEER;
		else if (result_str == "I2P_ERROR") parsed_msg.result = SAM::ResultCode::I2P_ERROR;
		else if (result_str == "INVALID_KEY") parsed_msg.result = SAM::ResultCode::INVALID_KEY;
		else if (result_str == "INVALID_ID") parsed_msg.result = SAM::ResultCode::INVALID_ID;
		else if (result_str == "TIMEOUT") parsed_msg.result = SAM::ResultCode::TIMEOUT;
		else if (result_str == "ALREADY_ACCEPTING") parsed_msg.result = SAM::ResultCode::ALREADY_ACCEPTING;
		else parsed_msg.result = SAM::ResultCode::UNKNOWN;
		parsed_msg.message_text = getValueForKey(sam_reply_line, "MESSAGE");
		if (parsed_msg.result == SAM::ResultCode::OK) {
			parsed_msg.destination_field = getValueForKey(sam_reply_line, "FROM_DESTINATION");
		}
	}
	else if (command1_upper == "NAMING" && command2_upper == "REPLY") {
		parsed_msg.type = SAM::MessageType::NAMING_REPLY;
		std::string result_str = getValueForKey(sam_reply_line, "RESULT");
		if (result_str == "OK") parsed_msg.result = SAM::ResultCode::OK;
		else if (result_str == "INVALID_KEY") parsed_msg.result = SAM::ResultCode::INVALID_KEY;
		else if (result_str == "KEY_NOT_FOUND") parsed_msg.result = SAM::ResultCode::KEY_NOT_FOUND;
		else parsed_msg.result = SAM::ResultCode::UNKNOWN;
		parsed_msg.name = getValueForKey(sam_reply_line, "NAME");
		parsed_msg.value = getValueForKey(sam_reply_line, "VALUE");
		parsed_msg.message_text = getValueForKey(sam_reply_line, "MESSAGE");
	}
	else if (command1_upper == "DEST" && command2_upper == "REPLY") {
		parsed_msg.type = SAM::MessageType::DEST_REPLY;
		std::string result_str = getValueForKey(sam_reply_line, "RESULT");
		if (result_str == "I2P_ERROR") {
			parsed_msg.result = SAM::ResultCode::I2P_ERROR;
		} else {
			parsed_msg.pub_key = getValueForKey(sam_reply_line, "PUB");
			parsed_msg.priv_key = getValueForKey(sam_reply_line, "PRIV");
			if (!parsed_msg.pub_key.empty() && !parsed_msg.priv_key.empty()) {
				parsed_msg.result = SAM::ResultCode::OK;
			} else if (!result_str.empty() && SamParser_ToUpper(result_str) != "OK") { // If RESULT is present and not OK
				 parsed_msg.result = SAM::ResultCode::FAILED; // or UNKNOWN based on result_str
			} else if (result_str.empty() && (parsed_msg.pub_key.empty() || parsed_msg.priv_key.empty())) { // No RESULT and keys missing
				 parsed_msg.result = SAM::ResultCode::FAILED; 
			} else { // Default to OK if no explicit error and keys might be optional or handled differently
				 parsed_msg.result = SAM::ResultCode::OK;
			}
		}
		parsed_msg.message_text = getValueForKey(sam_reply_line, "MESSAGE");
	}
	else {
		SPDLOG_ERROR("Unknown message format: {}", sam_reply_line);
		// Type already defaults to UNKNOWN_OR_ERROR
	}

	return parsed_msg;
}

} // namespace SAM