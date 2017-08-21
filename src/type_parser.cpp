#include "zion.h"
#include "type_parser.h"
#include "types.h"
#include "token.h"
#include "code_id.h"
#include "colors.h"
#include "dbg.h"


#define TK_TVAR_BEGIN tk_lsquare
#define TK_TVAR_END tk_rsquare

types::type_t::ref parse_maybe_type(parse_state_t &ps,
	   	identifier::ref supertype_id,
	   	identifier::refs type_variables,
	   	identifier::set generics)
{
	types::type_t::ref type = parse_type(ps, supertype_id, type_variables, generics);
	if (!!ps.status) {
		if (ps.token.tk == tk_question) {
			/* no named maybe generic */
			type = type_maybe(type);
			ps.advance();
			return type;
		} else {
			return type;
		}
	}
	assert(!ps.status);
	return nullptr;
}

bool token_begins_type(token_kind_t tk) {
	return (
			tk == tk_star ||
			tk == tk_identifier ||
			tk == tk_lsquare ||
			tk == TK_TVAR_BEGIN);
}

types::type_t::ref parse_function_type(parse_state_t &ps, identifier::set generics) {
	chomp_ident(K(fn));
	chomp_token(tk_lparen);
	types::type_t::refs param_types;
	types::type_t::ref return_type;
	std::map<std::string, int> name_index;
	int index = 0;

	while (!!ps.status) {
		if (ps.token.tk == tk_identifier) {
			auto var_name = ps.token;
			ps.advance();
			types::type_t::ref type = parse_maybe_type(ps, {}, {}, generics);
			if (!!ps.status) {
				param_types.push_back(type);
				if (name_index.find(var_name.text) != name_index.end()) {
					ps.error("duplicated parameter name: %s",
							var_name.text.c_str());
				} else {
					name_index[var_name.text] = index;
					++index;
				}
				if (ps.token.tk == tk_comma) {
					/* advance past a comma */
					ps.advance();
				}
			}
		} else if (ps.token.tk == tk_rparen) {
			ps.advance();
			break;
		} else {
			ps.error("expected a parameter name");
			return nullptr;
		}
	}

	if (!!ps.status) {
		/* now let's parse the return type */
		if (!ps.line_broke() && token_begins_type(ps.token.tk)) {
			return_type = parse_maybe_type(ps, {}, {}, generics);
		} else {
			return_type = type_void();
		}

		if (!!ps.status) {
			return type_function(
					type_args(param_types),
					return_type);
		}
	}

	assert(!ps.status);
	return nullptr;
}

types::type_t::refs parse_type_operands(
		parse_state_t &ps,
		identifier::ref supertype_id,
		identifier::refs type_variables,
		identifier::set generics)
{
	if (ps.token.tk != TK_TVAR_BEGIN) {
		ps.error("unexpected token %s. expected {",
				ps.token.text.c_str());
		return {};
	}

	types::type_t::refs arguments;

	/* loop over the type arguments */
	while (!!ps.status && (ps.token.tk == tk_identifier || ps.token.is_ident(K(any)))) {
		/* we got an argument, recursively parse */
		auto next_type = parse_maybe_type(ps, supertype_id, type_variables, generics);
		if (!!ps.status) {
			arguments.push_back(next_type);

			if (ps.token.tk == TK_TVAR_END) {
				/* move on */
				break;
			} else if (ps.token.tk == tk_comma) {
				/* if we get a comma, move past it */
				ps.advance();
			} else {
				ps.error("expected ('}' or ','), got %s", tkstr(ps.token.tk));
			}
		}
	}

	if (ps.token.tk == TK_TVAR_END) {
		ps.advance();
	} else {
		ps.error("unexpected token found in type reference argument list, found %s",
				tkstr(ps.token.tk));
	}
	return arguments;
}

#if 0
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
#endif

types::type_t::ref parse_single_type(
		parse_state_t &ps,
	   	identifier::ref supertype_id,
	   	identifier::refs type_variables,
	   	identifier::set generics)
{
	if (!token_begins_type(ps.token.tk)) {
		ps.error("type references cannot begin with %s", ps.token.str().c_str());
		return nullptr;
	}

	if (ps.token.tk == tk_star || ps.token.tk == tk_hat) {
		auto tk = ps.token.tk;
		ps.advance();
		auto type = parse_single_type(ps, supertype_id, type_variables, generics);
		if (!!ps.status) {
			if (tk == tk_hat) {
				type = type_managed(type);
			}
			return ::type_ptr(type);
		}
	} else if (ps.token.is_ident(K(has))) {
		ps.advance();
		chomp_token(TK_TVAR_BEGIN);
		types::type_t::refs dimensions;
		types::name_index_t name_index;
		int index = 0;
		while (!!ps.status && ps.token.tk != TK_TVAR_END) {
			if (!ps.line_broke() && ps.prior_token.tk != TK_TVAR_BEGIN) {
				ps.error("product type dimensions must be separated by a newline");
			}

			token_t var_token;
			expect_token(tk_identifier);
			var_token = ps.token;
			name_index[var_token.text] = index++;
			ps.advance();

			types::type_t::ref dim_type = parse_maybe_type(ps, {}, {}, generics);
			if (!!ps.status) {
				dimensions.push_back(dim_type);
			}
		}
		chomp_token(TK_TVAR_END);
		if (!!ps.status) {
			return ::type_struct(dimensions, name_index);
		} else {
			return nullptr;
		}
	} else if (ps.token.is_ident(K(fn))) {
		return parse_function_type(ps, generics);
	} else if (ps.token.is_ident(K(any))) {
		/* parse generic refs */
		ps.advance();
		types::type_t::ref type;
		if (ps.token.tk == tk_identifier) {
			/* named generic */
			type = type_variable(make_code_id(ps.token));
			ps.advance();
		} else {
			/* no named generic */
			type = type_variable(ps.token.location);
		}
		return type;
	} else if (ps.token.tk == tk_identifier) {
		/* build the type that is referenced here */
		types::type_t::ref cur_type;
		location_t location = ps.token.location;
		identifier::ref id = make_code_id(ps.token);
		ps.advance();

		debug_above(9, log("checking what " c_id("%s") " is", id->str().c_str()));

		/* stash the identifier */
		if (generics.find(id) != generics.end()) {
			/* this type is marked as definitely unbound - aka generic. let's
			 * create a generic for it */
			cur_type = type_variable(id);
		} else {
			/* this is not a generic */
			if (in(id->get_name(), ps.type_macros)) {
				debug_above(9, log("checking whether type " c_id("%s") " expands...",
							id->get_name().c_str()));

				/* macro type expansion */
				cur_type = ps.type_macros[id->get_name()];
			} else {
				/* we don't have a macro/type_name link for this type, so
				 * let's assume it's in this module */
				cur_type = type_id(id);
				debug_above(9, log("transformed " c_id("%s") " to " c_id("%s"),
							id->get_name().c_str(),
							cur_type->str().c_str()));
			}
		}

		types::type_t::refs arguments;
		if (ps.token.tk == TK_TVAR_BEGIN) {
			arguments = parse_type_operands(ps, supertype_id,
					type_variables, generics);
			if (!ps.status) {
				return nullptr;
			}
		}

		for (auto type_arg : arguments) {
			cur_type = type_operator(cur_type, type_arg);
		}

		return cur_type;
	} else if (ps.token.tk == tk_lsquare) {
		ps.advance();
		auto list_element_type = parse_maybe_type(ps, supertype_id,
				type_variables, generics);

		if (ps.token.tk != tk_rsquare) {
			ps.error("list type reference must end with a ']', found %s",
					ps.token.str().c_str());
			return nullptr;
		} else {
			ps.advance();
			return type_list_type(list_element_type);
		}
	} else if (ps.token.tk == TK_TVAR_BEGIN) {
		/* raw tuple type */
		types::type_t::refs arguments = parse_type_operands(ps, supertype_id, type_variables, generics);
		if (!!ps.status) {
			// todo: allow named members
			return ::type_struct(arguments, {});
		}
	} else {
		panic("should have been caught above.");
	}

	not_impl();
	return nullptr;
}

types::type_t::ref parse_type(
		parse_state_t &ps,
	   	identifier::ref supertype_id,
	   	identifier::refs type_variables,
	   	identifier::set generics)
{
	types::type_t::refs options;
	while (!!ps.status) {
		auto type = parse_single_type(ps, supertype_id, type_variables, generics);

		if (!!ps.status) {
			options.push_back(type);
			if (ps.token.tk == tk_or) {
				ps.advance();
				continue;
			} else {
				break;
			}
		}
	}
	
	if (!!ps.status) {
		if (options.size() == 1) {
			return options[0];
		} else {
			types::type_t::ref sum_fn = type_sum_safe(ps.status, options);
			if (!!ps.status) {
				for (auto iter = type_variables.rbegin();
						iter != type_variables.rend();
						++iter)
				{
					sum_fn = ::type_lambda(*iter, sum_fn);
				}

				return sum_fn;
			}
		}
	}
	assert(!ps.status);
	return nullptr;
}

types::type_t::ref parse_type_ref(parse_state_t &ps) {
	return parse_maybe_type(ps, nullptr, {}, {});
}

