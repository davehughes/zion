#include "zion.h"
#include "llz_lexer.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include "dbg.h"
#include <csignal>
#include "logger_decls.h"
#include "json_lexer.h"

llz_lexer_t::llz_lexer_t(atom filename, std::istream &is)
	: m_filename(filename), m_is(is)
{
}

bool istchar_start(char ch);
bool istchar(char ch);

#define gts_keyword_case_ex(wor, letter, _gts) \
		case gts_##wor: \
			if (ch != letter) { \
				assert(lltk == lltk_identifier); \
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
				assert(lltk == lltk_identifier); \
				gts = gts_token; \
				scan_ahead = false; \
			} else { \
				lltk = lltk_##word; \
				gts = gts_end; \
				scan_ahead = false; \
			} \
			break;

#define gts_keyword_case_last(word) gts_keyword_case_last_ex(word, gts_##word)

void advance_line_col(char ch, int &line, int &col);

bool llz_lexer_t::get_token(
		llz_token_t &token,
		std::vector<llz_token_t> *comments)
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
	llz_token_kind_t lltk = lltk_none;
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
			switch (ch) {
			case '*':
				gts = gts_end;
				lltk = lltk_star;
				break;
			case '#':
				gts = gts_comment;
				lltk = lltk_comment;
				break;
			case '.':
				gts = gts_end;
				lltk = lltk_dot;
				break;
			case ';':
				gts = gts_end;
				lltk = lltk_semicolon;
				break;
			case ':':
				gts = gts_end;
				lltk = lltk_colon;
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
				lltk = lltk_assign;
				gts = gts_eq;
				break;
			case '\'':
				gts = gts_single_quoted;
				break;
			case '"':
				gts = gts_quoted;
				break;
			case '(':
				lltk = lltk_lparen;
				gts = gts_end;
				break;
			case ')':
				lltk = lltk_rparen;
				gts = gts_end;
				break;
			case ',':
				lltk = lltk_comma;
				gts = gts_end;
				break;
			case '[':
				lltk = lltk_lsquare;
				gts = gts_end;
				break;
			case ']':
				lltk = lltk_rsquare;
				gts = gts_end;
				break;
			case '{':
				lltk = lltk_lcurly;
				gts = gts_end;
				break;
			case '}':
				lltk = lltk_rcurly;
				gts = gts_end;
				break;
			};

			if (gts == gts_start) {
				if (ch == EOF || m_is.fail()) {
					lltk = lltk_none;
					gts = gts_end;
					break;
				} else if (isdigit(ch)) {
					gts = gts_integer;
					lltk = lltk_integer;
				} else if (istchar_start(ch)) {
					gts = gts_token;
					lltk = lltk_identifier;
				} else {
					sequence_length = utf8_sequence_length(ch);
					if (sequence_length > 1) {
						/* assume any non-ascii utf-8 characters are identifier names
						 * for now. */
						--sequence_length;
						gts = gts_token;
						lltk = lltk_identifier;
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
				lltk = lltk_assign;
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
				lltk = lltk_float;
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
				lltk = lltk_float;
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
					assert(lltk = lltk_identifier);
					lltk = lltk_identifier;
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
				lltk = lltk_string;
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
				assert(!"not yet handling multibyte chars as lltk_char - up for debate");
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
				lltk = lltk_char;
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
		if (lltk != lltk_none) {
			token = llz_token_t({m_filename, line, col}, lltk, token_text.str());
		}

		if (ch == EOF) {
			token = llz_token_t({m_filename, line, col}, lltk_none, token_text.str());
		}

		return true;
	}

	return false;
}

llz_lexer_t::~llz_lexer_t() {
}
