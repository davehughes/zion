#pragma once
#include "token.h"

struct status_t;

#ifdef DEBUG_LEXER
#define debug_lexer(x) x
#else
#define debug_lexer(x)
#endif

class lexer_t
{
public:
	lexer_t(std::string filename, std::istream &is);
	~lexer_t();

	void get_token(status_t &status, token_t &token, std::vector<token_t> *comments);

private:
	void reset_token();

	std::string    m_filename;
	std::istream  &m_is;
	int            m_line=1, m_col=1;
};
