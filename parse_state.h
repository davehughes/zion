#pragma once
#include <string>
#include "lexer.h"
#include "atom.h"
#include "logger_decls.h"
#include "status.h"
#include "ptr.h"
#include "identifier.h"

namespace types {
	struct type_t;
}

struct parse_state_t {
	typedef log_level_t parse_error_level_t;
	parse_error_level_t pel_error = log_error;
	parse_error_level_t pel_warning = log_warning;

	parse_state_t(
			status_t &status,
			std::string filename,
			lexer_t &lexer,
			std::vector<token_t> *comments=nullptr);

	void advance();
	void warning(const char *format, ...);
	void error(const char *format, ...);

	token_t token;
	token_t prior_token;
	atom filename;
	identifier::ref module_id;
	lexer_t &lexer;
	status_t &status;
	std::vector<token_t> *comments;

private:
	bool newline = false;
};
