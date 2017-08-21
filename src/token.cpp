#include "token.h"
#include "dbg.h"
#include "assert.h"
#include <sstream>


std::string token_t::str() const {
	std::stringstream ss;
	if (text.size() != 0) {
		ss << C_ID << "'" << text << "'" << C_RESET;
		ss << "@";
	}
	ss << location.str();
	return ss.str();
}

#define tk_case(x) case tk_##x: return #x

const char *tkstr(token_kind_t tk) {
	switch (tk) {
	tk_case(assign);
	tk_case(char);
	tk_case(colon);
	tk_case(comma);
	tk_case(comment);
	tk_case(dot);
	tk_case(float);
	tk_case(hat);
	tk_case(identifier);
	tk_case(integer);
	tk_case(none);
	tk_case(string);
	case tk_or: return "|";
	case tk_lcurly: return "{";
	case tk_rcurly: return "}";
	case tk_lparen: return "(";
	case tk_rparen: return ")";
	case tk_lsquare: return "[";
	case tk_rsquare: return "]";
	case tk_question: return "?";
	case tk_semicolon: return ":";
	case tk_star: return "*";
	}
	return "";
}

void token_t::emit(int &indent_level, token_kind_t &last_tk, bool &indented_line) {
	/* Pretty print this token in a stream. */
	switch (tk) {
	case tk_none:
		assert(false);
	   	break;
	case tk_assign:
		printf("=");
		break;
	case tk_or:
		printf("|");
		break;
	case tk_comment:
		printf("# %s\n", text.c_str());
		break;
	case tk_question:
		printf("?");
		break;
	case tk_identifier:
		printf("%s", text.c_str());
		break;
	case tk_dot:
		printf(".");
		break;
	case tk_hat:
		printf("^");
		break;
	case tk_lparen:
		printf("(");
		break;
	case tk_rparen:
		printf(")");
		break;
	case tk_comma:
		printf(",");
		break;
	case tk_lcurly:
		printf("{");
		break;
	case tk_rcurly:
		printf("}");
		break;
	case tk_lsquare:
		printf("[");
		break;
	case tk_rsquare:
		printf("]");
		break;
	case tk_colon:
		printf(":");
		break;
	case tk_semicolon:
		printf(";");
		break;

	// Literals
	case tk_char:
		printf("'%s'", text.c_str());
		break;
	case tk_float:
		printf("%s", text.c_str());
		break;
	case tk_integer:
		printf("%s", text.c_str());
		break;
	case tk_string:
		printf("string");
		break;

	// Flow control
	case tk_star:
		printf("*");
		break;
	}
	last_tk = tk;
}

bool token_t::is_ident(const char *x) const {
	return tk == tk_identifier && text == x;
}

void emit_tokens(const std::vector<token_t> &tokens) {
	int indent_level = 0;
	token_kind_t tk = tk_none;
	bool indented_line = false;
	for (auto token : tokens) {
		token.emit(indent_level, tk, indented_line);
	}
}
