#pragma once
#include "ast_decls.h"
#include "scopes.h"
#include <stack>
#include <unordered_map>

struct compiler_t;

/* the job of the scope_setup is to set up scopes for eventual name
 * resolution at a later phase */
void scope_error(const ast::item_t &item, const char *msg, ...);
status_t scope_setup_program(const ptr<const ast::program_t> &obj, compiler_t &compiler);
unchecked_var_t::ref scope_setup_function_defn(
		status_t &status,
		const ptr<const ast::item_t> &obj,
		std::string symbol,
		module_scope_t::ref module_scope);
