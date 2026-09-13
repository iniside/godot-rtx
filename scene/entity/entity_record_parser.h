#pragma once

#include "core/variant/variant.h"

class EntityRecordParser {
public:
	static bool parse_utf8(const uint8_t *p_data, uint64_t p_length, Variant &r_value);
};
