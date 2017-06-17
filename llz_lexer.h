#pragma once
#include "llz_token.h"

#ifdef DEBUG_LEXER
#define debug_lexer(x) x
#else
#define debug_lexer(x)
#endif

class llz_lexer_t
{
public:
	llz_lexer_t(atom filename, std::istream &is);
	~llz_lexer_t();

	bool get_token(llz_token_t &token, std::vector<llz_token_t> *comments);

private:
	void reset_token();

	atom           m_filename;
	std::istream  &m_is;
	int            m_line=1, m_col=1;
};
