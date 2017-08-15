#pragma once
#include "ast_decls.h"
#include "scopes.h"
#include "life.h"
#include <unordered_map>
#include <map>
#include "llvm_zion.h"

struct status_t;
struct compiler_t;

void type_check_program(
        status_t &status,
		llvm::IRBuilder<> &builder,
		const ast::program_t &obj,
		compiler_t &compiler);

bound_type_t::named_pairs zip_named_pairs(const std::vector<std::string> &names, bound_type_t::refs args);
bound_var_t::ref call_typeid(
		status_t &status,
		scope_t::ref scope,
		life_t::ref life,
		ptr<const ast::item_t> callsite,
		identifier::ref id,
		llvm::IRBuilder<> &builder,
		bound_var_t::ref resolved_value);
