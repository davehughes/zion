#include "token.h"
#include "dbg.h"
#include "assert.h"
#include <sstream>

void ensure_indented_line(bool &indented_line, int indent_level);

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
	tk_case(break);
	tk_case(char);
	tk_case(colon);
	tk_case(comma);
	tk_case(comment);
	tk_case(const);
	tk_case(continue);
	tk_case(dot);
	tk_case(else);
	tk_case(float);
	tk_case(fn);
	tk_case(identifier);
	tk_case(if);
	tk_case(integer);
	tk_case(lcurly);
	tk_case(line);
	tk_case(link);
	tk_case(loop);
	tk_case(lparen);
	tk_case(lsquare);
	tk_case(match);
	tk_case(none);
	tk_case(polymorph);
	tk_case(program);
	tk_case(rcurly);
	tk_case(return);
	tk_case(rparen);
	tk_case(rsquare);
	tk_case(semicolon);
	tk_case(star);
	tk_case(string);
	tk_case(struct);
	tk_case(type);
	tk_case(var);
	}
	return "";
}

token_kind_t translate_tk(token_kind_t tk, const zion_string_t &token_text) {
	struct token_matcher {
		std::string text;
		token_kind_t tk;
	};

	static const auto token_matchers = std::vector<token_matcher>{
		{"break", tk_break},
		{"const", tk_const},
		{"continue", tk_continue},
		{"else", tk_else},
		{"fn", tk_fn},
		{"if", tk_if},
		{"line", tk_line},
		{"loop", tk_loop},
		{"match", tk_match},
		{"polymorph", tk_polymorph},
		{"return", tk_return},
		{"struct", tk_struct},
		{"type", tk_type},
		{"var", tk_var},
	};

	if (tk == tk_identifier) {
		for (auto &tm : token_matchers) {
			if (token_text == tm.text.c_str()) {
				return tm.tk;
			}
		}
	}
	return tk;
}

void token_t::emit(int &indent_level, token_kind_t &last_tk, bool &indented_line) {
	/* Pretty print this token in a stream. */
	ensure_indented_line(indented_line, indent_level);

	switch (tk) {
	case tk_none:
		assert(false);
	   	break;
	case tk_comment:
		printf("# %s\n", text.c_str());
		break;
	case tk_line:
		printf("line");
		break;
	case tk_struct:
		printf("struct");
		break;
	case tk_polymorph:
		printf("polymorph");
		break;
	case tk_identifier:
		printf("%s", text.c_str());
		break;
	case tk_dot:
		printf(".");
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
	case tk_type:
		printf("type");
		break;
	case tk_fn:
		printf("fn");
		break;
	case tk_var:
		printf("var");
		break;
	case tk_const:
		printf("const");
		break;
	case tk_return:
		printf("return");
		break;

	// Literals
	case tk_char:
		printf("char");
		break;
	case tk_float:
		printf("float");
		break;
	case tk_integer:
		printf("integer");
		break;
	case tk_string:
		printf("string");
		break;

	// Flow control
	case tk_if:
		printf("if");
		break;
	case tk_else:
		printf("else");
		break;
	case tk_loop:
		printf("loop");
		break;
	case tk_continue:
		printf("continue");
		break;
	case tk_break:
		printf("break");
		break;
	case tk_match:
		printf("match");
		break;

	// Operators
	case tk_assign:
		printf("assign");
		break;
	case tk_star:
		printf("star");
		break;

	// Dependency tokens
	case tk_program:
		printf("program");
		break;
	case tk_link:
		printf("link");
		break;
	}
	last_tk = tk;
}

void emit_tokens(const std::vector<token_t> &tokens) {
	int indent_level = 0;
	token_kind_t tk = tk_none;
	bool indented_line = false;
	for (auto token : tokens) {
		token.emit(indent_level, tk, indented_line);
	}
}
