#pragma once
#include "lexer.h"
#include "ast.h"
#include <memory>
#include <sstream>
#include "parse_state.h"

template <typename T, typename... Args>
ptr<T> parse_text(std::istream &is, std::string filename = "repl.llz") {
	lexer_t lexer(filename, is);
	status_t status;
	parse_state_t ps(status, filename, lexer, {});
	ps.module_id = make_iid("__parse_text__");

	auto item = T::parse(ps);
	if (ps.token.tk != tk_none) {
		assert(!status);
		return nullptr;
	}
	return item;
}

template <typename T, typename... Args>
ptr<T> parse_text(const std::string &text, std::string filename = "repl.llz") {
	std::istringstream iss(text);
	return parse_text<T>(iss, filename);
}
