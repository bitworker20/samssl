#ifndef I2P_IDENTITY_UTILS_H
#define I2P_IDENTITY_UTILS_H

#include <string>

namespace I2PIdentityUtils {

	// Attempts to parse a Base64 string (which could be a full private key,
	// a full public destination, or just a public key part from SAM)
	// and returns the .b32.i2p address.
	// Returns the original b64 string with a warning suffix if parsing fails.
	std::string getB32AddressFromSamDestinationReply(const std::string& sam_destination_field_value, bool is_transient_reply = false);
	std::string generateI2PPrivateKey();
	std::string genRandomName();
	std::pair<std::string, std::string> generateI2PKeyAndIdentity();
} // namespace I2PIdentityUtils

#endif // I2P_IDENTITY_UTILS_H