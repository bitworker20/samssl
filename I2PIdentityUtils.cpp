
#include "I2PIdentityUtils.h"
#include <iostream> // For warnings
#include "Identity.h" // Needs libi2pd's Identity.h for i2p::data::IdentityEx, PrivateKeys
#include <random>
#include <ctime>

namespace I2PIdentityUtils {

std::string getB32AddressFromSamDestinationReply(
	const std::string& sam_destination_field_value, bool is_transient_reply) {

	if (sam_destination_field_value.empty()) {
		return "(Empty SAM Destination Field)";
	}

	std::string b32_address;
	i2p::data::IdentityEx ident_parser; // For parsing public destinations or identities

	if (is_transient_reply) {
		// For TRANSIENT reply, sam_destination_field_value is the full new private key string
		i2p::data::PrivateKeys tempKeys;
		if (tempKeys.FromBase64(sam_destination_field_value) > 0 && tempKeys.GetPublic()) {
			b32_address = tempKeys.GetPublic()->GetIdentHash().ToBase32() + ".b32.i2p";
		} else {
			b32_address = sam_destination_field_value + " (Error: TRANSIENT key parse failed)";
			std::cerr << "[I2PIdentityUtils] Failed to parse transient private key from SAM DESTINATION field: '" 
					  << sam_destination_field_value << "'" << std::endl;
		}
	} else {
		// For fixed key SESSION STATUS or FROM_DESTINATION, it's usually a public IdentityEx B64
		if (ident_parser.FromBase64(sam_destination_field_value) > 0) {
			b32_address = ident_parser.GetIdentHash().ToBase32() + ".b32.i2p";
		} else {
			// Fallback: Sometimes SAM might just return a B64 public key that's not a full IdentityEx.
			// This part is harder without knowing the exact format if it's not IdentityEx.
			// For now, we'll just indicate a parse warning.
			// A more robust solution might try to decode B64 and see if it matches raw key formats.
			b32_address = sam_destination_field_value + " (Warning: Could not parse to .b32.i2p via IdentityEx)";
			std::cerr << "[I2PIdentityUtils] Warning: Could not parse SAM DESTINATION field ('" 
					  << sam_destination_field_value << "') to .b32.i2p via IdentityEx." << std::endl;
		}
	}
	return b32_address;
}

std::string generateI2PPrivateKey() {
	
	i2p::crypto::InitCrypto (false);
	i2p::data::SigningKeyType type = i2p::data::SIGNING_KEY_TYPE_EDDSA_SHA512_ED25519;


	auto keys = i2p::data::PrivateKeys::CreateRandomKeys (type);
	return keys.ToBase64();
}



std::string genRandomName() {
	std::string chars = "abcdefghijklmnopqrstuvwxyz";
	std::string name;
	std::srand(std::time(nullptr));
	for (int i = 0; i < 6; i++) {
		name += chars[std::rand() % chars.size()];
	}
	return name;
}

std::pair<std::string, std::string> generateI2PKeyAndIdentity() {
	std::string private_key = generateI2PPrivateKey();
	std::string identity = getB32AddressFromSamDestinationReply(private_key, true);
	return {private_key, identity};
}

} // namespace I2PIdentityUtils