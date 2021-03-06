//
//  stackstring.h
//
//  Copyright (c) 2016 Will Bradley.
//

#pragma once
#include <string>
#include <string.h>

template <size_t MAX_STRLEN>
struct stackstring_t {
	stackstring_t() : length(0) {
	}

	~stackstring_t() = default;

	void reset() {
		length = 0;
	}

	bool append(const char ch) {
		if (length < MAX_STRLEN) {
			buffer[length++] = ch;
			return true;
		} else {
			return false;
		}
	}

	std::string str() const {
		return std::string(buffer, length);
	}

	const char *c_str() const {
		const_cast<char *>(buffer)[length] = '\0';
		return buffer;
	}

	size_t size() const {
		return length;
	}

	const char *begin() const {
		return buffer;
	}

	const char *end() const {
		return buffer + length;
	}

	bool operator ==(const char *str) const {
		return strcmp(str, c_str()) == 0;
	}

	char buffer[MAX_STRLEN + 1];
	size_t length;
};
