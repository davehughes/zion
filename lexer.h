#pragma once
#include "token.h"

#ifdef DEBUG_LEXER
#define debug_lexer(x) x
#else
#define debug_lexer(x)
#endif

class lexer_t
{
public:
	lexer_t(atom filename, std::istream &is);
	~lexer_t();

	bool get_token(token_t &token, std::vector<token_t> *comments);

private:
	void reset_token();

	atom           m_filename;
	std::istream  &m_is;
	int            m_line=1, m_col=1;
};
