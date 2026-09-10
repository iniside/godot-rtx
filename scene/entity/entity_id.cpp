#include "entity_id.h"

#include "core/crypto/crypto_core.h"

String EntityId::to_string() const {
	static constexpr char digits[] = "0123456789abcdef";
	char buffer[33];
	for (int i = 0; i < 16; i++) {
		buffer[i] = digits[(high >> ((15 - i) * 4)) & 15];
		buffer[16 + i] = digits[(low >> ((15 - i) * 4)) & 15];
	}
	buffer[32] = 0;
	return String::utf8(buffer);
}

Error EntityId::parse(const String &p_text, EntityId &r_id) {
	if (p_text.length() != 32) {
		return ERR_INVALID_DATA;
	}
	EntityId result;
	for (int i = 0; i < 32; i++) {
		char32_t c = p_text[i];
		uint64_t digit;
		if (c >= '0' && c <= '9') {
			digit = c - '0';
		} else if (c >= 'a' && c <= 'f') {
			digit = c - 'a' + 10;
		} else {
			return ERR_INVALID_DATA;
		}
		uint64_t &part = i < 16 ? result.high : result.low;
		part = (part << 4) | digit;
	}
	r_id = result;
	return OK;
}

Error EntityId::generate(EntityId &r_id) {
	CryptoCore::RandomGenerator random;
	Error error = random.init();
	if (error != OK) {
		return error;
	}
	uint8_t bytes[16];
	error = random.get_random_bytes(bytes, sizeof(bytes));
	if (error != OK) {
		return error;
	}
	EntityId result;
	for (int i = 0; i < 8; i++) {
		result.high = (result.high << 8) | bytes[i];
		result.low = (result.low << 8) | bytes[8 + i];
	}
	result.high = (result.high & ~uint64_t(0xf000)) | uint64_t(0x4000);
	result.low = (result.low & ~(uint64_t(3) << 62)) | (uint64_t(2) << 62);
	r_id = result;
	return OK;
}
