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
			ps.error("expected '%s', got '%s' at %s:%d", \
				   	tkstr(_tk), tkstr(ps.token.tk), \
					__FILE__, __LINE__); \
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
#define chomp_ident(ident) \
	do { \
		expect_ident(ident); \
		ps.advance(); \
	} while (0)

#define expect_ident(ident) \
	do { \
		if (ps.token.tk != tk_identifier || ps.token.text != ident) { \
			ps.error("expected '%s', got '%s' at %s:%d", \
					ident, ps.token.text.c_str(), \
					__FILE__, __LINE__); \
			return nullptr; \
		} \
	} while (0)


ptr<const var_decl_t> var_decl_t::parse(parse_state_t &ps) {
	bool constant = ps.token.is_ident(K(const));
	ps.advance();
	expect_token(tk_identifier);

	auto var_decl = create<ast::var_decl_t>(ps.token);
	var_decl->constant = constant;
	ps.advance();

	var_decl->declared_type = parse_type_ref(ps);
	if (!!ps.status) {
		chomp_token(tk_assign);

		var_decl->initializer = expression_t::parse(ps);
		if (!!ps.status) {
			return var_decl;
		}
	}

	assert(!ps.status);
	return nullptr;
}

ptr<const return_statement_t> return_statement_t::parse(parse_state_t &ps) {
	auto return_statement = create<ast::return_statement_t>(ps.token);
	chomp_ident("return");
	if (ps.token.tk != tk_rcurly) {
		return_statement->expr = expression_t::parse(ps);
		if (!ps.status) {
			assert(!ps.status);
			return nullptr;
		}
	}
	return return_statement;
}

ptr<const item_t> link_statement_parse(parse_state_t &ps) {
	expect_ident(K(link));
	auto link_token = ps.token;
	ps.advance();

	if (ps.token.is_ident(K(fn))) {
		auto link_function_statement = create<ast::link_function_statement_t>(link_token);
		auto function_decl = function_decl_t::parse(ps);
		if (function_decl) {
			link_function_statement->function_name = function_decl->get_token();
			link_function_statement->extern_function = function_decl;
		} else {
			assert(!ps.status);
		}
		return link_function_statement;
	} else if (ps.token.is_ident(K(module))) {
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

	if (ps.token.is_ident(K(var)) || ps.token.is_ident(K(const))) {
		return var_decl_t::parse(ps);
	} else if (ps.token.is_ident(K(set))) {
		return assignment_t::parse(ps);
	} else if (ps.token.is_ident(K(call))) {
		return callsite_expr_t::parse(ps);
	} else if (ps.token.is_ident(K(if))) {
		return if_block_t::parse(ps);
	} else if (ps.token.is_ident(K(loop))) {
		return loop_block_t::parse(ps);
	} else if (ps.token.is_ident(K(match))) {
		return match_block_t::parse(ps);
	} else if (ps.token.is_ident(K(return))) {
		return return_statement_t::parse(ps);
	} else if (ps.token.is_ident(K(continue))) {
		auto continue_flow = create<ast::continue_flow_t>(ps.token);
		eat_token();
		return std::move(continue_flow);
	} else if (ps.token.is_ident(K(break))) {
		auto break_flow = create<ast::break_flow_t>(ps.token);
		eat_token();
		return std::move(break_flow);
	} else {
		ps.error("unexpected token %s", ps.token.text.c_str());
		return nullptr;
	}
}

ptr<const typeid_expr_t> typeid_expr_t::parse(parse_state_t &ps) {
	auto token = ps.token;
	chomp_token(tk_identifier);
	chomp_token(tk_lparen);

	auto value = reference_expr_t::parse(ps);
	if (!!ps.status) {
		assert(value != nullptr);
		auto typeid_expr = ast::create<typeid_expr_t>(token);
		typeid_expr->expr = value;
		chomp_token(tk_rparen);
		return typeid_expr;
	}

	assert(!ps.status);
	return nullptr;
}

ptr<const sizeof_expr_t> sizeof_expr_t::parse(parse_state_t &ps) {
	auto callsite_token = ps.token;
	chomp_token(tk_identifier);
	chomp_token(tk_lparen);
	expect_token(tk_identifier);
	auto sizeof_expr = ast::create<sizeof_expr_t>(callsite_token);
	sizeof_expr->type = parse_type_ref(ps);
	chomp_token(tk_rparen);
	return sizeof_expr;
}

ptr<const expression_t> parse_cast_wrap(parse_state_t &ps, ptr<reference_expr_t> expr) {
    if (ps.token.is_ident(K(as))) {
		auto token = ps.token;
		ps.advance();
		auto cast = ast::create<ast::cast_expr_t>(token);
		cast->lhs = expr;

		expect_token(tk_identifier);
		cast->type_cast = parse_type_ref(ps);
		ps.advance();
        return cast;
    } else {
        return expr;
    }
}

ptr<const callsite_expr_t> callsite_expr_t::parse(parse_state_t &ps) {
	auto callsite = create<callsite_expr_t>(ps.token);
	chomp_ident(K(call));
	callsite->function_expr = expression_t::parse(ps);
	if (!!ps.status) {
		auto params = param_list_t::parse(ps);
		if (!!ps.status) {
			callsite->params = params;
			return callsite;
		} else {
			return nullptr;
		}
	}
	assert(!ps.status);
	return nullptr;
}

ptr<const reference_expr_t> reference_expr_t::parse(parse_state_t &ps) {
	expect_token(tk_identifier);
	auto ref_expr = create<ast::reference_expr_t>(ps.token);
	ref_expr->path.push_back(make_code_id(ps.token));
	while (ps.advance(), ps.token.tk == tk_dot) {
		ps.advance();
		expect_token(tk_identifier);
		ref_expr->path.push_back(make_code_id(ps.token));
	}

	return ref_expr;
}

ternary_expr_t::ref ternary_expr_t::parse(parse_state_t &ps) {
	chomp_ident(K(if));
	auto ternary = create<ternary_expr_t>(ps.token);
	ternary->condition = expression_t::parse(ps);

	if (!!ps.status) {
		chomp_ident(K(then));
		ternary->when_true = expression_t::parse(ps);
		if (!!ps.status) {
			chomp_ident(K(else));
			ternary->when_false = expression_t::parse(ps);

			if (!!ps.status) {
				return ternary;
			}
		}
	}

	assert(!ps.status);
	return nullptr;
}

ptr<const expression_t> expression_t::parse(parse_state_t &ps) {
	if (ps.token.is_ident(K(call))) {
		return callsite_expr_t::parse(ps);
	} else if (ps.token.is_ident(K(if))) {
		return ternary_expr_t::parse(ps);
	} else if (ps.token.is_ident(K(typeid))) {
		return typeid_expr_t::parse(ps);
	} else if (ps.token.is_ident(K(sizeof))) {
		return sizeof_expr_t::parse(ps);
	} else if (ps.token.tk == tk_identifier) {
		return reference_expr_t::parse(ps);
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
	auto assignment = create<ast::assignment_t>(ps.token);
	chomp_ident(K(set));
	assignment->lhs = reference_expr_t::parse(ps);
	if (!!ps.status) {
		chomp_token(tk_assign);
		assignment->rhs = expression_t::parse(ps);
		if (!!ps.status) {
			return assignment;
		}
	}

	assert(!ps.status);
	return nullptr;
}

ptr<const param_list_t> param_list_t::parse(parse_state_t &ps) {
	auto param_list = create<ast::param_list_t>(ps.token);
	chomp_token(tk_lparen);
	int i = 0;
	while (ps.token.tk != tk_rparen) {
		++i;
		auto param = expression_t::parse(ps);
		if (!!ps.status) {
			param_list->expressions.push_back(param);
			if (ps.token.tk == tk_comma) {
				eat_token();
			} else if (ps.token.tk != tk_rparen) {
				ps.error("unexpected token " c_id("%s") " in parameter list", ps.token.text.c_str());
				return nullptr;
			}
			// continue and read the next parameter
		} else {
			break;
		}
	}

	if (!!ps.status) {
		chomp_token(tk_rparen);
		return param_list;
	}

	assert(!ps.status);
	return nullptr;
}

ptr<const block_t> block_t::parse(parse_state_t &ps) {
	auto block = create<ast::block_t>(ps.token);
	auto block_start_token = ps.token;
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
			user_error(ps.status, block_start_token.location, "while parsing this block");
			return nullptr;
		}
	}

	expect_token(tk_rcurly);
	ps.advance();
	return block;
}

ptr<const if_block_t> if_block_t::parse(parse_state_t &ps) {
	auto if_block = create<ast::if_block_t>(ps.token);
	if (ps.token.is_ident(K(if))) {
		ps.advance();
	} else {
		ps.error("expected if or elif");
		return nullptr;
	}

	auto condition = reference_expr_t::parse(ps);
	if (!!ps.status) {
		if_block->condition = condition;
		auto block = block_t::parse(ps);
		if (!!ps.status) {
			if_block->block = block;

			if (ps.prior_token.tk == tk_rcurly) {
				/* check the successive instructions for elif or else */
				if (ps.token.is_ident(K(else))) {
					ps.advance();
					if (ps.token.tk == tk_lcurly) {
						if_block->else_ = block_t::parse(ps);
					} else if (ps.token.is_ident(K(if))) {
						auto else_token = ps.token;
						auto if_stmt = if_block_t::parse(ps);
						if (!!ps.status) {
							auto else_block = ast::create<block_t>(else_token);
							else_block->statements.push_back(if_stmt);
							if_block->else_ = else_block;
						}
					} else {
						ps.error("expected " c_control("if") " or " c_control("{") " after " c_control("else"));
					}
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
	chomp_ident(K(loop));
	loop_block->block = block_t::parse(ps);
	if (!!ps.status) {
		return loop_block;
	}

	assert(!ps.status);
	return nullptr;
}

ptr<const match_block_t> match_block_t::parse(parse_state_t &ps) {
	auto match_block = create<ast::match_block_t>(ps.token);
	chomp_ident(K(match));
	match_block->value = reference_expr_t::parse(ps);
	if (!!ps.status) {
		chomp_token(tk_lcurly);
		while (ps.token.tk == tk_identifier) {
			auto pattern_block = pattern_block_t::parse(ps);
			if (!!ps.status) {
				match_block->pattern_blocks.push_back(pattern_block);
			} else {
				break;
			}
		}
		chomp_token(tk_rcurly);
	}

	if (!!ps.status) {
		return match_block;
	}

	assert(!ps.status);
	return nullptr;
}

ptr<const pattern_block_t> pattern_block_t::parse(parse_state_t &ps) {
	expect_token(tk_identifier);
	auto pattern_block = ast::create<ast::pattern_block_t>(ps.token);
	pattern_block->type_match = parse_type_ref(ps);
	if (!!ps.status) {
		chomp_token(tk_colon);
		auto block = block_t::parse(ps);
		if (!!ps.status) {
			pattern_block->block = block;
			return pattern_block;
		}
	}

	assert(!ps.status);
	return nullptr;
}

ptr<const function_decl_t> function_decl_t::parse(parse_state_t &ps) {
	assert(!!ps.status);
	chomp_ident(K(fn));

	auto function_decl = create<ast::function_decl_t>(ps.token);

	chomp_token(tk_identifier);
	chomp_token(tk_lparen);


	while (!!ps.status && ps.token.tk != tk_rparen) {
		auto dimension = dimension_t::parse(ps);
		if (!!ps.status) {
			function_decl->params.push_back(dimension);
			if (ps.token.tk == tk_comma) {
				eat_token();
				continue;
			} else {
				break;
			}
		}
	}

	if (!!ps.status) {
		chomp_token(tk_rparen);
		expect_token(tk_identifier);
		function_decl->return_type = parse_type_ref(ps);

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
			auto function_defn = create<ast::function_defn_t>(function_decl->get_token());
			function_defn->decl = function_decl;
			function_defn->block = block;
			return function_defn;
		} else {
			user_error(ps.status, function_decl->get_token().location,
				   	"while parsing the block for %s", function_decl->str().c_str());
		}
	}

	assert(!ps.status);
	return nullptr;
}

ptr<const module_decl_t> module_decl_t::parse(parse_state_t &ps, bool skip_module_token) {
	if (!skip_module_token) {
		chomp_ident(K(module));
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
	chomp_ident(K(type));
	expect_token(tk_identifier);
	auto type_name = ps.token;
	ps.advance();

	if (ps.token.is_ident(K(struct))) {
		return struct_t::parse(ps, type_name);
	} else if (ps.token.is_ident(K(polymorph))) {
		return polymorph_t::parse(ps, type_name);
	} else {
		ps.error(
				"type descriptions must begin with '%s' or '%s'. (Found %s)",
				K(struct), K(polymorph),
				ps.token.str().c_str());
		return nullptr;
	}
}

polymorph_t::ref polymorph_t::parse(parse_state_t &ps, token_t type_name) {
	chomp_ident(K(polymorph));
	chomp_token(tk_lcurly);

	auto polymorph = ast::create<polymorph_t>(type_name);
	while (ps.token.tk == tk_identifier) {
		auto subtype = make_code_id(ps.token);
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
	chomp_ident(K(struct));
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

types::type_t::ref parse_type_ref(parse_state_t &ps) {
	std::list<token_kind_t> tks;
	while (ps.token.tk == tk_hat || ps.token.tk == tk_star) {
		tks.push_back(ps.token.tk);
		ps.advance();
	}

	if (ps.token.tk == tk_identifier) {
		auto token = ps.token;
		ps.advance();
		auto type = type_id(make_code_id(token));
		for (auto tk : tks) {
			switch (tk) {
			case tk_star:
				type = type_native_ptr(type);
				break;
			case tk_hat:
				type = type_managed_ptr(type);
				break;
			default:
				break;
			}
		}
		return type;
	} else {
		ps.error("invalid type: expected an identifier (got %s)", ps.token.text.c_str());
		return nullptr;
	}
}

dimension_t::ref dimension_t::parse(parse_state_t &ps) {
	expect_token(tk_identifier);
	token_t var_name = ps.token;
	ps.advance();
	auto dimension = ast::create<dimension_t>(var_name);
	dimension->type = parse_type_ref(ps);
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
		while (ps.token.is_ident(K(link))) {
			auto link_statement = link_statement_parse(ps);
			if (auto linked_module = dyncast<const link_module_statement_t>(link_statement)) {
				module->linked_modules.push_back(linked_module);
			} else if (auto linked_function = dyncast<const link_function_statement_t>(link_statement)) {
				module->linked_functions.push_back(linked_function);
			}
		}
		
		// Get vars, functions or type defs
		while (!!ps.status) {
			if (ps.token.is_ident(K(fn))) {
				/* function definitions */
				auto function = function_defn_t::parse(ps);
				if (!!ps.status) {
					module->functions.push_back(function);
				}
			} else if (ps.token.is_ident(K(type))) {
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
				ps.error("unexpected " c_internal("%s") " '" c_error("%s") "' at top-level module scope",
						tkstr(ps.token.tk), ps.token.text.c_str());
			}
		}
		return module;
	} else {
		assert(!ps.status);
	}
	return nullptr;
}
