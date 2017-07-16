#include "zion.h"
#include "dbg.h"
#include "unchecked_var.h"
#include "ast.h"
#include "bound_type.h"

std::string unchecked_var_t::str() const {
    std::stringstream ss;
    ss << id->str() << " : unchecked var";
    return ss.str();
}

std::string unchecked_data_ctor_t::str() const {
    std::stringstream ss;
    ss << C_ID << id->str() << C_RESET << " : unchecked data ctor : ";
	ss << sig->str();
    return ss.str();
}

types::type_t::ref unchecked_data_ctor_t::get_type(scope_t::ref scope) const {
	return sig;
}

types::type_t::ref unchecked_var_t::get_type(scope_t::ref scope) const {
	/* TODO: plumb status down here */
	status_t status;
	if (auto fn = dyncast<const ast::function_defn_t>(node)) {
		auto decl = fn->decl;
		assert(decl != nullptr);

		/* this is a function declaration, so let's set up our output parameters */
		types::type_t::refs args;
		for (auto &param : decl->params) {
			args.push_back(param->type);
		}

		/* get the return type */
		types::type_function_t::ref sig = type_function(
				type_args(args),
				decl->return_type);

		debug_above(9, log(log_info, "found unchecked type for %s : %s",
					decl->token.str().c_str(),
					sig->str().c_str()));
		return sig;
	} else {
		log(log_warning, "not-impl: get a type from unchecked_var %s", node->str().c_str());
		not_impl();
		return type_unreachable();
	}

	panic("dead end codepath.");
	assert(!status);
	return nullptr;
}

location_t unchecked_var_t::get_location() const {
	return node->get_location();
}
