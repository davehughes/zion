#include <stdlib.h>
#include <string>
#include "ast.h"
#include "token.h"
#include "logger_decls.h"
#include "compiler.h"
#include <csignal>
#include "parse_state.h"
#include "parser.h"
#include "code_id.h"

using namespace ast;


#define eat_token_or_return(fail_code) \
	do { \
		debug_lexer(log(log_info, "eating a %s", tkstr(ps.token.tk))); \
		ps.advance(); \
	} while (0)

#define eat_token() eat_token_or_return(nullptr)

#define expect_token_or_return(_tk, fail_code) \
	do { \
		if (ps.token.tk != _tk) { \
			ps.error("expected '%s', got '%s'", \
				   	tkstr(_tk), tkstr(ps.token.tk)); \
			/* dbg(); */ \
			return fail_code; \
		} \
	} while (0)

#define expect_token(_tk) expect_token_or_return(_tk, nullptr)

#define chomp_token_or_return(_tk, fail_code) \
	do { \
		expect_token_or_return(_tk, fail_code); \
		eat_token_or_return(fail_code); \
	} while (0)
#define chomp_token(_tk) chomp_token_or_return(_tk, nullptr)

ptr<const var_decl_t> var_decl_t::parse(parse_state_t &ps) {
	expect_token(tk_identifier);

	auto var_decl = create<ast::var_decl_t>(ps.token);
	eat_token();

	chomp_token(tk_assign);

	var_decl->initializer = expression_t::parse(ps);
	return var_decl;
}

ptr<const return_statement_t> return_statement_t::parse(parse_state_t &ps) {
	auto return_statement = create<ast::return_statement_t>(ps.token);
	chomp_token(tk_return);
	if (ps.token.tk != tk_rcurly) {
		return_statement->expr = reference_expr_t::parse(ps);
		if (!ps.status) {
			assert(!ps.status);
			return nullptr;
		}
	}
	return return_statement;
}

ptr<const statement_t> link_statement_parse(parse_state_t &ps) {
	expect_token(tk_link);
	auto link_token = ps.token;
	ps.advance();

	if (ps.token.tk == tk_fn) {
		auto link_function_statement = create<ast::link_function_statement_t>(link_token);
		auto function_decl = function_decl_t::parse(ps);
		if (function_decl) {
			link_function_statement->function_name = function_decl->token;
			link_function_statement->extern_function = function_decl;
		} else {
			assert(!ps.status);
		}
		return link_function_statement;
	} else if (ps.token.tk == tk_module) {
		auto module_decl = module_decl_t::parse(ps);
		if (!!ps.status) {
			auto link_statement = create<link_module_statement_t>(link_token);
			link_statement->extern_module = module_decl;

			return link_statement;
		}
	} else {
		ps.error("link must be followed by function declaration or module import");
	}

	assert(!ps.status);
	return nullptr;
}

ptr<const statement_t> statement_t::parse(parse_state_t &ps) {
	assert(ps.token.tk != tk_rcurly);

	if (ps.token.tk == tk_var) {
		eat_token();
		return var_decl_t::parse(ps);
	} else if (ps.token.tk == tk_if) {
		return if_block_t::parse(ps);
	} else if (ps.token.tk == tk_loop) {
		return loop_block_t::parse(ps);
	} else if (ps.token.tk == tk_match) {
		return match_block_t::parse(ps);
	} else if (ps.token.tk == tk_return) {
		return return_statement_t::parse(ps);
	} else if (ps.token.tk == tk_continue) {
		auto continue_flow = create<ast::continue_flow_t>(ps.token);
		eat_token();
		return std::move(continue_flow);
	} else if (ps.token.tk == tk_break) {
		auto break_flow = create<ast::break_flow_t>(ps.token);
		eat_token();
		return std::move(break_flow);
	} else {
		return assignment_t::parse(ps);
	}
}

ptr<const reference_expr_t> reference_expr_t::parse(parse_state_t &ps) {
	if (ps.token.tk == tk_identifier) {
		auto reference_expr = create<ast::reference_expr_t>(ps.token);
		ps.advance();
		return reference_expr;
	} else {
		ps.error("expected an identifier");
		return nullptr;
	}
}

ptr<const typeid_expr_t> typeid_expr_t::parse(parse_state_t &ps) {
	auto token = ps.token;
	chomp_token(tk_identifier);
	chomp_token(tk_lparen);

	auto value = expression_t::parse(ps);
	if (!!ps.status) {
		assert(value != nullptr);
		auto expr = ast::create<typeid_expr_t>(token, value);
		chomp_token(tk_rparen);
		return expr;
	}

	assert(!ps.status);
	return nullptr;
}

ptr<const sizeof_expr_t> sizeof_expr_t::parse(parse_state_t &ps) {
	auto callsite_token = ps.token;
	chomp_token(tk_identifier);
	chomp_token(tk_lparen);
	expect_token(tk_identifier);
	auto expr = ast::create<sizeof_expr_t>(callsite_token, ps.token);
	chomp_token(tk_rparen);
	return expr;
}

ptr<const expression_t> parse_cast_wrap(parse_state_t &ps, ptr<expression_t> expr) {
    if (ps.token.tk == tk_as) {
		auto token = ps.token;
		ps.advance();
		auto cast = ast::create<ast::cast_expr_t>(token);
		cast->lhs = expr;

		expect_token(tk_identifier);
		cast->type_cast = ps.token;
		ps.advance();
        return cast;
    } else {
        return expr;
    }
}

ptr<const callsite_expr_t> callsite_expr_t::parse(parse_state_t &ps, reference_expr_t::ref ref_expr) {
	auto callsite = create<callsite_expr_t>(ps.token);
	auto params = param_list_t::parse(ps);
	if (!!ps.status) {
		callsite->params = params;
		callsite->function_expr = ref_expr;
		return callsite;
	} else {
		return nullptr;
	}
}

ptr<const expression_t> expression_t::parse(parse_state_t &ps) {
	if (ps.token.tk == tk_identifier) {
		if (ps.token.text == "typeid") {
			return typeid_expr_t::parse(ps);
		} else if (ps.token.text == "sizeof") {
			return sizeof_expr_t::parse(ps);
		} else {
			auto ref_expr = reference_expr_t::parse(ps);
			if (!!ps.status) {
				if (ps.token.tk == tk_lparen) {
					return callsite_expr_t::parse(ps, ref_expr);
				} else if (ps.token.tk == tk_question) {
					ps.advance();
					auto when_true_expr = expression_t::parse(ps);
					if (!!ps.status) {
						chomp_token(tk_colon);
						auto when_false_expr = expression_t::parse(ps);
						if (!!ps.status) {
							auto ternary = ast::create<ternary_expr_t>(ref_expr->token);
							ternary->condition = ref_expr;
							ternary->when_true = when_true_expr;
							ternary->when_false = when_false_expr;
							return ternary;
						}
					}
				} else {
					return ref_expr;
				}
			}
		}
	} else {
		return literal_expr_t::parse(ps);
	}

	assert(!ps.status);
	return nullptr;
}

ptr<const expression_t> array_literal_expr_t::parse(parse_state_t &ps) {
	chomp_token(tk_lsquare);
	auto array = create<array_literal_expr_t>(ps.token);
	auto &items = array->items;

	int i = 0;
	while (ps.token.tk != tk_rsquare && ps.token.tk != tk_none) {
		++i;
		auto item = expression_t::parse(ps);
		if (item) {
			items.push_back(item);
		} else {
			assert(!ps.status);
		}
		if (ps.token.tk == tk_comma) {
			ps.advance();
		} else if (ps.token.tk != tk_rsquare) {
			ps.error("found something that does not make sense in an array literal");
			break;
		}
	}
	chomp_token(tk_rsquare);
	return array;
}

ptr<const expression_t> literal_expr_t::parse(parse_state_t &ps) {
	switch (ps.token.tk) {
	case tk_integer:
	case tk_string:
	case tk_char:
	case tk_float:
		{
			auto literal_expr = create<ast::literal_expr_t>(ps.token);
			ps.advance();
			return std::move(literal_expr);
		}
	case tk_lsquare:
		return array_literal_expr_t::parse(ps);
	case tk_lcurly:
		ps.error("unexpected indent");
		return nullptr;

	default:
		ps.error("out of place token '" c_id("%s") "' (%s)",
			   	ps.token.text.c_str(), tkstr(ps.token.tk));
		return nullptr;
	}
}

ptr<const statement_t> assignment_t::parse(parse_state_t &ps) {
	auto ref_expr = reference_expr_t::parse(ps);
	if (!!ps.status) {
		if (ps.token.tk == tk_assign) {
			auto var_decl = create<ast::var_decl_t>(ref_expr->token);
			chomp_token(tk_assign);
			auto initializer = expression_t::parse(ps);
			if (!!ps.status) {
				var_decl->initializer = initializer;
				return var_decl;
			}
		} else if (ps.token.tk == tk_lparen) {
			/* parse a callsite that does not catch its return value */
			return callsite_expr_t::parse(ps, ref_expr);
		} else {
			ps.error("free-standing reference expression results in a noop");
		}
	}

	assert(!ps.status);
	return nullptr;
}

ptr<const param_list_decl_t> param_list_decl_t::parse(parse_state_t &ps) {
	auto param_list_decl = create<ast::param_list_decl_t>(ps.token);
	while (ps.token.tk != tk_rparen) {
		param_list_decl->params.push_back(var_decl_t::parse_param(ps));
		if (ps.token.tk == tk_comma) {
			eat_token();
		} else if (ps.token.tk != tk_rparen) {
			ps.error("unexpected token in param_list_decl");
			return nullptr;
		}
	}
	return param_list_decl;
}

ptr<const param_list_t> param_list_t::parse(parse_state_t &ps) {
	auto param_list = create<ast::param_list_t>(ps.token);
	chomp_token(tk_lparen);
	int i = 0;
	while (ps.token.tk != tk_rparen) {
		++i;
		auto expr = expression_t::parse(ps);
		if (expr) {
			param_list->expressions.push_back(std::move(expr));
			if (ps.token.tk == tk_comma) {
				eat_token();
			} else if (ps.token.tk != tk_rparen) {
				ps.error("unexpected token " c_id("%s") " in parameter list", ps.token.text.c_str());
				return nullptr;
			}
			// continue and read the next parameter
		} else {
			assert(!ps.status);
			return nullptr;
		}
	}
	chomp_token(tk_rparen);

	return param_list;
}

ptr<const block_t> block_t::parse(parse_state_t &ps) {
	auto block = create<ast::block_t>(ps.token);
	chomp_token(tk_lcurly);
	if (ps.token.tk == tk_rcurly) {
		ps.error("empty blocks are not allowed, sorry. use pass.");
		return nullptr;
	}

	while (!!ps.status && ps.token.tk != tk_rcurly) {
		auto statement = statement_t::parse(ps);
		if (!!ps.status) {
			block->statements.push_back(std::move(statement));
		} else {
			assert(!ps.status);
			return nullptr;
		}
	}

	expect_token(tk_rcurly);
	ps.advance();
	return block;
}

ptr<const if_block_t> if_block_t::parse(parse_state_t &ps) {
	auto if_block = create<ast::if_block_t>(ps.token);
	if (ps.token.tk == tk_if) {
		ps.advance();
	} else {
		ps.error("expected if or elif");
		return nullptr;
	}

	auto condition = reference_expr_t::parse(ps);
	if (!ps.status) {
		if_block->condition = condition;
		auto block = block_t::parse(ps);
		if (!!ps.status) {
			if_block->block = block;

			if (ps.prior_token.tk == tk_rcurly) {
				/* check the successive instructions for elif or else */
				if (ps.token.tk == tk_else) {
					ps.advance();
					if_block->else_ = block_t::parse(ps);
				}
			}

			return if_block;
		}
	}

	assert(!ps.status);
	return nullptr;
}

ptr<const loop_block_t> loop_block_t::parse(parse_state_t &ps) {
	auto loop_block = create<ast::loop_block_t>(ps.token);
	chomp_token(tk_loop);
	loop_block->block = block_t::parse(ps);
	if (!!ps.status) {
		return loop_block;
	}

	assert(!ps.status);
	return nullptr;
}

ptr<const pattern_block_t> pattern_block_t::parse(parse_state_t &ps) {
	expect_token(tk_identifier);
	auto type_token = ps.token;
	types::signature type_name = type_token.text;
	chomp_token(tk_colon);
	chomp_token(tk_lcurly);
	auto pattern_block = ast::create<ast::pattern_block_t>(type_token);
	pattern_block->type_name = type_name;

	auto block = block_t::parse(ps);
	if (!!ps.status) {
		pattern_block->block = block;
		return pattern_block;
	}

	assert(!ps.status);
	return nullptr;
}

ptr<const function_decl_t> function_decl_t::parse(parse_state_t &ps) {
	assert(!!ps.status);
	chomp_token(tk_fn);

	auto function_decl = create<ast::function_decl_t>(ps.token);

	chomp_token(tk_identifier);
	chomp_token(tk_lparen);


	while (!!ps.status && ps.token.tk != tk_rparen) {
		auto dimension = dimension_t::parse(ps);
		if (!ps.status) {
			function_decl->params.push_back(dimension);
			if (ps.token.tk == tk_comma) {
				eat_token();
				continue;
			} else {
				expect_token(tk_rparen);
				break;
			}
		}
	}

	if (!!ps.status) {
		expect_token(tk_identifier);
		function_decl->return_type_name = make_iid_impl({ps.token.text}, ps.token.location);

		return function_decl;
	}

	assert(!ps.status);
	return nullptr;
}

ptr<const function_defn_t> function_defn_t::parse(parse_state_t &ps) {
	auto function_decl = function_decl_t::parse(ps);

	if (!!ps.status) {
		assert(function_decl != nullptr);
		auto block = block_t::parse(ps);
		if (!!ps.status) {
			assert(block != nullptr);
			auto function_defn = create<ast::function_defn_t>(function_decl->token);
			function_defn->decl = function_decl;
			function_defn->block = block;
			return function_defn;
		}
	}

	assert(!ps.status);
	return nullptr;
}

ptr<const module_decl_t> module_decl_t::parse(parse_state_t &ps, bool skip_module_token) {
	if (!skip_module_token) {
		chomp_token(tk_module);
	} else {
		/* we've skipped the check for the 'module' token */
	}

	auto module_decl = create<ast::module_decl_t>(ps.token);

	expect_token(tk_identifier);
	module_decl->name = ps.token;
	eat_token();

	return module_decl;
}

identifier::ref make_code_id(const token_t &token) {
	return make_ptr<code_id>(token);
}

identifier::ref make_type_id_code_id(const location_t location, atom var_name) {
	return make_ptr<type_id_code_id>(location, var_name);
}

#if 0
token_t _parse_function_type(parse_state_t &ps) {
	chomp_token_or_return(tk_fn, {});
	chomp_token_or_return(tk_lparen, {});
	std::vector<types::signature> param_type_names;
	atom::map<int> name_index;
	int index = 0;

	while (!!ps.status) {
		if (ps.token.tk == tk_identifier) {
			auto var_name = ps.token;
			ps.advance();
			expect_token_or_return(tk_identifier, {});
			types::signature type_name = ps.token;
			param_type_names.push_back(type_name);
			if (name_index.find(var_name.text) != name_index.end()) {
				ps.error("duplicated parameter name: %s", var_name.text.c_str());
			} else {
				name_index[var_name.text] = index;
				++index;
			}
			if (ps.token.tk == tk_comma) {
				/* advance past a comma */
				ps.advance();
			}
		} else if (ps.token.tk == tk_rparen) {
			ps.advance();
			break;
		} else {
			ps.error("expected a parameter name");
			return token_t{};
		}
	}

	if (!!ps.status) {
		/* now let's parse the return type */
		expect_token_or_return(tk_identifier, {});
		types::signature return_type_name = ps.token;

		return type_function(
				type_args(param_type_names),
				return_type_name);
	}

	assert(!ps.status);
	return {};
}
#endif

identifier::ref reduce_ids(std::list<identifier::ref> ids, location_t location) {
	assert(ids.size() != 0);
	std::stringstream ss;
	const char *inner_sep = SCOPE_SEP;
	const char *sep = "";
	for (auto id : ids) {
		ss << sep << id->get_name();
		sep = inner_sep;
	}
	return make_iid_impl(ss.str(), location);
}

type_decl_t::ref type_decl_t::parse(parse_state_t &ps, token_t name_token) {
	assert(false);

	if (!!ps.status) {
		return create<ast::type_decl_t>(name_token);
	} else {
		return nullptr;
	}
}

ptr<const user_defined_type_t> user_defined_type_t::parse(parse_state_t &ps) {
	chomp_token(tk_type);
	ps.advance();
	expect_token(tk_identifier);
	auto type_name = ps.token;
	ps.advance();

	switch (ps.token.tk) {
	case tk_struct:
		return struct_t::parse(ps, type_name);
	case tk_polymorph:
		return polymorph_t::parse(ps, type_name);
	default:
		ps.error(
				"type descriptions must begin with "
			   	c_id("struct") ", " c_id("polymorph") ". (Found %s)",
				ps.token.str().c_str());
		return nullptr;
	}
}

polymorph_t::ref polymorph_t::parse(parse_state_t &ps, token_t type_name) {
	chomp_token(tk_polymorph);
	chomp_token(tk_lcurly);

	auto polymorph = ast::create<polymorph_t>(type_name);
	while (ps.token.tk == tk_identifier) {
		auto subtype = make_iid_impl({ps.token.text}, ps.token.location);
		if (polymorph->subtypes.find(subtype) != polymorph->subtypes.end()) {
			ps.error("subtype " c_id("%s") " already exists", ps.token.text.c_str()); 
			ps.advance();
		} else {
			polymorph->subtypes.insert(subtype);
			ps.advance();
		}
	}

	if (!!ps.status) {
		chomp_token(tk_rcurly);
		return polymorph;
	}

	assert(!ps.status);
	return nullptr;
}

struct_t::ref struct_t::parse(parse_state_t &ps, token_t type_name) {
	chomp_token(tk_struct);
	chomp_token(tk_lcurly);
	auto struct_ = ast::create<struct_t>(type_name);
	while (ps.token.tk != tk_rcurly) {
		auto dimension = dimension_t::parse(ps);
		if (!!ps.status) {
			struct_->dimensions.push_back(dimension);
		} else {
			break;
		}
	}

	if (!!ps.status) {
		chomp_token(tk_rcurly);
		return struct_;
	}

	assert(!ps.status);
	return nullptr;
}

dimension_t::ref dimension_t::parse(parse_state_t &ps) {
	expect_token(tk_identifier);
	token_t var_name = ps.token;
	ps.advance();
	expect_token(tk_identifier);
	auto dimension = ast::create<dimension_t>(var_name);
	dimension->type_name = make_iid_impl({ps.token.text}, ps.token.location);
	return dimension;
}

ptr<const module_t> module_t::parse(parse_state_t &ps) {
	debug_above(6, log("about to parse %s", ps.filename.str().c_str()));

	auto module_decl = module_decl_t::parse(ps);

	if (module_decl != nullptr) {
		ps.module_id = make_iid(module_decl->get_canonical_name());
		assert(ps.module_id != nullptr);

		auto module = create<ast::module_t>(ps.token, ps.filename);
		module->decl = module_decl;

		// Get links
		while (ps.token.tk == tk_link) {
			auto link_statement = link_statement_parse(ps);
			if (auto linked_module = dyncast<link_module_statement_t>(link_statement)) {
				module->linked_modules.push_back(linked_module);
			} else if (auto linked_function = dyncast<link_function_statement_t>(link_statement)) {
				module->linked_functions.push_back(linked_function);
			}
		}
		
		// Get vars, functions or type defs
		while (!!ps.status) {
			if (ps.token.tk == tk_fn) {
				/* function definitions */
				auto function = function_defn_t::parse(ps);
				if (!!ps.status) {
					module->functions.push_back(function);
				}
			} else if (ps.token.tk == tk_type) {
				/* type definitions */
				auto user_defined_type = user_defined_type_t::parse(ps);
				if (!!ps.status) {
					module->user_defined_types.push_back(user_defined_type);
				}
			} else {
				break;
			}
		}

		if (ps.token.tk != tk_none) {
			if (!!ps.status) {
				ps.error("unexpected '" c_error("%s") "' at top-level module scope",
						tkstr(ps.token.tk));
			}
		}
		return module;
	} else {
		assert(!ps.status);
	}
	return nullptr;
}
