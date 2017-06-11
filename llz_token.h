#pragma once
#include "stackstring.h"
#include <vector>
#include <string>
#include "location.h"

typedef stackstring_t<(1024 * 4) - sizeof(char) - sizeof(size_t)> zion_string_t;

enum llz_token_kind_t
{
	lltk_none, /* NULL TOKEN */
	lltk_comment, /* # hey */
	lltk_identifier, /* identifier */

	// Syntax
	lltk_lparen, /* ( */
	lltk_rparen, /* ) */
	lltk_comma, /* , */
	lltk_lcurly, /* { */
	lltk_rcurly, /* } */
	lltk_lsquare, /* [ */
	lltk_rsquare, /* ] */
	lltk_colon, /* : */
	lltk_semicolon, /* ; */
	lltk_type, /* type */
	lltk_fn, /* fn */
	lltk_var, /* var */
	lltk_const, /* const */
	lltk_return, /* return */

	// Literals
	lltk_char, /* char literal */
	lltk_float, /* 3.1415e20 */
	lltk_integer, /* [0-9]+ */
	lltk_string, /* "string literal" */

	// Flow control
	lltk_if, /* if */
	lltk_else, /* else */
	lltk_loop, /* loop */
	lltk_continue, /* continue */
	lltk_break, /* break */
	lltk_match, /* match */

	// Operators
	lltk_assign,  /* = */
	lltk_becomes, /* := */
	lltk_star,    /* * */

	// Dependency tokens
	lltk_program, /* program */
	lltk_link, /* link */
};


struct llz_token_t {
	llz_token_t(const location_t &location={{""},-1,-1}, llz_token_kind_t lltk=lltk_none, std::string text="") : location(location), lltk(lltk), text(text) {}
	location_t location;
	llz_token_kind_t lltk = lltk_none;
	std::string text;
	std::string str() const;
	void emit(int &indent_level, llz_token_kind_t &last_tk, bool &indented_line);
};

const char *lltkstr(llz_token_kind_t lltk);
void emit_tokens(const std::vector<llz_token_t> &tokens);
