#include "zion.h"
#include "lexer.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include "dbg.h"
#include <csignal>
#include "logger_decls.h"
#include "json_lexer.h"

lexer_t::lexer_t(atom filename, std::istream &is)
	: m_filename(filename), m_is(is)
{
}

bool istchar_start(char ch);
bool istchar(char ch);

#define gts_keyword_case_ex(wor, letter, _gts) \
		case gts_##wor: \
			if (ch != letter) { \
				assert(tk == tk_identifier); \
				gts = gts_token; \
				scan_ahead = false; \
			} else { \
				gts = _gts; \
			} \
			break

#define gts_keyword_case(wor, letter, word) gts_keyword_case_ex(wor, letter, gts_##word)

#define gts_keyword_case_last_ex(word, _gts) \
		case _gts: \
			if (istchar(ch)) { \
				assert(tk == tk_identifier); \
				gts = gts_token; \
				scan_ahead = false; \
			} else { \
				tk = tk_##word; \
				gts = gts_end; \
				scan_ahead = false; \
			} \
			break;

#define gts_keyword_case_last(word) gts_keyword_case_last_ex(word, gts_##word)

void advance_line_col(char ch, int &line, int &col);

bool lexer_t::get_token(
		token_t &token,
		std::vector<token_t> *comments)
{
	enum gt_state {
		gts_start,
		gts_end,
		gts_single_quoted,
		gts_single_quoted_got_char,
		gts_single_quoted_escape,
		gts_quoted,
		gts_quoted_escape,
		gts_end_quoted,
		gts_whitespace,
		gts_token,
		gts_error,
		gts_integer,
		gts_float,
		gts_float_symbol,
		gts_expon,
		gts_expon_symbol,
		gts_eq,
		gts_comment,
	};

	gt_state gts = gts_start;
	bool scan_ahead = true;

	char ch = 0;
	size_t sequence_length = 0;
	zion_string_t token_text;
	token_kind_t tk = tk_none;
	int line = m_line;
	int col = m_col;
	while (gts != gts_end && gts != gts_error) {
		ch = m_is.peek();

		switch (gts) {
		case gts_whitespace:
			if (!isspace(ch)) {
				gts = gts_start;
				scan_ahead = false;
			}
			break;
		case gts_comment:
			if (ch == EOF || ch == '\r' || ch == '\n') {
				gts = gts_end;
				scan_ahead = false;
			}
			break;
		case gts_start:
			scan_ahead = true;
			token_text.reset();
			line = m_line;
			col = m_col;
			sequence_length = 0;

			switch (ch) {
			case '*':
				gts = gts_end;
				tk = tk_star;
				break;
			case '#':
				gts = gts_comment;
				tk = tk_comment;
				break;
			case '.':
				gts = gts_end;
				tk = tk_dot;
				break;
			case ';':
				gts = gts_end;
				tk = tk_semicolon;
				break;
			case ':':
				gts = gts_end;
				tk = tk_colon;
				break;
			case '\r':
				gts = gts_whitespace;
				break;
			case '\n':
				gts = gts_whitespace;
				break;
			case '\t':
				gts = gts_whitespace;
				break;
			case ' ':
				gts = gts_whitespace;
				break;
			case '=':
				tk = tk_assign;
				gts = gts_eq;
				break;
			case '\'':
				gts = gts_single_quoted;
				break;
			case '"':
				gts = gts_quoted;
				break;
			case '(':
				tk = tk_lparen;
				gts = gts_end;
				break;
			case ')':
				tk = tk_rparen;
				gts = gts_end;
				break;
			case ',':
				tk = tk_comma;
				gts = gts_end;
				break;
			case '[':
				tk = tk_lsquare;
				gts = gts_end;
				break;
			case ']':
				tk = tk_rsquare;
				gts = gts_end;
				break;
			case '{':
				tk = tk_lcurly;
				gts = gts_end;
				break;
			case '}':
				tk = tk_rcurly;
				gts = gts_end;
				break;
			};

			if (gts == gts_start) {
				if (ch == EOF || m_is.fail()) {
					tk = tk_none;
					gts = gts_end;
					break;
				} else if (isdigit(ch)) {
					gts = gts_integer;
					tk = tk_integer;
				} else if (istchar_start(ch)) {
					gts = gts_token;
					tk = tk_identifier;
				} else {
					sequence_length = utf8_sequence_length(ch);
					if (sequence_length > 1) {
						/* assume any non-ascii utf-8 characters are identifier names
						 * for now. */
						--sequence_length;
						gts = gts_token;
						tk = tk_identifier;
					} else {
						log(log_warning, "unknown character parsed at start of token (0x%02x) '%c'",
								(int)ch,
								isprint(ch) ? ch : '?');
						gts = gts_error;
					}
				}
			}
			break;
		case gts_eq:
			gts = gts_end;
			if (ch == '=') {
				tk = tk_assign;
			} else {
				scan_ahead = false;
			}
			break;
		case gts_float_symbol:
			if (!isdigit(ch)) {
				gts = gts_error;
			} else {
				gts = gts_float;
			}
			break;
		case gts_float:
			if (ch == 'e' || ch == 'E') {
				gts = gts_expon_symbol;
			} else if (!isdigit(ch)) {
				tk = tk_float;
				gts = gts_end;
				scan_ahead = false;
			}
			break;
		case gts_expon_symbol:
			if (ch < '1' || ch > '9') {
				gts = gts_error;
			} else {
				gts = gts_expon;
			}
			break;
		case gts_expon:
			if (!isdigit(ch)) {
				gts = gts_end;
				tk = tk_float;
				scan_ahead = false;
			}
			break;
		case gts_integer:
			if (ch == 'e') {
				gts = gts_expon_symbol;
			} else if (ch == '.') {
				gts = gts_float_symbol;
			} else if (!isdigit(ch)) {
				gts = gts_end;
				scan_ahead = false;
			}
			break;
		case gts_token:
			if (sequence_length > 0) {
				--sequence_length;
			} else if (!istchar(ch)) {
				sequence_length = utf8_sequence_length(ch);
				if (sequence_length > 1) {
					--sequence_length;
				} else {
					assert(tk = tk_identifier);
					tk = tk_identifier;
					gts = gts_end;
					scan_ahead = false;
				}
			} else {
				/* we can transition into gts_token from other near-collisions with keywords */
				scan_ahead = true;
			}
			break;

		case gts_quoted:
			if (ch == EOF) {
				gts = gts_error;
			} else if (sequence_length > 0) {
				--sequence_length;
			} else if (ch == '\\') {
				gts = gts_quoted_escape;
			} else if (ch == '"') {
				tk = tk_string;
				gts = gts_end_quoted;
			} else {
				sequence_length = utf8_sequence_length(ch);
				if (sequence_length != 0)
					--sequence_length;
 			}
			break;
		case gts_end_quoted:
			gts = gts_end;
			scan_ahead = false;
			break;
		case gts_quoted_escape:
			gts = gts_quoted;
			break;
		case gts_single_quoted:
			if (sequence_length > 0) {
				assert(!"not yet handling multibyte chars as tk_char - up for debate");
			} else if (ch == '\\') {
				gts = gts_single_quoted_escape;
			} else if (ch == '\'') {
				gts = gts_error;
			} else {
				gts = gts_single_quoted_got_char;
 			}
			break;
		case gts_single_quoted_escape:
			gts = gts_single_quoted_got_char;
			break;
		case gts_single_quoted_got_char:
			if (ch != '\'') {
				gts = gts_error;
			} else {
				tk = tk_char;
				gts = gts_end;
			}
			break;
		case gts_error:
			log(log_warning, "token lexing error occurred, so far = (%s)", token_text.c_str());
			break;
		case gts_end:
			break;
		}

		if (scan_ahead && gts != gts_error) {
			char ch_old = ch;
			m_is.get(ch);
			if (ch == '\n') {
				++m_line;
				m_col = 1;
			} else {
				++m_col;
			}
			assert(ch == ch_old);

			if (!m_is.fail() && !token_text.append(ch)) {
				log(log_error, "symbol too long? [line %d: col %d]",
						m_line, m_col);
				return false;
			}
		}
	}

	if (gts != gts_error) {
		if (tk != tk_none) {
			tk = translate_lltk(tk, token_text);
			debug_above(12, log(log_info, "got llz token %s %s",
						tkstr(tk), token_text.c_str()));

			token = token_t({m_filename, line, col}, tk, token_text.str());
		}

		if (ch == EOF) {
			token = token_t({m_filename, line, col}, tk_none, token_text.str());
			return false;
		}

		return true;
	}

	return false;
}

lexer_t::~lexer_t() {
}

