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

	// Literals
	tk_char, /* char literal */
	tk_float, /* 3.1415e20 */
	tk_integer, /* [0-9]+ */
	tk_string, /* "string literal" */

	// Operators
	tk_assign,  /* = */
	tk_star,    /* * */
	tk_hat,    /* ^ */
};

const char * const K_fn = "fn";
const char * const K_return = "return";
const char * const K_in = "in";
const char * const K_let = "let";
const char * const K_module = "module";
const char * const K_link = "link";
const char * const K_var = "var";
const char * const K_set = "set";
const char * const K_const = "const";
const char * const K_call = "call";
const char * const K_if = "if";
const char * const K_else = "else";
const char * const K_loop = "loop";
const char * const K_match = "match";
const char * const K_break = "break";
const char * const K_continue = "continue";
const char * const K_as = "as";
const char * const K_then = "then";
const char * const K_typeid = "typeid";
const char * const K_sizeof = "sizeof";
const char * const K_type = "type";
const char * const K_has = "has";
const char * const K_is = "is";
#define K(x) K_##x

struct token_t {
	token_t(const location_t &location={{""},-1,-1}, token_kind_t tk=tk_none, std::string text="") : location(location), tk(tk), text(text) {}
	location_t location;
	token_kind_t tk = tk_none;
	std::string text;
	std::string str() const;
	void emit(int &indent_level, token_kind_t &last_tk, bool &indented_line);
	bool is_ident(const char *x) const;
};

const char *tkstr(token_kind_t tk);
void emit_tokens(const std::vector<token_t> &tokens);
