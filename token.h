#pragma once
#include "stackstring.h"
#include <vector>
#include <string>
#include "location.h"

typedef stackstring_t<(1024 * 4) - sizeof(char) - sizeof(size_t)> zion_string_t;

enum token_kind_t {
	tk_none, /* NULL TOKEN */
	tk_comment, /* # hey */
	tk_identifier, /* identifier */

	// Syntax
	tk_lparen, /* ( */
	tk_rparen, /* ) */
	tk_comma, /* , */
	tk_lcurly, /* { */
	tk_rcurly, /* } */
	tk_lsquare, /* [ */
	tk_rsquare, /* ] */
	tk_question, /* ? */
	tk_colon, /* : */
	tk_dot, /* . */
	tk_semicolon, /* ; */
	tk_type, /* type */
	tk_fn, /* fn */
	tk_var, /* var */
	tk_const, /* const */
	tk_return, /* return */

	tk_struct, /* struct */
	tk_polymorph, /* polymorph */

	// Literals
	tk_char, /* char literal */
	tk_float, /* 3.1415e20 */
	tk_integer, /* [0-9]+ */
	tk_string, /* "string literal" */

	tk_as,

	tk_line, /* line */
	tk_if, /* if */
	tk_else, /* else */
	tk_loop, /* loop */
	tk_continue, /* continue */
	tk_break, /* break */
	tk_match, /* match */

	// Operators
	tk_assign,  /* = */
	tk_star,    /* * */

	// Dependency tokens
	tk_module, /* module */
	tk_link, /* link */
};


struct token_t {
	token_t(const location_t &location={{""},-1,-1}, token_kind_t tk=tk_none, std::string text="") : location(location), tk(tk), text(text) {}
	location_t location;
	token_kind_t tk = tk_none;
	std::string text;
	std::string str() const;
	void emit(int &indent_level, token_kind_t &last_tk, bool &indented_line);
};

const char *tkstr(token_kind_t tk);
void emit_tokens(const std::vector<token_t> &tokens);
