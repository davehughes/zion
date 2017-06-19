#include "dbg.h"
#include "parse_state.h"
#include "logger_decls.h"
#include <cstdarg>


parse_state_t::parse_state_t(
		status_t &status,
		std::string filename,
		lexer_t &lexer,
		std::map<atom, ptr<const types::type_t>> type_macros,
		std::vector<token_t> *comments) :
	filename(filename),
	lexer(lexer),
	status(status),
	type_macros(type_macros),
	comments(comments)
{
	advance();
}

bool parse_state_t::advance() {
	debug_lexer(log(log_info, "advanced from %s %s", tkstr(token.tk), token.text.c_str()[0] != '\n' ? token.text.c_str() : ""));
	prior_token = token;
	return lexer.get_token(token, comments);
}

void parse_state_t::warning(const char *format, ...) {
	va_list args;
	va_start(args, format);
	status.emit_messagev(log_error, token.location, format, args);
	va_end(args);
}

void parse_state_t::error(const char *format, ...) {
	va_list args;
	va_start(args, format);
	status.emit_messagev(log_error, token.location, format, args);
	va_end(args);
}
