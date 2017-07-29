#include "zion.h"
#include "ast.h"
#include "type_checker.h"
#include "disk.h"
#include "scopes.h"
#include "utils.h"
#include "lexer.h"

namespace ast {
	void log_named_item_create(const char *type, const std::string &name) {
		if (name.size() > 0) {
			debug_lexer(log(log_info, "creating a " c_ast("%s") " named " c_var("%s"),
						type, name.c_str()));
		} else {
			debug_lexer(log(log_info, "creating a " c_ast("%s"), type));
		}
	}

	module_t::module_t(const std::string filename) : filename(filename) {
	}

	std::string module_t::get_canonical_name() const {
		return decl->get_canonical_name();
	}

	token_t module_decl_t::get_name() const {
		return name;
	}

	std::string module_decl_t::get_canonical_name() const {
		static std::string ext = ".llz";
		if (name.text == "_") {
			/* this name is too generic, let's use the leaf filename */
			auto leaf = leaf_from_file_path(name.location.filename);
			if (ends_with(leaf, ext)) {
				return leaf.substr(0, leaf.size() - ext.size());
			} else {
				return leaf;
			}
		} else {
			return name.text;
		}
	}

	item_t::~item_t() throw() {
	}

	type_decl_t::type_decl_t() {
	}

	std::string var_decl_t::get_symbol() const {
        return {token.text};
    }

    location_t var_decl_t::get_location() const  {
        return token.location;
    }

    bound_var_t::ref var_decl_t::resolve_initializer(
            status_t &status,
            llvm::IRBuilder<> &builder,
            scope_t::ref scope,
            life_t::ref life) const
    {
        return initializer->resolve_expression(
				status, builder, scope, life);
    }

	std::string struct_t::get_type_name() const {
		return token.text;
	}

	types::type_t::ref struct_t::get_type() const {
		return null_impl();
	}

	std::string polymorph_t::get_type_name() const {
		return token.text;
	}

	types::type_t::ref polymorph_t::get_type() const {
		return null_impl();
	}
}
