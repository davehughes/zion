#pragma once
#include <string>
#include <map>
#include "lexer.h"
#include "logger_decls.h"
#include "status.h"
#include "ptr.h"
#include "identifier.h"

namespace types {
	struct type_t;
}

typedef std::map<std::string, ptr<const types::type_t>> type_macros_t;

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

	bool line_broke() const;
	token_t token;
	token_t prior_token;
	std::string filename;
	identifier::ref module_id;
	lexer_t &lexer;
	status_t &status;
	std::vector<token_t> *comments;
	type_macros_t type_macros;

private:
	bool newline = false;
};

#define eat_token_or_return(fail_code) \
	do { \
		debug_lexer(log(log_info, "eating a %s", tkstr(ps.token.tk))); \
		ps.advance(); \
	} while (0)

#define eat_token() eat_token_or_return(nullptr)

#define expect_token_or_return(_tk, fail_code) \
	do { \
		if (ps.token.tk != _tk) { \
			ps.error("expected '%s', got '%s' at %s:%d", \
				   	tkstr(_tk), tkstr(ps.token.tk), \
					__FILE__, __LINE__); \
			/* dbg(); */ \
			return fail_code; \
		} \
	} while (0)

#define expect_token(_tk) expect_token_or_return(_tk, nullptr)

#define chomp_token_or_return(_tk, fail_code) \
	do { \
		expect_token_or_return(_tk, fail_code); \
		eat_token_or_return(fail_code); \
	} while (0)
#define chomp_token(_tk) chomp_token_or_return(_tk, nullptr)
#define chomp_ident(ident) \
	do { \
		expect_ident(ident); \
		ps.advance(); \
	} while (0)

#define expect_ident(ident) \
	do { \
		if (ps.token.tk != tk_identifier || ps.token.text != ident) { \
			ps.error("expected '%s', got '%s' at %s:%d", \
					ident, ps.token.text.c_str(), \
					__FILE__, __LINE__); \
			return nullptr; \
		} \
	} while (0)


