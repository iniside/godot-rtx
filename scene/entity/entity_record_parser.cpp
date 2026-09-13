#include "entity_record_parser.h"

#include <string.h>

namespace {

constexpr int MAX_NUMBER_LENGTH = 48;
constexpr int MAX_INTEGER_DIGITS = 18;
constexpr int MAX_IDENTIFIER_LENGTH = 16;
constexpr int MAX_DEPTH = 24;

bool is_ascii_digit(uint8_t p_char) {
	return p_char >= '0' && p_char <= '9';
}

bool is_identifier_start(uint8_t p_char) {
	return (p_char >= 'a' && p_char <= 'z') || (p_char >= 'A' && p_char <= 'Z') || p_char == '_';
}

class RecordReader {
	const uint8_t *cursor = nullptr;
	const uint8_t *end = nullptr;

	void skip_space() {
		while (cursor < end && *cursor <= 32) {
			cursor++;
		}
	}

	bool parse_string(String &r_string);
	bool parse_number(Variant &r_value);
	bool parse_arguments(double *r_arguments, int p_count);
	bool parse_identifier(Variant &r_value);
	bool parse_dictionary(Dictionary &r_dictionary, int p_depth);
	bool parse_array(Array &r_array, int p_depth);

public:
	RecordReader(const uint8_t *p_data, uint64_t p_length) :
			cursor(p_data), end(p_data + p_length) {}
	bool parse_value(Variant &r_value, int p_depth);
};

bool RecordReader::parse_string(String &r_string) {
	cursor++;
	const uint8_t *begin = cursor;
	bool ascii = true;
	while (cursor < end && *cursor != '"') {
		if (*cursor == '\\' || (*cursor < 32 && *cursor != '\n')) {
			return false;
		}
		ascii = ascii && *cursor < 0x80;
		cursor++;
	}
	if (cursor >= end) {
		return false;
	}
	const int length = int(cursor - begin);
	cursor++;
	r_string = String();
	if (!ascii) {
		return r_string.append_utf8((const char *)begin, length) == OK;
	}
	if (length == 0) {
		return true;
	}
	if (r_string.resize_uninitialized(length + 1) != OK) {
		return false;
	}
	char32_t *target = r_string.ptrw();
	for (int i = 0; i < length; i++) {
		target[i] = char32_t(begin[i]);
	}
	target[length] = 0;
	return true;
}

bool RecordReader::parse_number(Variant &r_value) {
	char buffer[MAX_NUMBER_LENGTH];
	int length = 0;
	int digits = 0;
	bool is_float = false;
	if (*cursor == '-') {
		buffer[length++] = '-';
		cursor++;
	}
	if (cursor >= end || !is_ascii_digit(*cursor)) {
		return false;
	}
	enum { READING_INT,
		READING_DEC,
		READING_EXP,
		READING_DONE };
	int reading = READING_INT;
	bool exponent_sign = false;
	bool exponent_begun = false;
	while (true) {
		const uint8_t character = cursor < end ? *cursor : 0;
		switch (reading) {
			case READING_INT: {
				if (is_ascii_digit(character)) {
					digits++;
				} else if (character == '.') {
					reading = READING_DEC;
					is_float = true;
				} else if (character == 'e' || character == 'E') {
					reading = READING_EXP;
					is_float = true;
				} else {
					reading = READING_DONE;
				}
			} break;
			case READING_DEC: {
				if (is_ascii_digit(character)) {
				} else if (character == 'e' || character == 'E') {
					reading = READING_EXP;
				} else {
					reading = READING_DONE;
				}
			} break;
			case READING_EXP: {
				if (is_ascii_digit(character)) {
					exponent_begun = true;
				} else if ((character == '-' || character == '+') && !exponent_sign && !exponent_begun) {
					exponent_sign = true;
				} else {
					reading = READING_DONE;
				}
			} break;
			default:
				break;
		}
		if (reading == READING_DONE) {
			break;
		}
		if (length >= MAX_NUMBER_LENGTH - 1) {
			return false;
		}
		buffer[length++] = char(character);
		cursor++;
	}
	buffer[length] = 0;
	if (is_float) {
		r_value = String::to_float(buffer);
		return true;
	}
	if (digits > MAX_INTEGER_DIGITS) {
		return false;
	}
	r_value = String::to_int(buffer);
	return true;
}

bool RecordReader::parse_arguments(double *r_arguments, int p_count) {
	skip_space();
	if (cursor >= end || *cursor != '(') {
		return false;
	}
	cursor++;
	for (int i = 0; i < p_count; i++) {
		if (i > 0) {
			skip_space();
			if (cursor >= end || *cursor != ',') {
				return false;
			}
			cursor++;
		}
		skip_space();
		if (cursor >= end || (*cursor != '-' && !is_ascii_digit(*cursor))) {
			return false;
		}
		Variant number;
		if (!parse_number(number)) {
			return false;
		}
		r_arguments[i] = number;
	}
	skip_space();
	if (cursor >= end || *cursor != ')') {
		return false;
	}
	cursor++;
	return true;
}

bool arguments_are_int32(const double *p_arguments, int p_count) {
	for (int i = 0; i < p_count; i++) {
		if (!(p_arguments[i] >= -2147483648.0 && p_arguments[i] <= 2147483647.0)) {
			return false;
		}
	}
	return true;
}

bool RecordReader::parse_identifier(Variant &r_value) {
	char buffer[MAX_IDENTIFIER_LENGTH];
	int length = 0;
	while (cursor < end && (is_identifier_start(*cursor) || is_ascii_digit(*cursor))) {
		if (length >= MAX_IDENTIFIER_LENGTH - 1) {
			return false;
		}
		buffer[length++] = char(*cursor);
		cursor++;
	}
	buffer[length] = 0;
	if (strcmp(buffer, "true") == 0) {
		r_value = true;
		return true;
	}
	if (strcmp(buffer, "false") == 0) {
		r_value = false;
		return true;
	}
	if (strcmp(buffer, "null") == 0 || strcmp(buffer, "nil") == 0) {
		r_value = Variant();
		return true;
	}
	double arguments[9];
	if (strcmp(buffer, "Vector2") == 0) {
		if (!parse_arguments(arguments, 2)) {
			return false;
		}
		r_value = Vector2(arguments[0], arguments[1]);
		return true;
	}
	if (strcmp(buffer, "Vector2i") == 0) {
		if (!parse_arguments(arguments, 2)) {
			return false;
		}
		if (!arguments_are_int32(arguments, 2)) {
			return false;
		}
		r_value = Vector2i(int32_t(arguments[0]), int32_t(arguments[1]));
		return true;
	}
	if (strcmp(buffer, "Vector3") == 0) {
		if (!parse_arguments(arguments, 3)) {
			return false;
		}
		r_value = Vector3(arguments[0], arguments[1], arguments[2]);
		return true;
	}
	if (strcmp(buffer, "Vector3i") == 0) {
		if (!parse_arguments(arguments, 3)) {
			return false;
		}
		if (!arguments_are_int32(arguments, 3)) {
			return false;
		}
		r_value = Vector3i(int32_t(arguments[0]), int32_t(arguments[1]), int32_t(arguments[2]));
		return true;
	}
	if (strcmp(buffer, "Vector4") == 0) {
		if (!parse_arguments(arguments, 4)) {
			return false;
		}
		r_value = Vector4(arguments[0], arguments[1], arguments[2], arguments[3]);
		return true;
	}
	if (strcmp(buffer, "Vector4i") == 0) {
		if (!parse_arguments(arguments, 4)) {
			return false;
		}
		if (!arguments_are_int32(arguments, 4)) {
			return false;
		}
		r_value = Vector4i(int32_t(arguments[0]), int32_t(arguments[1]), int32_t(arguments[2]), int32_t(arguments[3]));
		return true;
	}
	if (strcmp(buffer, "Rect2") == 0) {
		if (!parse_arguments(arguments, 4)) {
			return false;
		}
		r_value = Rect2(arguments[0], arguments[1], arguments[2], arguments[3]);
		return true;
	}
	if (strcmp(buffer, "Color") == 0) {
		if (!parse_arguments(arguments, 4)) {
			return false;
		}
		r_value = Color(float(arguments[0]), float(arguments[1]), float(arguments[2]), float(arguments[3]));
		return true;
	}
	if (strcmp(buffer, "AABB") == 0) {
		if (!parse_arguments(arguments, 6)) {
			return false;
		}
		r_value = AABB(Vector3(arguments[0], arguments[1], arguments[2]), Vector3(arguments[3], arguments[4], arguments[5]));
		return true;
	}
	if (strcmp(buffer, "Basis") == 0) {
		if (!parse_arguments(arguments, 9)) {
			return false;
		}
		r_value = Basis(arguments[0], arguments[1], arguments[2], arguments[3], arguments[4], arguments[5], arguments[6], arguments[7], arguments[8]);
		return true;
	}
	return false;
}

bool RecordReader::parse_dictionary(Dictionary &r_dictionary, int p_depth) {
	cursor++;
	bool first = true;
	while (true) {
		skip_space();
		if (cursor >= end) {
			return false;
		}
		if (*cursor == '}') {
			cursor++;
			return true;
		}
		if (!first) {
			if (*cursor != ',') {
				return false;
			}
			cursor++;
			skip_space();
			if (cursor >= end) {
				return false;
			}
			if (*cursor == '}') {
				cursor++;
				return true;
			}
		}
		first = false;
		if (*cursor != '"') {
			return false;
		}
		String key;
		if (!parse_string(key)) {
			return false;
		}
		skip_space();
		if (cursor >= end || *cursor != ':') {
			return false;
		}
		cursor++;
		Variant value;
		if (!parse_value(value, p_depth)) {
			return false;
		}
		r_dictionary[key] = value;
	}
}

bool RecordReader::parse_array(Array &r_array, int p_depth) {
	cursor++;
	bool first = true;
	while (true) {
		skip_space();
		if (cursor >= end) {
			return false;
		}
		if (*cursor == ']') {
			cursor++;
			return true;
		}
		if (!first) {
			if (*cursor != ',') {
				return false;
			}
			cursor++;
			skip_space();
			if (cursor >= end) {
				return false;
			}
			if (*cursor == ']') {
				cursor++;
				return true;
			}
		}
		first = false;
		Variant value;
		if (!parse_value(value, p_depth)) {
			return false;
		}
		r_array.push_back(value);
	}
}

bool RecordReader::parse_value(Variant &r_value, int p_depth) {
	if (p_depth >= MAX_DEPTH) {
		return false;
	}
	skip_space();
	if (cursor >= end) {
		return false;
	}
	const uint8_t character = *cursor;
	if (character == '{') {
		Dictionary dictionary;
		if (!parse_dictionary(dictionary, p_depth + 1)) {
			return false;
		}
		r_value = dictionary;
		return true;
	}
	if (character == '[') {
		Array array;
		if (!parse_array(array, p_depth + 1)) {
			return false;
		}
		r_value = array;
		return true;
	}
	if (character == '"') {
		String text;
		if (!parse_string(text)) {
			return false;
		}
		r_value = text;
		return true;
	}
	if (character == '-' || is_ascii_digit(character)) {
		return parse_number(r_value);
	}
	if (is_identifier_start(character)) {
		return parse_identifier(r_value);
	}
	return false;
}

} // namespace

// Any token this grammar does not cover returns false so the caller re-parses the file with VariantParser.
bool EntityRecordParser::parse_utf8(const uint8_t *p_data, uint64_t p_length, Variant &r_value) {
	RecordReader reader(p_data, p_length);
	Variant value;
	if (!reader.parse_value(value, 0)) {
		return false;
	}
	r_value = value;
	return true;
}
