#include "zion.h"
#include "phase_scope_setup.h"
#include "logger_decls.h"
#include "utils.h"
#include "compiler.h"
#include "ast.h"
#include "code_id.h"

/*
 * The idea here is that we need a phase that sets up a directed graph of name
 * resolution and adds names to the appropriate scopes.
 */

void scope_setup_error(status_t &status, const ast::item_t::ref &item, const char *format, ...) {
	va_list args;
	va_start(args, format);
	auto str = string_formatv(format, args);
	va_end(args);

	user_error(status, item->get_location(), "scope-error: %s", str.c_str());
}


unchecked_var_t::ref scope_setup_function_defn(
		status_t &status,
		const ptr<const ast::item_t> &obj,
		identifier::ref id,
		module_scope_t::ref module_scope)
{
	if (id && id->get_name().size() != 0) {
		return module_scope->get_program_scope()->put_unchecked_variable(
				id->get_name(), unchecked_var_t::create(id, obj, module_scope));
	} else {
		scope_setup_error(status, obj, "module-level function definition does not have a name");
		return nullptr;
	}
}

void scope_setup_type_def(
		status_t &status,
	   	const ptr<const ast::user_defined_type_t> &udt,
	   	ptr<module_scope_t> module_scope)
{
	std::string fqn = module_scope->make_fqn(udt->get_type_name());
	module_scope->put_unchecked_type(
			status,
			unchecked_type_t::create(fqn, udt, module_scope));
}

status_t scope_setup_module(compiler_t &compiler, const ast::module_t::ref &module) {
	status_t status;
	auto module_name = module->decl->get_canonical_name();

	/* create this module's LLVM IR representation */
	module_scope_t::ref module_scope;

	auto llvm_module = compiler.llvm_get_program_module();

	/* create a new scope for this module */
	module_scope = compiler.get_program_scope()->new_module_scope(
			module_name, llvm_module);
   	compiler.set_module_scope(module->module_key, module_scope);

	/* add any unchecked tags, types, links, or variables to this module */
	for (auto &user_defined_type : module->user_defined_types) {
		scope_setup_type_def(status, user_defined_type, module_scope);
	}

	for (auto &function : module->functions) {
		scope_setup_function_defn(status, function,
				make_code_id(function->decl->token), module_scope);
	}

	return status;
}

status_t scope_setup_program(const ast::program_t::ref &program, compiler_t &compiler) {
	status_t status;

	/* create the outermost scope of the program */
	for (auto &module : program->modules) {
		assert(module != nullptr);
		status |= scope_setup_module(compiler, module);
	}
	return status;
}
