#include "logger_decls.h"
#include "logger.h"
#include "dbg.h"
#include <sstream>
#include <iterator>
#include <vector>
#include "lexer.h"
#include "parser.h"
#include "json_lexer.h"
#include <regex>
#include <fstream>
#include "compiler.h"
#include "json.h"
#include "disk.h"
#include "utils.h"
#include "llvm_test.h"
#include "llvm_utils.h"

#define test_assert(x) if (!(x)) { \
	log(log_error, "test_assert " c_error(#x) " failed at " c_line_ref("%s:%d"), __FILE__, __LINE__); \
   	return false; \
} else {}

std::vector<token_kind_t> get_tks(lexer_t &lexer, std::vector<token_t> &comments) {
	std::vector<token_kind_t> tks;
	token_t token;
	while (lexer.get_token(token, &comments)) {
		tks.push_back(token.tk);
	}
	return tks;
}

const char *to_str(token_kind_t tk) {
	return tkstr(tk);
}

template <typename T>
bool check_tks_match(T &expect, T &result) {
	auto e_iter = expect.begin();
	auto r_iter = result.begin();
	auto e_end = expect.end();
	auto r_end = result.end();

	while (e_iter != e_end && r_iter != r_end) {
		if (*e_iter != *r_iter) {
			log(log_error, "expected %s, but got %s",
					to_str(*e_iter), to_str(*r_iter));
			return false;
		}
		++e_iter;
		++r_iter;
	}

	bool e_at_end = (e_iter == e_end);
	bool r_at_end = (r_iter == r_end);
	if (e_at_end != r_at_end) {
		const char *who_ended = e_at_end ? "expected and end" : "got a premature end";
		log(log_error, "%s from list", who_ended);
		return false;
	}

	return (e_iter == e_end) && (r_iter == r_end);
}

template <typename T>
void log_list(log_level_t level, const char *prefix, T &xs) {
	std::stringstream ss;
	const char *sep = "";
	for (auto x : xs) {
		ss << sep;
		ss << to_str(x);
		sep = ", ";
	}
	log(level, "%s [%s]", prefix, ss.str().c_str());
}

bool check_lexer(
		std::string text, std::vector<token_kind_t> expect_tks,
	   	std::vector<token_t> &comments)
{
	std::istringstream iss(text);
	lexer_t lexer("check_lexer", iss);
	std::vector<token_kind_t> result_tks = get_tks(lexer, comments);
	if (!check_tks_match(expect_tks, result_tks)) {
		log(log_info, "for text: '%s'", text.c_str());
		log_list(log_info, "expected", expect_tks);
		log_list(log_info, "got     ", result_tks);
		return false;
	} 
	return true;
}

struct lexer_test_t {
	std::string text;
	std::vector<token_kind_t> tks;
};

typedef std::vector<lexer_test_t> lexer_tests;

bool lexer_test_comments(const lexer_tests &tests, std::vector<token_t> &comments) {
	for (auto &test : tests) {
		if (!check_lexer(test.text, test.tks, comments)) {
			return false;
		}
	}
	return true;
}

bool lexer_test(const lexer_tests &tests) {
	std::vector<token_t> comments;
	for (auto &test : tests) {
		if (!check_lexer(test.text, test.tks, comments)) {
			return false;
		}
	}
	return true;
}

/*
 * LEXER TESTS
 */
bool test_lex_blocks() {
	lexer_tests tests = {
		{"A\n\t{ (B\n\n)}",
			{tk_identifier,
				tk_lcurly, tk_lparen, tk_identifier,
				tk_rparen, tk_rcurly}},
		{"A{\n\tB}", {tk_identifier, tk_lcurly, tk_identifier, tk_rcurly}},
	};
	return lexer_test(tests);
}

bool test_lex_comments() {
	lexer_tests tests = {
		{"# hey", {}},
		{"a # hey", {tk_identifier}},
		{"( # hey )", {tk_lparen}},
	};
	std::vector<token_t> comments;
	if (lexer_test_comments(tests, comments)) {
		if (comments.size() != tests.size()) {
			log(log_error, "failed to find the comments");
			return false;
		} else {
			return true;
		}
	} else {
		return false;
	}
}

bool test_lex_functions() {
	lexer_tests tests = {
		{"fn", {tk_fn}},
		{" fn", {tk_fn}},
		{"fn ", {tk_fn}},
		{"_def", {tk_identifier}},
		{"definitely", {tk_identifier}},
		{"fn A", {tk_fn, tk_identifier}},
		{"fn A\n", {tk_fn, tk_identifier}},
		{"fn A{\tstatement}", {tk_fn, tk_identifier, tk_lcurly, tk_identifier, tk_rcurly}},
		{"fn A{\tstatement\n\tstatement}", {tk_fn, tk_identifier, tk_lcurly, tk_identifier, tk_identifier, tk_rcurly}},
		{"fn A{\tpass}", {tk_fn, tk_identifier, tk_lcurly, tk_identifier, tk_rcurly}},
	};
	return lexer_test(tests);
}

bool test_lex_module_stuff() {
	lexer_tests tests = {
		{"module modules", {tk_module, tk_identifier}},
		{"link module foo", {tk_link, tk_module, tk_identifier}},
	};
	return lexer_test(tests);
}

bool test_lex_operators() {
	lexer_tests tests = {
		{"( ),{};[]:", {tk_lparen, tk_rparen, tk_comma, tk_lcurly, tk_rcurly, tk_semicolon, tk_lsquare, tk_rsquare, tk_colon}},
	};
	return lexer_test(tests);
}

bool test_lex_dependency_keywords() {
	lexer_tests tests = {
		{"tote", {tk_identifier}},
		{"link linker", {tk_link, tk_identifier}},
		{"module modules # ignore this", {tk_module, tk_identifier}},
	};
	return lexer_test(tests);
}

bool test_lex_literals() {
	lexer_tests tests = {
		{"\"hello world\\n\" 13493839", {tk_string, tk_integer}},
		{"\"\"", {tk_string}},
		{"0", {tk_integer}},
	};
	return lexer_test(tests);
}

bool test_lex_syntax() {
	lexer_tests tests = {
		{"retur note", {tk_identifier, tk_identifier}},
		{"return note not", {tk_return, tk_identifier, tk_identifier}},
		{"return var = == pass.pass..",
		   	{tk_return, tk_var, tk_assign, tk_assign, tk_assign,
			   	tk_identifier, tk_dot, tk_identifier, tk_dot, tk_dot}},
		{"nil", {tk_identifier}},
		{"loop", {tk_loop}},
		{"if", {tk_if}},
		{"else", {tk_else}},
		{"break", {tk_break}},
		{"breakfast", {tk_identifier}},
		{"continue", {tk_continue}},
		{"continually", {tk_identifier}},
		{"loop {\n\tfoo()}", {tk_loop, tk_lcurly, tk_identifier, tk_lparen, tk_rparen, tk_rcurly}},
		{"true false", {tk_identifier, tk_identifier}},
		{" nothing", {tk_identifier}},
		{"? *", {tk_question, tk_star}},
	};
	return lexer_test(tests);
}

bool test_lex_floats() {
	lexer_tests tests = {
		{"1.0", {tk_float}},
		{"1.0e1", {tk_float}},
		{"123e12 # whatever this is not here\n", {tk_float}},
		{"123.29382974284e12", {tk_float}},
		{"h(3.14159265)", {tk_identifier, tk_lparen, tk_float, tk_rparen}},
	};
	return lexer_test(tests);
}

bool test_lex_types() {
	lexer_tests tests = {
		{"type x struct { x X y Y }", {
			tk_type, tk_identifier, tk_struct,
			tk_lcurly, tk_identifier, tk_identifier,
			tk_identifier, tk_identifier, tk_rcurly}},
		{"type \"List{int}\" polymorph match :\r\n  ; *", {
			tk_type, tk_string, tk_polymorph,
			tk_match, tk_colon, tk_semicolon,
			tk_star}},
	};
	return lexer_test(tests);
}

const char *test_module_name = "-test-";

bool compare_texts(std::string result, std::string expect) {
	/* skips whitespace at the beginning of both strings */
	auto e_i = expect.begin();
	auto e_end = expect.end();
	auto r_i = result.begin();
	auto r_end = result.end();
	for (;e_i != e_end; ++e_i) {
		if (*e_i != ' ') {
			break;
		}
	}

	for (;r_i != r_end; ++r_i) {
		if (*r_i != ' ') {
			break;
		}
	}
	
	while (e_i != e_end && r_i != r_end && *e_i == *r_i) {
		++e_i;
		++r_i;
	}

	return e_i == e_end && r_i == r_end;
}

bool compare_texts(ast::item_t &result, std::string expect) {
	auto printed_result = result.str();
	return compare_texts(printed_result, expect);
}

bool compare_lispy_results(std::string text, ast::item_t &result, std::string expect) {
	if (compare_texts(result, expect)) {
		return true;
	} else {
		auto printed_result = result.str();
		debug(log(log_info, "based on: \n\n%s\n", text.c_str()));
		debug(log(log_info, "expected: \n\n%s\n", expect.c_str()));
		debug(log(log_info, "result: \n\n%s\n", printed_result.c_str()));
		return false;
	}
}

template <typename T, typename... Args>
bool check_parse(std::string text, std::string filename = test_module_name) {
	auto result = parse_text<const T>(text, filename);
	if (!result) {
		debug(log(log_error, "failed to get a parsed result"));
		return false;
	}

	/* make sure we can print back the code without crashing */
	log(log_info, "\n%s", result->get_token().str().c_str());
	return true;
}

/*
 * PARSER TESTS
 */
bool test_parse_minimal_module() {
	return check_parse<ast::module_t>("module minimal");
}

bool test_parse_module_one_function() {
	return check_parse<ast::module_t>("module foobar\n\nfn foo() void {\n\treturn\n}");
}

bool test_parse_module_function_with_return_plus_expr() {
	return check_parse<ast::module_t>(
			"module foobar fn foo() void { return }");
}

bool test_parse_array_literal() {
	// TODO: implement array literals
	return true;
	// return check_parse<ast::expression_t>("[]int [0, 1, 2]");
}

bool test_parse_if_else() {
	return check_parse<ast::module_t>(
		   	"module minmax\n"
			"fn foo(m int, n int) int {"
			"\tif n {"
			"\t\treturn n }\n"
			"\telse if m {\n"
			"\t\treturn m }\n"
			"\telse {\n"
			"\t\treturn m}\n",
			"test" /*module*/);
}

bool test_parse_single_line_when() {
	return check_parse<ast::module_t>(
			"module _\n"
			"fn check() int\n"
			"\twhen x is X\n"
			"\t\treturn 1\n"
			"\treturn 1\n"
			"test" /*module*/);
}

bool test_parse_single_function_call() {
	return check_parse<ast::block_t>(
		   	"{fib(n)}",
			"test" /*module*/);
}

bool test_parse_empty_quote() {
	return check_parse<ast::expression_t>("\"\"", "\"\"");
}

bool test_parse_link_extern_module() {
	return check_parse<ast::module_t>(
		   	"module www\n"
			"link module http\n");
}

bool test_parse_link_extern_function() {
	return check_parse<ast::module_t>(
		   	"module www\n"
			"link fn open(filename str, mode str) int\n");
}

struct expression_check {
	std::string expr_text;
	runtime::variant expected;
};

enum test_output_source_t {
	tos_program,
	tos_compiler_error,
};

const char *tosstr(test_output_source_t tos) {
	switch (tos) {
	case tos_program:
		return c_error("program");
	case tos_compiler_error:
		return c_error("compiler error");
	}
}

bool check_output_contains(test_output_source_t tos, std::string output, std::string expected, bool use_regex) {
	/* make sure that the string output we search does not contain simple ansi
	 * escape sequences */
	std::string result = clean_ansi_escapes(output);
	trim(result);
	if (use_regex) {
		std::smatch match;
		if (std::regex_search(result, match, std::regex(expected.c_str()))) {
			return true;
		}
	}

	if (output == expected) {
		return true;
	}

	return false;
}

bool expect_output_contains(test_output_source_t tos,
	   	std::string output, std::string expected, bool use_regex) {
	if (check_output_contains(tos, output, expected, use_regex)) {
		return true;
	} else {
		if (!verbose()) {
			log(log_error, "output from %s was \n" c_internal("vvvvvvvv") "\n%s" c_internal("^^^^^^^^"),
					tosstr(tos),
					output.c_str());
		}
		log(log_error, "The problem is that we couldn't find \"" c_error("%s") "\" in the output.",
				expected.c_str());
		return false;
	}
}

bool expect_output_lacks(test_output_source_t tos,
	   	std::string output, std::string expected, bool use_regex) {
	if (check_output_contains(tos, output, expected, use_regex)) {
		if (!verbose()) {
			log(log_error, "output from %s was \n" c_internal("vvvvvvvv") "\n%s" c_internal("^^^^^^^^"),
					tosstr(tos),
					output.c_str());
		}
		log(log_error, "The problem is that we found \"" c_error("%s") "\" in the output.",
				expected.c_str());
		return false;
	} else {
		return true;
	}
}

bool get_testable_comments(
		const std::vector<token_t> &comments,
		std::vector<std::string> &error_terms,
	   	std::vector<std::string> &unseen_terms,
		bool &skip_test,
		bool &pass_file) {
	const std::string compile_skip_file = "# test: skip";
	const std::string compile_pass_file = "# test: pass";
	const std::string compile_error_prefix = "# error: ";
	const std::string compile_unseen_prefix = "# unseen: ";

	pass_file = false;
	skip_test = false;
	for (const auto &comment : comments) {
		if (starts_with(comment.text, compile_error_prefix)) {
			error_terms.push_back(comment.text.substr(compile_error_prefix.size()));
		} else if (starts_with(comment.text, compile_unseen_prefix)) {
			unseen_terms.push_back(comment.text.substr(compile_unseen_prefix.size()));
		} else if (starts_with(comment.text, compile_skip_file)) {
			assert(!pass_file);
			skip_test = true;
		} else if (starts_with(comment.text, compile_pass_file)) {
			assert(!skip_test);
			pass_file = true;
		}
	}

	if (error_terms.size() == 0 && unseen_terms.size() == 0) {
		if (!pass_file && !skip_test) {
			log(log_error, "tests must specify error terms, or unseen terms, or pass/skip");
			return false;
		}
	}
	return true;
}

bool _check_compiler_error(compiler_t &compiler, int &skipped) {
	tee_logger tee_log;
	status_t status;
	compiler.build_parse_modules(status);
	std::vector<std::string> error_search_terms, unseen_search_terms;
	bool skip_file = false;
	bool pass_file = false;
	if (!get_testable_comments(compiler.get_comments(), error_search_terms,
				unseen_search_terms, skip_file, pass_file))
   	{
		return false;
	}

	std::string program_name = compiler.get_program_name();
	if (skip_file) {
		++skipped;
		log(log_warning, "skipping compiler error tests of " c_error("%s"),
				program_name.c_str());
		return true;
	} else {
		if (!!status) {
			compiler.build_type_check_and_code_gen(status);
		}

		if (!!status) {
			/* if everything looks good so far, be sure to check all the modules in
			 * the program using LLVM's built-in checker */
			for (auto &llvm_module_pair : compiler.llvm_modules) {
				llvm_verify_module(status, *llvm_module_pair.second);
			}

			if (!pass_file) {
				log(log_error, "compilation of " c_module("%s") c_warn(" succeeded") " but we " c_error("wanted it to fail"),
						program_name.c_str());
				return false;
			} else {
				debug_above(2, log(log_info, "compilation of " c_module("%s") c_good(" succeeded") " which is good",
							program_name.c_str()));
				return true;
			}
		}

		bool checked_something = false;

		for (const auto &search_term : error_search_terms) {
			checked_something = true;
			if (!expect_output_contains(
						tos_compiler_error,
						tee_log.captured_logs_as_string(),
						search_term, true /*use_regex*/)) {
				return false;
			}
		}

		for (const auto &search_term : unseen_search_terms) {
			checked_something = true;
			if (!expect_output_lacks(
						tos_compiler_error,
						tee_log.captured_logs_as_string(),
						search_term, true /*use_regex*/)) {
				return false;
			}
		}

		if (pass_file) {
			log(log_error, "compilation of " c_module("%s") c_warn(" failed") " when " c_error("it should have passed."), program_name.c_str());
			return false;
		}

		if (!checked_something) {
			debug_above(2, log(log_error, "compilation of " c_module("%s") c_warn(" failed") " (which is fine), but " c_error("couldn't find any comment checks."),
						program_name.c_str()));

			return false;
		}

		return true;
	}
}

bool check_compiler_error(std::string module_name, int &skipped) {
	compiler_t compiler(module_name, {".", "lib", "tests"});
	bool result = _check_compiler_error(compiler, skipped);
	if (!result) {
		log(log_info, "\n--- " c_internal("test program listing") " of " c_module("%s") " ---\n%s",
				module_name.c_str(),
				compiler.dump_program_text(module_name).c_str());
	}
	return result;
}

bool check_code_gen_emitted(std::string test_module_name, std::string regex_string) {
	tee_logger tee_log;
	compiler_t compiler(test_module_name, {".", "lib", "tests"});

	status_t status;
	compiler.build_parse_modules(status);
	if (!!status) {
		compiler.build_type_check_and_code_gen(status);
	}

	if (!!status) {
		std::string code_gen = compiler.dump_llvm_modules();
		debug_above(8, log(log_info, "code generated -\n%s", code_gen.c_str()));
		std::smatch match;
		if (std::regex_search(code_gen, match, std::regex(regex_string))) {
			return true;
		} else {
			log(log_error, "could not find regex " c_internal("/%s/") " in code gen", regex_string.c_str());
			return false;
		}
	} else {
		return false;
	}
}

bool test_string_stuff() {
	return !starts_with("abc", "bc")
		   	&& starts_with("abc", "ab")
		   	&& starts_with("abc", "abc")
		   	&& !ends_with("abc", "ab")
		   	&& ends_with("abc", "bc")
		   	&& ends_with("abc", "abc");
}

bool test_utf8() {
	if (utf8_sequence_length(0xe6) != 3) {
		log(log_error, "E6 is a 3-byte utf-8 sequence");
		return false;
	} else if (utf8_sequence_length(0xe5) != 3) {
		log(log_error, "E5 is a 3-byte utf-8 sequence");
		return false;
	} else {
		return true;
	}
}

using test_func = std::function<bool ()>;

struct test_desc {
	std::string name;
	test_func func;
};

#define T(x) {#x, x}

auto test_descs = std::vector<test_desc>{
	T(test_llvm_builder),

	{
		"test_string_format",
		[] () -> bool {
			auto a = string_format("1 %d 3 %s", 2, std::string("four").c_str());
			auto b = string_format("1 %f 3 %s", 2.0f, std::string("four").c_str());
			log(log_info, "a = %s", a.c_str());
			log(log_info, "b = %s", b.c_str());
			return (a == "1 2 3 four" &&
				   	b == "1 2.000000 3 four");
		}
	},
	{
		"test_tee_logger",
		[] () -> bool {
			tee_logger tee_log;
			log(log_info, "So test. Much tee. Wow. %s %d", _magenta("Doge"), 100);
			return tee_log.captured_logs_as_string().find(_magenta("Doge")) != std::string::npos;
		}
	},
	{
		"test_tee_logger_flush",
		[] () -> bool {
			tee_logger tee_log_outer;
			log(log_info, "So test. Much tee. Wow. %s %d", _magenta("Doge"), 100);
			if (true) {
				tee_logger tee_log_nested;

				log(log_info, "This is nested. %s %d", _magenta("Doge"), 200);

				if (tee_log_outer.captured_logs_as_string().find("nested") == std::string::npos) {
					log(log_error, "Nested tee_logger captured text");
					return false;
				}
			}

			return true;
		}
	},
	{
		"test_compiler_build_state",
		[] () -> bool {
			auto filename = "xyz.llz";
			token_t token({filename, 1, 1}, tk_module, "xyz");
			auto module = ast::create<ast::module_t>(token, filename);
			return !!module;
		}
	},
	{
		"test_atoms",
		[] () -> bool {
			test_assert(atom{"a"} == atom{"a"});
			test_assert(atom{"bog"} == atom{"bog"});
			test_assert(!(atom{"a"} == atom{"A"}));

			return true;
		}
	},

	{
		"test_check_output_contains",
		[] () -> bool {
			return check_output_contains(
					tos_compiler_error,
				   	"aks\t " c_good("djf") " hadssdkf street askfjdaskdjf",
					R"(street)", true /*use_regex*/);
		}
	},

	{
		"test_expect_output_lacks",
		[] () -> bool {
			return expect_output_lacks(
					tos_compiler_error,
				   	"aks\t " c_good("djf") " hadssdkf street askfjdaskdjf",
					R"(funky chicken)", true /*use_regex*/);
		}
	},

	{
    	"test_base26",
		[] () -> bool {
			int i = -1;
			log(log_info, "base 26 of %d is %s", i, base26(i).c_str());
			return true;
		}
	},

	T(test_string_stuff),
	T(test_utf8),

	T(test_lex_comments),
	T(test_lex_dependency_keywords),
	T(test_lex_functions),
	T(test_lex_literals),
	T(test_lex_module_stuff),
	T(test_lex_blocks),
	T(test_lex_operators),
	T(test_lex_syntax),
	T(test_lex_floats),
	T(test_lex_types),

	{
		"test_type_algebra",
		[] () -> bool {
			auto a1 = type_id(make_iid("int"));
			return true;
		}
	},

	T(test_parse_empty_quote),
	T(test_parse_if_else),
	T(test_parse_link_extern_function),
	T(test_parse_link_extern_module),
	T(test_parse_array_literal),
	T(test_parse_minimal_module),
	T(test_parse_module_function_with_return_plus_expr),
	T(test_parse_module_one_function),
	T(test_parse_single_function_call),
	{
		"test_code_gen_module_exists",
		[] () -> bool {
			tee_logger tee_log;
			auto test_module_name = "test_puts_emit";
			compiler_t compiler(test_module_name, {".", "lib", "tests"});

			status_t status;
			compiler.build_parse_modules(status);
			if (!!status) {
				compiler.build_type_check_and_code_gen(status);
			}

			if (!status) {
				return false;
			}

			assert(compiler.get_program_scope() != nullptr);
			if (!compiler.get_program_scope()->lookup_module(test_module_name)) {
				log(log_error, "no module %s found", test_module_name);
				return false;
			} else {
				return true;
			}
		}
	},

	{
		"test_read_llir",
		[] () -> bool {
			tee_logger tee_log;
			status_t status;
			compiler_t compiler("rt_str", compiler_t::libs{});
			compiler.llvm_load_ir(status, "rt_str.llir");
			return !!status;
		}
	},

	{
		"test_code_gen_renders",
		[] () -> bool {
			return check_code_gen_emitted("test_puts_emit", "test_puts_emit");
		}
	},

	{
		"test_code_gen_renders_function",
		[] () -> bool {
			return check_code_gen_emitted("test_puts_emit", "declare i64 @puts");
		}
	},

	{
		"test_code_gen_renders_entry",
		[] () -> bool {
			return check_code_gen_emitted("test_puts_emit", "entry:");
		}
	},

};

bool check_filters(std::string name, std::string filter, std::vector<std::string> excludes) {
	if (filter.size() != 0 && name.find(filter.c_str()) == std::string::npos) {
		return false;
	}
	for (auto exclude : excludes) {
		if (name.find(exclude.c_str()) != std::string::npos) {
			return false;
		}
	}
	return true;
}

bool run_tests(std::string filter, std::vector<std::string> excludes) {
	int pass=0, total=0, skipped=0;

	if (getenv("DEBUG") == nullptr) {
		setenv("DEBUG", "1", true /*overwrite*/);
	}

	static bool init_from_files = false;
	if (!init_from_files) {
		init_from_files = true;
		std::vector<std::string> leaf_names;
		std::string tests_errors_dir = "tests";
		auto ext_regex = R"(.+\.llz$)";
		if (list_files(tests_errors_dir, ext_regex, leaf_names)) {
			for (auto leaf_name : leaf_names) {
				auto name = leaf_name;
				assert(regex_exists(name, ext_regex));
				name.resize(name.size() - strlen(".llz"));

				if (starts_with(name, "test_")) {
					/* create a test_desc of this file */
					test_desc test_desc = {
						name,
						[ext_regex, tests_errors_dir, name, &skipped] () {
							auto filename = tests_errors_dir + "/" + name;
							note_logger note_logger(string_format("testing " C_FILENAME " %s " C_RESET "...",
										filename.c_str()));
							return check_compiler_error(filename, skipped);
						}
					};

					test_descs.push_back(test_desc);
				}
			}
			debug_above(2, log(log_info, "found %d .llz test files in tests/errors", leaf_names.size()));
		} else {
			panic("can't find any tests/errors files");
			return false;
		}
	}

	/* run all of the compiler test suite */
	bool success = true;
	std::vector<std::string> failures;
	for (auto &test_desc : test_descs) {
		++total;
		if (check_filters(test_desc.name, filter, excludes)) {
			debug_above(2, log(log_info, "------ " c_test_msg("running %s") " ------", test_desc.name.c_str()));

			bool test_failure = !test_desc.func();

			if (test_failure) {
				debug_above(2, log(log_error, "------ " c_error("✗ ") c_test_msg("%s") c_error(" FAILED ") "------", test_desc.name.c_str()));
				success = false;
				failures.push_back(test_desc.name);
				if (getenv("ALL_TESTS") == nullptr)
					break;
			} else {
				debug_above(2, log(log_info, "------ " c_good("✓ ") c_test_msg("%s") c_good(" PASS ") "------", test_desc.name.c_str()));
				++pass;
			}
		} else {
			debug_above(10, log(log_warning, "------ " c_test_msg("skipping %s") " ------", test_desc.name.c_str()));
			++skipped;
		}
	}
	if (skipped) {
		log(log_warning, c_warn("%d TESTS SKIPPED"), skipped);
	}
	if (success) {
		if (pass != 0) {
			log(log_info, c_good("====== %d TESTS PASSED ======"), pass);
		} else {
			log(log_warning, c_warn("====== NO TESTS WERE RUN ======"), pass);
		}
	} else {
		log(log_error, "====== %d/%d TESTS PASSED (" c_error("%d failures") ", " c_warn("%d skipped") ") ======",
			   	pass, total, total - pass, skipped);
		for (auto fail : failures) {
			log(log_error, "%s failed", fail.c_str());
		}
	}
	return success;
}
