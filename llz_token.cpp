#include "llz_token.h"
#include "dbg.h"
#include "assert.h"
#include <sstream>

void ensure_indented_line(bool &indented_line, int indent_level);

std::string llz_token_t::str() const {
	std::stringstream ss;
	if (text.size() != 0) {
		ss << C_ID << "'" << text << "'" << C_RESET;
		ss << "@";
	}
	ss << location.str();
	return ss.str();
}

#define lltk_case(x) case lltk_##x: return #x

const char *lltkstr(llz_token_kind_t lltk) {
	switch (lltk) {
	lltk_case(assign);
	lltk_case(break);
	lltk_case(char);
	lltk_case(colon);
	lltk_case(comma);
	lltk_case(comment);
	lltk_case(dot);
	lltk_case(const);
	lltk_case(continue);
	lltk_case(else);
	lltk_case(float);
	lltk_case(fn);
	lltk_case(identifier);
	lltk_case(if);
	lltk_case(integer);
	lltk_case(lcurly);
	lltk_case(link);
	lltk_case(loop);
	lltk_case(lparen);
	lltk_case(lsquare);
	lltk_case(match);
	lltk_case(none);
	lltk_case(program);
	lltk_case(rcurly);
	lltk_case(return);
	lltk_case(rparen);
	lltk_case(rsquare);
	lltk_case(semicolon);
	lltk_case(star);
	lltk_case(string);
	lltk_case(type);
	lltk_case(var);
	}
	return "";
}

void llz_token_t::emit(int &indent_level, llz_token_kind_t &last_tk, bool &indented_line) {
	/* Pretty print this token in a stream. */
	ensure_indented_line(indented_line, indent_level);

	switch (lltk) {
	case lltk_none:
		assert(false);
	   	break;
	case lltk_comment:
		printf("# %s\n", text.c_str());
		break;
	case lltk_identifier:
		printf("%s", text.c_str());
		break;
	case lltk_dot:
		printf(".");
		break;
	case lltk_lparen:
		printf("(");
		break;
	case lltk_rparen:
		printf(")");
		break;
	case lltk_comma:
		printf(",");
		break;
	case lltk_lcurly:
		printf("{");
		break;
	case lltk_rcurly:
		printf("}");
		break;
	case lltk_lsquare:
		printf("[");
		break;
	case lltk_rsquare:
		printf("]");
		break;
	case lltk_colon:
		printf(":");
		break;
	case lltk_semicolon:
		printf(";");
		break;
	case lltk_type:
		printf("type");
		break;
	case lltk_fn:
		printf("fn");
		break;
	case lltk_var:
		printf("var");
		break;
	case lltk_const:
		printf("const");
		break;
	case lltk_return:
		printf("return");
		break;

	// Literals
	case lltk_char:
		printf("char");
		break;
	case lltk_float:
		printf("float");
		break;
	case lltk_integer:
		printf("integer");
		break;
	case lltk_string:
		printf("string");
		break;

	// Flow control
	case lltk_if:
		printf("if");
		break;
	case lltk_else:
		printf("else");
		break;
	case lltk_loop:
		printf("loop");
		break;
	case lltk_continue:
		printf("continue");
		break;
	case lltk_break:
		printf("break");
		break;
	case lltk_match:
		printf("match");
		break;

	// Operators
	case lltk_assign:
		printf("assign");
		break;
	case lltk_star:
		printf("star");
		break;

	// Dependency tokens
	case lltk_program:
		printf("program");
		break;
	case lltk_link:
		printf("link");
		break;
	}
	last_tk = lltk;
}

void emit_tokens(const std::vector<llz_token_t> &tokens) {
	int indent_level = 0;
	llz_token_kind_t lltk = lltk_none;
	bool indented_line = false;
	for (auto token : tokens) {
		token.emit(indent_level, lltk, indented_line);
	}
}
