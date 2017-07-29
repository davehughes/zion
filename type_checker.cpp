#include "zion.h"
#include "logger.h"
#include "type_checker.h"
#include "utils.h"
#include "callable.h"
#include "compiler.h"
#include "llvm_zion.h"
#include "llvm_utils.h"
#include "json.h"
#include "ast.h"
#include "llvm_types.h"
#include "parser.h"
#include "unification.h"
#include "code_id.h"
#include "patterns.h"
#include <iostream>

/*
 * The basic idea here is that type checking is a graph operation which can be
 * ordered topologically based on dependencies between callers and callees.
 * Luckily our AST has exactly that structure.  We will perform a topological
 * sort by resolving types as we return from our depth first traversal.
 */


/************************************************************************/

bound_type_t::ref get_fully_bound_dimension(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		const ast::dimension_t::ref &obj,
		atom &var_name)
{
	if (!!status) {
		/* get the name of this parameter */
		var_name = obj->token.text;

		assert(obj->type != nullptr);

		/* the user specified a type */
		debug_above(6, log(log_info, "upserting type for param %s",
					obj->type->str().c_str()));
		return upsert_bound_type(status, builder, scope, obj->type);
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref generate_stack_variable(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		const ast::var_decl_t &obj,
		atom symbol,
		types::type_t::ref declared_type,
		bool maybe_unbox)
{
	/* 'init_var' is keeping track of the value we are assigning to our new
	 * variable (if any exists.) */

	/* only check initializers inside a runnable scope */
	assert(dyncast<runnable_scope_t>(scope) != nullptr);

	/* we have an initializer */
	bound_var_t::ref init_var = obj.resolve_initializer(status, builder, scope, life);

	/* 'type' is keeping track of what the variable's ending type will be */
	bound_type_t::ref bound_type;
	types::type_t::ref lhs_type = declared_type;

	/* 'unboxed' tracks whether we are doing maybe unboxing for this var_decl */
	bool unboxed = false;

	if (!!status) {
		if (init_var != nullptr) {
			/* we have an initializer */
			if (declared_type != nullptr) {
				/* ensure 'init_var' <: 'declared_type' */
				unification_t unification = unify(
						declared_type,
						init_var->get_type(),
						scope->get_typename_env());

				if (unification.result) {
					/* the lhs is a supertype of the rhs */
					lhs_type = declared_type->rebind(unification.bindings);
				} else {
					/* report that the variable type does not match the initializer type */
					user_error(status, obj.get_location(),
							"declared type of `" c_var("%s") "` does not match type of initializer",
							obj.get_symbol().c_str());
					user_message(log_info, status, init_var->get_location(), c_type("%s") " != " c_type("%s"),
							declared_type->str().c_str(),
							init_var->type->str().c_str());
				}
			} else {
				/* we must get the type from the initializer */
				lhs_type = init_var->type->get_type();
			}
		}

		assert(lhs_type != nullptr);

		if (maybe_unbox) {
			debug_above(3, log(log_info, "attempting to unbox %s", obj.get_symbol().c_str()));

			/* try to see if we can unbox this if it's a Maybe */
			if (init_var == nullptr) {
				user_error(status, obj.get_location(), "missing initialization value");
			} else {
				/* since we are maybe unboxing, then let's first off see if
				 * this is even a maybe type. */
				if (auto maybe_type = dyncast<const types::type_maybe_t>(lhs_type)) {
					/* looks like the initialization variable is a supertype
					 * of the nil type */
					unboxed = true;

					bound_type = upsert_bound_type(status, builder, scope,
							maybe_type->just);
				} else {
					/* this is not a maybe, so let's just move along */
				}
			}
		}

		if (bound_type == nullptr) {
			bound_type = upsert_bound_type(status, builder, scope, lhs_type);
		}
	}

	if (!!status) {
		/* generate the mutable stack-based variable for this var */
		llvm::Function *llvm_function = llvm_get_function(builder);
		llvm::AllocaInst *llvm_alloca = llvm_create_entry_block_alloca(llvm_function,
				bound_type, symbol);

		if (init_var) {
			if (!init_var->type->get_type()->is_nil()) {
				debug_above(6, log(log_info, "creating a store instruction %s := %s",
							llvm_print(llvm_alloca).c_str(),
							llvm_print(init_var->get_llvm_value()).c_str()));

				llvm::Value *llvm_init_value = init_var->resolve_value(builder);
				if (llvm_init_value->getName().size() == 0) {
					llvm_init_value->setName(string_format("%s.initializer", symbol.c_str()));
				}

				builder.CreateStore(
						llvm_maybe_pointer_cast(builder, llvm_init_value,
							bound_type->get_llvm_specific_type()),
						llvm_alloca);
			} else {
				llvm::Constant *llvm_null_value = llvm::Constant::getNullValue(bound_type->get_llvm_specific_type());
				builder.CreateStore(llvm_null_value, llvm_alloca);
			}
		} else if (dyncast<const types::type_maybe_t>(lhs_type)) {
			/* this can be null, let's initialize it as such */
			llvm::Constant *llvm_null_value = llvm::Constant::getNullValue(bound_type->get_llvm_specific_type());
			builder.CreateStore(llvm_null_value, llvm_alloca);
		} else {
			user_error(status, obj.get_location(), "missing initializer");
		}

		if (!!status) {
			/* the reference_expr that looks at this llvm_value will need to
			 * know to use store/load semantics, not just pass-by-value */
			bound_var_t::ref var_decl_variable = bound_var_t::create(INTERNAL_LOC(), symbol,
					bound_type, llvm_alloca, make_type_id_code_id(obj.get_location(), obj.get_symbol()),
					true /*is_lhs*/, false /*is_global*/);

			/* memory management */
			call_addref_var(status, builder, scope, var_decl_variable,
					string_format("variable %s initialization", symbol.c_str()));
			life->track_var(builder, var_decl_variable, lf_block);

			/* on our way out, stash the variable in the current scope */
			scope->put_bound_variable(status, var_decl_variable->name,
					var_decl_variable);

			if (!!status) {
				if (unboxed) {
					/* 'condition_value' refers to whether this was an unboxed maybe */
					bound_var_t::ref condition_value;

					assert(init_var != nullptr);
					assert(maybe_unbox);

					/* get the maybe type so that we can use it as a conditional */
					bound_type_t::ref condition_type = upsert_bound_type(status, builder, scope, lhs_type);

					/* we're unboxing a Maybe{any}, so let's return
					 * whether this was Empty or not... */
					return bound_var_t::create(INTERNAL_LOC(), symbol,
							condition_type, init_var->resolve_value(builder),
							make_type_id_code_id(obj.get_location(), obj.get_symbol()),
							false /*is_lhs*/,
							false /*is_global*/);
				} else {
					return var_decl_variable;
				}
			}
		}
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref generate_module_variable(
		status_t &status,
		llvm::IRBuilder<> &builder,
		function_scope_t::ref scope,
		life_t::ref life,
		const ast::var_decl_t &obj,
		atom symbol,
		types::type_t::ref declared_type)
{
	/* 'init_var' is keeping track of the value we are assigning to our new
	 * variable (if any exists.) */
	bound_var_t::ref init_var;

	if (obj.initializer) {
		/* we have an initializer */
		init_var = obj.initializer->resolve_expression(status, builder,
				scope, life);
	}

	types::type_t::ref lhs_type = declared_type;
	if (init_var != nullptr) {
		/* we have an initializer */
		if (declared_type != nullptr) {
			/* ensure 'init_var' <: 'declared_type' */
			unification_t unification = unify(
					declared_type,
					init_var->get_type(),
					scope->get_typename_env());

			if (unification.result) {
				/* the lhs is a supertype of the rhs */
				lhs_type = declared_type->rebind(unification.bindings);
			} else {
				/* report that the variable type does not match the initializer type */
				user_error(status, obj.get_location(), "declared type of `" c_var("%s") "` does not match type of initializer",
						obj.token.text.c_str());
				user_message(log_info, status, init_var->get_location(), c_type("%s") " != " c_type("%s"),
						declared_type->str().c_str(),
						init_var->type->str().c_str());
			}
		} else {
			/* we must get the type from the initializer */
			lhs_type = init_var->type->get_type();
		}
	}

	if (!!status) {
		/* 'type' is keeping track of what the variable's ending type will be */
		assert(lhs_type != nullptr);
		bound_type_t::ref bound_type = upsert_bound_type(status, builder, scope, lhs_type);

		/* generate the immutable global variable for this var */
		llvm::Constant *llvm_constant = nullptr;
		if (bound_type->get_llvm_specific_type()->isPointerTy()) {
			llvm_constant = llvm::Constant::getNullValue(bound_type->get_llvm_specific_type());
		} else {
			user_error(status, obj.get_location(),
				   	"unsupported type for module variable %s. currently only managed types are supported",
					bound_type->str().c_str());
		}

		if (!!status) {
			llvm::GlobalVariable *llvm_global_variable = llvm_get_global(
					llvm_get_module(builder),
					symbol.str(),
					llvm_constant,
					false /*is_constant*/);

			if (init_var != nullptr) {
				debug_above(6, log(log_info, "creating a store instruction %s := %s",
							llvm_print(llvm_global_variable).c_str(),
							llvm_print(init_var->get_llvm_value()).c_str()));

				llvm::Value *llvm_init_value = init_var->resolve_value(builder);

				if (llvm_init_value->getName().str().size() == 0) {
					llvm_init_value->setName(string_format("%s.initializer", symbol.c_str()));
				}

				builder.CreateStore(
						llvm_maybe_pointer_cast(builder, llvm_init_value,
							bound_type->get_llvm_specific_type()),
						llvm_global_variable);
			} else {
				user_error(status, obj.get_location(),
					   	"module var " c_id("%s") " missing initializer",
						symbol.c_str());
			}

			if (!!status) {
				/* the reference_expr that looks at this llvm_value will need to
				 * know to use store/load semantics, not just pass-by-value */
				bound_var_t::ref var_decl_variable = bound_var_t::create(INTERNAL_LOC(), symbol,
						bound_type, llvm_global_variable, make_code_id(obj.token),
						false /*is_lhs*/, true /*is_global*/);

				/* on our way out, stash the variable in the current scope */
				scope->get_module_scope()->put_bound_variable(status, var_decl_variable->name,
						var_decl_variable);

				return var_decl_variable;
			}
		}
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref type_check_bound_var_decl(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		const ast::var_decl_t &obj,
		life_t::ref life,
		bool maybe_unbox)
{
	const atom symbol = obj.get_symbol();

	debug_above(4, log(log_info, "type_check_var_decl is looking for a type for variable " c_var("%s") " : %s",
				symbol.c_str(), obj.get_symbol().c_str()));

	assert(dyncast<module_scope_t>(scope) == nullptr);
	if (scope->has_bound_variable(symbol, rc_capture_level)) {
		user_error(status, obj.get_location(), "variables cannot be redeclared");
		return nullptr;
	}

	if (!!status) {
		/* 'declared_type' tells us the user-declared type on the left-hand side of
		 * the assignment. this is generally used to allow a variable to be more
		 * generalized than the specific right-hand side initial value might be. */
		types::type_t::ref declared_type = obj.declared_type;

		assert(dyncast<runnable_scope_t>(scope) != nullptr);

		return generate_stack_variable(status, builder, scope, life,
				obj, symbol, declared_type, maybe_unbox);
	}

	assert(!status);
	return nullptr;
}

bound_type_t::named_pairs zip_named_pairs(
		atom::many names,
		bound_type_t::refs args)
{
	bound_type_t::named_pairs named_args;
	assert(names.size() == args.size());
	for (size_t i = 0; i < args.size(); ++i) {
		named_args.push_back({names[i], args[i]});
	}
	return named_args;
}

void get_fully_bound_dimensions(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		const std::vector<ast::dimension_t::ref> &dimensions,
		bound_type_t::named_pairs &params)
{
	if (!!status) {
		for (auto dimension : dimensions) {
			atom var_name;
			bound_type_t::ref param_type = get_fully_bound_dimension(status,
					builder, scope, dimension, var_name);

			if (!!status) {
				params.push_back({var_name, param_type});
			}
		}
	}
}

void type_check_fully_bound_function_decl(
		status_t &status,
		llvm::IRBuilder<> &builder,
		const ast::function_decl_t &obj,
		scope_t::ref scope,
		bound_type_t::named_pairs &params,
		bound_type_t::ref &return_type)
{
	/* returns the parameters and the return value types fully resolved */
	debug_above(4, log(log_info, "type checking function decl %s", obj.token.str().c_str()));

	/* the parameter types as per the decl */
	get_fully_bound_dimensions(status, builder, scope, obj.params, params);

	if (!!status) {
		return_type = upsert_bound_type(status, builder, scope, obj.return_type);

		/* we got the params, and the return value */
		return;
	}

	assert(!status);
}

bool type_is_unbound(types::type_t::ref type, types::type_t::map bindings) {
	return type->rebind(bindings)->ftv_count() > 0;
}

function_scope_t::ref make_param_list_scope(
		status_t &status,
		llvm::IRBuilder<> &builder,
		const ast::function_decl_t &function_decl,
		scope_t::ref &scope,
		life_t::ref life,
		bound_var_t::ref function_var,
		bound_type_t::named_pairs params)
{
	assert(!!status);
	assert(life->life_form == lf_function);

	if (!!status) {
		auto new_scope = scope->new_function_scope(
				string_format("function-%s", function_var->name.c_str()));

		assert(function_decl.params.size() == params.size());

		llvm::Function *llvm_function = llvm::cast<llvm::Function>(function_var->get_llvm_value());
		llvm::Function::arg_iterator args = llvm_function->arg_begin();

		int i = 0;

		for (auto &param : params) {
			llvm::Value *llvm_param = &(*args++);
			if (llvm_param->getName().str().size() == 0) {
				llvm_param->setName(param.first.str());
			}

			/* create an alloca in order to be able to reassign the named
			 * parameter to a new value. this does not mean that the parameter
			 * is an out param, we are simply enabling reuse of the name */
			llvm::AllocaInst *llvm_alloca = llvm_create_entry_block_alloca(
					llvm_function, param.second, param.first.str());

			// REVIEW: how to manage memory for named parameters? if we allow
			// changing their value then we have to enforce addref/release
			// semantics on them...
			debug_above(6, log(log_info, "creating a local alloca for parameter %s := %s",
						llvm_print(llvm_alloca).c_str(),
						llvm_print(llvm_param).c_str()));
			builder.CreateStore(llvm_param, llvm_alloca);	

			auto param_var = bound_var_t::create(INTERNAL_LOC(), param.first, param.second,
					llvm_alloca, make_code_id(function_decl.params[i++]->token),
					true /*is_lhs*/, false /*is_global*/);

			bound_type_t::ref return_type = get_function_return_type(scope, function_var->type);

			// TODO: an easy optimization here (to avoid the addref/release
			// overhead would be to check whether this symbol is on the LHS of
			// any assignment operations within this function AST's body. For
			// now, we'll be safe.
			call_addref_var(status, builder, scope, param_var, "function parameter lifetime");

			if (!!status) {
				life->track_var(builder, param_var, lf_function);
			}

			if (!!status) {
				/* add the parameter argument to the current scope */
				new_scope->put_bound_variable(status, param.first, param_var);
			}

			if (!status) {
				break;
			}
		}

		if (!!status) {
			return new_scope;
		}
	}

	assert(!status);
	return nullptr;
}

void ast::link_module_statement_t::resolve_linked_module(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope) const
{
	module_scope_t::ref module_scope = dyncast<module_scope_t>(scope);
	assert(module_scope != nullptr);

	auto linked_module_name = extern_module->get_canonical_name();
	assert(linked_module_name.size() != 0);

	program_scope_t::ref program_scope = scope->get_program_scope();
	module_scope_t::ref linked_module_scope = program_scope->lookup_module(linked_module_name);

	if (linked_module_scope != nullptr) {
		/* put the module into program scope as a named variable. this is to
		 * enable dot-expressions to resolve module scope lookups. note that
		 * the module variables are not reified into the actual generated LLVM
		 * IR.  they are resolved entirely at compile time.  perhaps in a
		 * future version they can be used as run-time variables, so that we
		 * can pass modules around for another level of polymorphism. */
		bound_module_t::ref module_variable = bound_module_t::create(INTERNAL_LOC(),
				{extern_module->get_name().text}, make_code_id(token), linked_module_scope);

		module_scope->put_bound_variable(status, module_variable->name, module_variable);

		if (!!status) {
			return;
		}
	} else {
		user_error(status, get_location(), "can't find module %s", linked_module_name.c_str());
	}

	assert(!status);
	return;
}

bound_var_t::ref ast::link_function_statement_t::resolve_linked_function(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope) const
{
	/* FFI */
	module_scope_t::ref module_scope = dyncast<module_scope_t>(scope);
	assert(module_scope);

	if (!scope->has_bound_variable(function_name.text, rc_just_current_scope)) {
		bound_type_t::named_pairs named_args;
		bound_type_t::ref return_value;

		type_check_fully_bound_function_decl(status, builder, *extern_function,
				scope, named_args, return_value);

		if (!!status) {
			bound_type_t::refs args;
			for (auto &named_arg_pair : named_args) {
				args.push_back(named_arg_pair.second);
			}

			// TODO: rearrange this, and get the pointer type
			llvm::FunctionType *llvm_func_type = llvm_create_function_type(
					status, builder, args, return_value);

			/* try to find this function, if it already exists... */
			llvm::Module *llvm_module = module_scope->get_llvm_module();
			llvm::Value *llvm_value = llvm_module->getOrInsertFunction(function_name.text,
					llvm_func_type);

			assert(llvm_print(llvm_value->getType()) != llvm_print(llvm_func_type));

			/* get the full function type */
			types::type_function_t::ref function_sig = get_function_type(
					args, return_value);
			debug_above(3, log(log_info, "%s has type %s",
						function_name.str().c_str(),
						function_sig->str().c_str()));

			/* actually create or find the finalized bound type for this function */
			bound_type_t::ref bound_function_type = upsert_bound_type(
					status, builder, scope, function_sig);

			return bound_var_t::create(
					INTERNAL_LOC(),
				   	scope->make_fqn(function_name.text),
				   	bound_function_type,
				   	llvm_value,
				   	make_code_id(extern_function->token),
				   	false /*is_lhs*/,
				   	false /*is_global*/);
		}
	} else {
		auto bound_var = scope->get_bound_variable(status, get_location(),
				function_name.text);
		user_error(status, get_location(), "name of " c_id("%s") " conflicts with %s",
			   function_name.text.c_str(), bound_var->str().c_str());
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::callsite_expr_t::resolve_expression(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life) const
{
	/* get the value of calling a function */
	bound_type_t::refs param_types;
	bound_var_t::refs arguments;

	if (auto symbol = dyncast<const ast::reference_expr_t>(function_expr)) {
		if (symbol->get_token().text == "static_print") {
			if (params->expressions.size() == 1) {
				auto param = params->expressions[0];
				bound_var_t::ref param_var = param->resolve_expression(
						status, builder, scope, life);

				if (!!status) {
					user_message(log_info, status, param->get_location(),
							"%s : %s", param->str().c_str(),
							param_var->type->str().c_str());
					return nullptr;
				}

				assert(!status);
				return nullptr;
			} else {
				user_error(status, get_location(),
					   	"static_print requires one and only one parameter");

				assert(!status);
				return nullptr;
			}
		}
	}

	if (params != nullptr) {
		/* iterate through the parameters and add their values to a vector */
		for (auto &param : params->expressions) {
			bound_var_t::ref param_var = param->resolve_expression(
					status, builder, scope, life);

			if (!status) {
				break;
			}

			arguments.push_back(param_var);
			param_types.push_back(param_var->type);
		}
	} else {
		/* the callsite has no parameters */
	}

	if (!!status) {
		if (auto can_reference_overloads = dyncast<const can_reference_overloads_t>(function_expr)) {
			/* we need to figure out which overload to call, if there are any */
			bound_var_t::ref function = can_reference_overloads->resolve_overrides(
					status, builder, scope, life, shared_from_this(),
					bound_type_t::refs_from_vars(arguments));

			if (!!status) {
				debug_above(5, log(log_info, "function chosen is %s", function->str().c_str()));

				return make_call_value(status, builder, get_location(), scope,
						life, function, arguments);
			}
		} else {
			user_error(status, function_expr->get_location(),
					"%s being called like a function. arguments are %s",
					function_expr->str().c_str(),
					::str(arguments).c_str());
		}
	}

	assert(!status);
	return nullptr;
}

void ast::callsite_expr_t::resolve_statement(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		local_scope_t::ref *new_scope,
		bool *returns) const
{
	auto return_value = resolve_expression(status, builder, scope, life);
	if (!!status) {
		if (return_value->get_signature() != "void") {
			user_error(status, token.location, "call statement must have void return value");
		} else {
			return;
		}
	}

	assert(!status);
	return;
}

bound_var_t::ref ast::reference_expr_t::resolve_expression(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life) const
{
	// TODO: handle path depth > 1
	assert(path.size() == 1);
	auto name = path[0]->get_name();
	/* we wouldn't be referencing a variable name here unless it was unique
	 * override resolution only happens on callsites, and we don't allow
	 * passing around unresolved overload references */
	bound_var_t::ref var = scope->get_bound_variable(status, get_location(),
			name);

	if (!!status) {
		return var;
	} else {
		user_error(status, get_location(), "undefined symbol " c_id("%s"), name.c_str());
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::array_index_expr_t::resolve_expression(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life) const
{
	/* this expression looks like this
	 *
	 *   lhs[index]
	 *
	 */

	if (!!status) {
		bound_var_t::ref lhs_val = lhs->resolve_expression(status, builder,
				scope, life);

		if (!!status) {
			bound_var_t::ref index_val = index->resolve_expression(status, builder,
					scope, life);

			if (!!status) {
				/* get or instantiate a function we can call on these arguments */
				return call_program_function(status, builder, scope, life,
						"__getitem__", shared_from_this(), {lhs_val, index_val});
			}
		}
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::array_literal_expr_t::resolve_expression(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life) const
{
	user_error(status, get_location(), "not impl");
	return nullptr;
}

llvm::Value *get_raw_condition_value(
		status_t &status,
	   	llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		ast::item_t::ref condition,
		bound_var_t::ref condition_value)
{
	if (condition_value->is_int()) {
		return condition_value->resolve_value(builder);
	} else if (condition_value->is_pointer()) {
		return condition_value->resolve_value(builder);
	} else {
		user_error(status, condition->get_location(), "unknown basic type: %s",
				condition_value->str().c_str());
	}

	assert(!status);
	return nullptr;
}

llvm::Value *maybe_get_bool_overload_value(
		status_t &status,
	   	llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		ast::item_t::ref condition,
		bound_var_t::ref condition_value)
{
	assert(life->life_form == lf_statement);

	llvm::Value *llvm_condition_value = nullptr;
	// TODO: check whether we are checking a raw value or not

	debug_above(2, log(log_info,
				"attempting to resolve a " c_var("%s") " override if condition %s, ",
				BOOL_TYPE,
				condition->str().c_str()));

	/* we only ever get in here if we are definitely non-null, so we can discard
	 * maybe type specifiers */
	types::type_t::ref condition_type;
	if (auto maybe = dyncast<const types::type_maybe_t>(condition_value->type->get_type())) {
		condition_type = maybe->just;
	} else {
		condition_type = condition_value->type->get_type();
	}

	var_t::refs fns;
	auto bool_fn = maybe_get_callable(status, builder, scope, BOOL_TYPE,
			condition->get_location(),
			type_args({condition_type}), fns);

	if (!!status) {
		if (bool_fn != nullptr) {
			/* we've found a bool function that will take our condition as input */
			assert(bool_fn != nullptr);

			if (get_function_return_type(bool_fn->type->get_type())->get_signature() == "__bool__") {
				debug_above(7, log(log_info, "generating a call to " c_var("bool") "(%s) for if condition evaluation (type %s)",
							condition->str().c_str(), bool_fn->type->str().c_str()));

				/* let's call this bool function */
				llvm_condition_value = llvm_create_call_inst(
						status, builder, condition->get_location(), bool_fn,
						{condition_value->resolve_value(builder)});

				if (!!status) {
					/* NB: no need to track this value in a life because it's
					 * returning an integer type */
					assert(llvm_condition_value->getType()->isIntegerTy());
					return llvm_condition_value;
				}
			} else {
				user_error(status, bool_fn->get_location(),
						"__bool__ coercion function must return a " C_TYPE "__bool__" C_RESET);
				user_error(status, bool_fn->get_location(),
						"implicit __bool__ was defined function must return a " C_TYPE "__type__" C_RESET);
			}
		} else {
			/* treat all values without overloaded bool functions as truthy */
			return nullptr;
		}
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::ternary_expr_t::resolve_expression(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life) const
{
	/* if scope allows us to set up new variables inside if conditions */
	local_scope_t::ref if_scope;

	bound_type_t::ref type_constraint;

	assert(condition != nullptr);


	/* evaluate the condition for branching */
	bound_var_t::ref condition_value = condition->resolve_expression(
			status, builder, scope, life);

	if (!!status) {
		/* if the condition value is a maybe type, then we'll need multiple
		 * anded conditions to be true in order to actually fall into the then
		 * block, let's figure out those conditions */
		llvm::Value *llvm_raw_condition_value = get_raw_condition_value(status,
				builder, scope, condition, condition_value);

		if (!!status) {
			assert(llvm_raw_condition_value != nullptr);

			llvm::Function *llvm_function_current = llvm_get_function(builder);

			/* generate some new blocks */
			llvm::BasicBlock *then_bb = llvm::BasicBlock::Create(
					builder.getContext(), "ternary.truthy", llvm_function_current);

			/* we've got an else block, so let's create an "else" basic block. */
			llvm::BasicBlock *else_bb = llvm::BasicBlock::Create(
					builder.getContext(), "ternary.falsey", llvm_function_current);

			/* put the merge block after the else block */
			llvm::BasicBlock *merge_bb = llvm::BasicBlock::Create(
					builder.getContext(), "ternary.phi", llvm_function_current);

			/* create the actual branch instruction */
			llvm_create_if_branch(status, builder, scope, 0,
					nullptr, llvm_raw_condition_value, then_bb, else_bb);

			if (!!status) {
				/* calculate the false path's value in the else block */
				builder.SetInsertPoint(else_bb);
				bound_var_t::ref false_path_value = when_false->resolve_expression(
						status, builder, scope, life);

				/* after calculation, the code should jump to the phi node's basic block */
				builder.CreateBr(merge_bb);

				if (!!status) {
					/* let's generate code for the "true-path" block */
					builder.SetInsertPoint(then_bb);
					llvm::Value *llvm_bool_overload_value = maybe_get_bool_overload_value(status,
							builder, scope, life, condition, condition_value);

					if (!!status) {
						llvm::BasicBlock *truth_path_bb = then_bb;

						if (llvm_bool_overload_value != nullptr) {
							/* we've got a second condition to check, let's do it */
							auto deep_then_bb = llvm::BasicBlock::Create(
									builder.getContext(), "ternary.truthy.__bool__", llvm_function_current);

							llvm_create_if_branch(status, builder, scope, 0,
									nullptr, llvm_bool_overload_value,
									deep_then_bb, else_bb ? else_bb : merge_bb);
							builder.SetInsertPoint(deep_then_bb);

							/* make sure the phi node knows to consider this deeper
							 * block as the source of one of its possible inbound
							 * values */
							truth_path_bb = deep_then_bb;
						}

						if (!!status) {
							/* get the bound_var for the truthy path */
							bound_var_t::ref true_path_value = when_true->resolve_expression(
									status, builder, if_scope ? if_scope : scope, life);

							if (!!status) {
								bound_type_t::ref ternary_type;
								if (true_path_value->type->get_llvm_specific_type() != false_path_value->type->get_llvm_specific_type()) {
									/* the when_true and when_false values have different
									 * types, let's create a sum type to represent this */
									auto ternary_sum_type = type_sum_safe(status, {
											true_path_value->type->get_type(),
											false_path_value->type->get_type()});

									if (!!status) {
										ternary_type = upsert_bound_type(status,
												builder, scope, ternary_sum_type);
									}
								} else {
									ternary_type = true_path_value->type;
								}

								if (!!status) {
									builder.CreateBr(merge_bb);
									builder.SetInsertPoint(merge_bb);

									llvm::PHINode *llvm_phi_node = llvm::PHINode::Create(
											ternary_type->get_llvm_specific_type(),
											2, "ternary.phi.node", merge_bb);

									llvm_phi_node->addIncoming(llvm_maybe_pointer_cast(
												builder, true_path_value->resolve_value(builder), ternary_type),
											truth_path_bb);

									llvm_phi_node->addIncoming(llvm_maybe_pointer_cast(
												builder, false_path_value->resolve_value(builder), ternary_type),
											else_bb);

									return bound_var_t::create(
											INTERNAL_LOC(),
											{"ternary.value"},
											ternary_type,
											llvm_phi_node,
											make_code_id(this->token),
											false /* is_lhs */, false /*is_global*/);
								}
							}
						}
					}
				}
			}
		}
	}

	assert(!status);
    return nullptr;
}

types::type_t::ref eval_to_struct_ref(
		status_t &status,
		scope_t::ref scope,
		ast::item_t::ref node,
		types::type_t::ref type)
{
	if (dyncast<const types::type_managed_ptr_t>(type)) {
		return type;
	} else if (dyncast<const types::type_native_ptr_t>(type)) {
		return type;
	} else if (dyncast<const types::type_maybe_t>(type)) {
		user_error(status, node->get_location(),
				"maybe type %s cannot be dereferenced. todo implement ?.",
				type->str().c_str());
		return nullptr;
	}

	types::type_t::ref expansion = eval(type, scope->get_typename_env());

	if (expansion != nullptr) {
		debug_above(5, log(log_info,
					"expanded %s to %s",
					type->str().c_str(),
					expansion->str().c_str()));
		return expansion;
	} else {
		user_error(status, node->get_location(),
				"type %s could not be expanded",
				type->str().c_str());
	}

	assert(!status);
	return nullptr;
}

types::type_struct_t::ref get_struct_type_from_managed_ptr(
		status_t &status,
		ast::item_t::ref node,
		types::type_t::ref type)
{
	if (auto type_managed_ptr = dyncast<const types::type_managed_ptr_t>(type)) {
		if (auto struct_type = dyncast<const types::type_struct_t>(type_managed_ptr->element_type)) {
			return struct_type;
		} else {
			panic("no struct type found in ref");
		}
	} else {
		user_error(status, node->get_location(),
				"unable to dereference %s, it's not a ref",
				node->str().c_str());
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref extract_member_variable(
		status_t &status, 
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		ast::item_t::ref node,
		bound_var_t::ref bound_var,
		atom member_name)
{
	bound_var = maybe_load_from_pointer(status, builder, scope, bound_var);

	if (!!status) {
		types::type_t::ref object_type = bound_var->type->get_type();

		types::type_t::ref type = eval_to_struct_ref(status, scope,
				node, object_type);

		if (!status) {
			return nullptr;
		}

		auto bound_struct_ref = upsert_bound_type(status, builder, scope, type);

		types::type_struct_t::ref struct_type = get_struct_type_from_managed_ptr(
				status, node, type);

		if (!status) {
			return nullptr;
		}

		auto member_index = struct_type->name_index;
		auto member_index_iter = member_index.find(member_name);

		for (auto member_index_pair : member_index) {
			debug_above(5, log(log_info, "%s: %d", member_index_pair.first.c_str(),
						member_index_pair.second));
		}

		if (member_index_iter != member_index.end()) {
			auto index = member_index_iter->second;
			debug_above(5, log(log_info, "found member " c_id("%s") " of type %s at index %d",
						member_name.c_str(),
						bound_struct_ref->str().c_str(), index));

			/* get the type of the dimension being referenced */
			bound_type_t::ref member_type = scope->get_bound_type(
					struct_type->dimensions[index]->get_signature());
			assert(bound_struct_ref->get_llvm_type() != nullptr);

			debug_above(5, log(log_info, "looking at bound_var %s : %s",
						bound_var->str().c_str(),
						llvm_print(bound_var->type->get_llvm_type()).c_str()));

			/* get an GEP-able version of the object */
			llvm::Value *llvm_var_value = bound_var->resolve_value(builder);

			llvm_var_value = llvm_maybe_pointer_cast(builder, llvm_var_value,
					bound_struct_ref->get_llvm_specific_type());

			/* the following code is heavily coupled to the physical layout of
			 * managed vs. native structures */

			/* GEP and load the member value from the structure */
			llvm::Value *llvm_gep = llvm_make_gep(builder,
					llvm_var_value, index, true /* managed */);
			if (llvm_gep->getName().str().size() == 0) {
				llvm_gep->setName(string_format("address_of.%s", member_name.c_str()));
			}

			llvm::Value *llvm_item = builder.CreateLoad(llvm_gep);

			/* add a helpful descriptive name to this local value */
			auto value_name = string_format(".%s", member_name.c_str());
			llvm_item->setName(value_name);

			return bound_var_t::create(
					INTERNAL_LOC(), value_name,
					member_type, llvm_item, make_iid(member_name), false /*is_lhs*/,
				   	false /*is_global*/);
		} else {
			auto full_type = bound_var->type->get_type()->rebind({});
			user_error(status, node->get_location(),
					"%s has no dimension called " c_id("%s"),
					full_type->str().c_str(),
					member_name.c_str());
			user_message(log_info, status, bound_var->type->get_location(), "%s has dimension(s) [%s]",
					full_type->str().c_str(),
					join_with(member_index, ", ", [] (std::pair<atom, int> index) -> std::string {
						return std::string(C_ID) + index.first.str() + C_RESET;
						}).c_str());
		}
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref call_typeid(
		status_t &status,
		scope_t::ref scope,
		life_t::ref life,
		ast::item_t::ref callsite,
		identifier::ref id,
	   	llvm::IRBuilder<> &builder,
		bound_var_t::ref resolved_value)
{
	debug_above(4, log(log_info, "getting typeid of %s",
				resolved_value->type->str().c_str()));
	auto program_scope = scope->get_program_scope();

	auto llvm_type = resolved_value->type->get_llvm_specific_type();
	auto llvm_obj_type = program_scope->get_bound_type({"__var"})->get_llvm_type();
	bool is_obj = false;

	if (llvm_type->isPointerTy()) {
		if (auto llvm_struct = llvm::dyn_cast<llvm::StructType>(llvm_type->getPointerElementType())) {
			is_obj = (
					llvm_struct == llvm_obj_type ||
				   	llvm_struct->getStructElementType(0) == llvm_obj_type);
		}
	}

	auto name = string_format("typeid(%s)", resolved_value->str().c_str());

	if (is_obj) {
		auto get_typeid_function = program_scope->get_bound_variable(status,
				callsite->get_location(), "__get_var_type_id");
		if (!!status) {
			return create_callsite(
					status,
					builder,
					scope,
					life,
					get_typeid_function,
					name,
					id->get_location(),
					{resolved_value});
		}
	} else {
		auto iatom = atom{resolved_value->type->get_type()->get_signature()}.iatom;
		return bound_var_t::create(
				INTERNAL_LOC(),
				string_format("typeid(%s)", resolved_value->str().c_str()),
				program_scope->get_bound_type({TYPEID_TYPE}),
				llvm_create_int32(builder, iatom),
				id,
				false/*is_lhs*/, false /*is_global*/);
	}

	assert(!status);
	return nullptr;
}


bound_var_t::ref ast::typeid_expr_t::resolve_expression(
		status_t &status,
	   	llvm::IRBuilder<> &builder,
	   	scope_t::ref scope,
		life_t::ref life) const
{
	auto resolved_value = expr->resolve_expression(status, builder, scope, life);

	if (!!status) {
		return call_typeid(status, scope, life, shared_from_this(),
				make_code_id(token), builder, resolved_value);
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::sizeof_expr_t::resolve_expression(
		status_t &status,
	   	llvm::IRBuilder<> &builder,
	   	scope_t::ref scope,
		life_t::ref life) const
{
	/* calculate the size of the object being referenced assume native types */
	bound_type_t::ref bound_type = upsert_bound_type(status, builder, scope, type);
	bound_type_t::ref size_type = scope->get_program_scope()->get_bound_type({INT_TYPE});
	if (!!status) {
		llvm::Value *llvm_size = llvm_sizeof_type(builder,
				llvm_deref_type(bound_type->get_llvm_specific_type()));

		return bound_var_t::create(
				INTERNAL_LOC(), type->str(), size_type, llvm_size,
				make_iid("sizeof"), false /*is_lhs*/, false /*is_global*/);
	}

	assert(!status);
	return nullptr;
}

void ast::function_defn_t::resolve_function_defn(
        status_t &status,
        llvm::IRBuilder<> &builder,
        module_scope_t::ref scope) const
{
	llvm::IRBuilderBase::InsertPointGuard ipg(builder);

	/* lifetimes have extents at function boundaries */
	auto life = make_ptr<life_t>(status, lf_function);

	assert(!!status);

	/* function definitions are type checked at instantiation points. callsites
	 * are instantiation points.
	 *
	 * The main job of this function is to:
	 * 0. type check the function given the scope.
	 * 1. generate code for this function.
	 * 2. bind the function name to the generated code within the given scope.
	 * */
	indent_logger indent(2, string_format(
				"type checking %s in %s", token.str().c_str(),
				scope->get_name().c_str()));

	/* see if we can get a monotype from the function declaration */
	bound_type_t::named_pairs args;
	bound_type_t::ref return_type;
	type_check_fully_bound_function_decl(status, builder, *decl, scope, args, return_type);

	if (!!status) {
		instantiate_with_args_and_return_type(status, builder, scope, life,
				nullptr, args, return_type);
		return;
	} else {
		user_error(status, get_location(), "unable to declare function %s due to related errors",
				token.str().c_str());
	}

	assert(!status);
	return;
}

bound_var_t::ref ast::function_defn_t::instantiate_with_args_and_return_type(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
		local_scope_t::ref *new_scope,
		bound_type_t::named_pairs args,
		bound_type_t::ref return_type) const
{
	llvm::IRBuilderBase::InsertPointGuard ipg(builder);
	assert(!!status);
	assert(life->life_form == lf_function);
	assert(life->values.size() == 0);

	std::string function_name = token.text;

	assert(scope->get_llvm_module() != nullptr);

	auto function_type = get_function_type(args, return_type);
	bound_type_t::ref bound_function_type = upsert_bound_type(status,
			builder, scope, function_type);

	if (!!status) {
		assert(bound_function_type->get_llvm_type() != nullptr);

		llvm::Type *llvm_type = bound_function_type->get_llvm_specific_type();
		if (llvm_type->isPointerTy()) {
			llvm_type = llvm_type->getPointerElementType();
		}
		debug_above(5, log(log_info, "creating function %s with LLVM type %s",
				function_name.c_str(),
				llvm_print(llvm_type).c_str()));
		assert(llvm_type->isFunctionTy());

		llvm::Function *llvm_function = llvm::Function::Create(
				(llvm::FunctionType *)llvm_type,
				llvm::Function::ExternalLinkage, function_name,
				scope->get_llvm_module());

		llvm::BasicBlock *llvm_block = llvm::BasicBlock::Create(
				builder.getContext(), "entry", llvm_function);
		builder.SetInsertPoint(llvm_block);

		/* set up the mapping to this function for use in recursion */
		bound_var_t::ref function_var = bound_var_t::create(
				INTERNAL_LOC(), token.text, bound_function_type, llvm_function,
				make_code_id(token), false/*is_lhs*/, false /*is_global*/);

		/* we should be able to check its block as a callsite. note that this
		 * code will also run for generics but only after the
		 * sbk_generic_substitution mechanism has run its course. */
		auto params_scope = make_param_list_scope(status, builder, *decl, scope,
				life, function_var, args);

		if (!!status) {
			/* now put this function declaration into the containing scope in case
			 * of recursion */
			if (function_var->name.size() != 0) {
				module_scope_t::ref module_scope = dyncast<module_scope_t>(scope);
				assert(module_scope != nullptr);

				/* before recursing directly or indirectly, let's just add
				 * this function to the module scope we're in */
				scope->get_program_scope()->put_bound_variable(status, function_var->name, function_var);
				if (!!status) {
					module_scope->mark_checked(builder, shared_from_this());
				}
			} else {
				user_error(status, get_location(), "function definitions need names");
			}
		}

		if (!!status) {
			/* keep track of whether this function returns */
			bool all_paths_return = false;
			params_scope->return_type_constraint = return_type;

			block->resolve_statement(status, builder, params_scope, life,
					nullptr, &all_paths_return);

			if (!!status) {
				debug_above(10, log(log_info, "module dump from %s\n%s",
							__PRETTY_FUNCTION__,
							llvm_print_module(*llvm_get_module(builder)).c_str()));

				if (all_paths_return) {
					return function_var;
				} else {
					/* not all control paths return */
					if (return_type->is_void()) {
						/* if this is a void let's give the user a break and insert
						 * a default void return */

						life->release_vars(status, builder, scope, lf_function);
						builder.CreateRetVoid();
						return function_var;
					} else {
						/* no breaks here, we don't know what to return */
						user_error(status, get_location(), "not all control paths return a value");
					}
				}

				llvm_verify_function(status, llvm_function);
			}
		}
	}

    assert(!status);
    return nullptr;
}

void type_check_module_links(
		status_t &status,
		compiler_t &compiler,
		llvm::IRBuilder<> &builder,
		const ast::module_t &obj,
		scope_t::ref program_scope)
{
	if (!!status) {
		indent_logger indent(3, string_format("resolving links in " c_module("%s"),
					obj.module_key.c_str()));

		/* get module level scope variable */
		module_scope_t::ref scope = compiler.get_module_scope(obj.module_key);

		for (auto &link : obj.linked_modules) {
			link->resolve_linked_module(status, builder, scope);
		}

		if (!!status) {
			for (auto &link : obj.linked_functions) {
				bound_var_t::ref link_value = link->resolve_linked_function(
						status, builder, scope);

				if (!!status) {
					if (link->function_name.text.size() != 0) {
						scope->put_bound_variable(status, link->function_name.text, link_value);
					} else {
						user_error(status, link->get_location(), "module level link definitions need names");
					}
				}
			}
		}
	}
}

#if 0
void type_check_module_vars(
		status_t &status,
        compiler_t &compiler,
		llvm::IRBuilder<> &builder,
		const ast::module_t &obj,
		scope_t::ref program_scope)
{
	if (!!status) {
		indent_logger indent(3, string_format("resolving module vars in " c_module("%s"),
					obj.module_key.c_str()));

		/* get module level scope variable */
		module_scope_t::ref module_scope = compiler.get_module_scope(obj.module_key);
		auto scope = module_scope->new_function_scope("__init_module_vars");
		assert(llvm_get_function(builder) != nullptr);

		for (auto &var_decl : obj.var_decls) {
			debug_above(6, log("instantiating module var " c_id("%s"),
						module_scope->make_fqn(var_decl->token.text).c_str()));
			/* track memory consumed during global construction */
			auto life = make_ptr<life_t>(status, lf_statement);

			/* the idea here is to put this variable into module scope,
			 * available globally, but to initialize it in the
			 * __init_module_vars function */
			type_check_module_var_decl(status, builder, scope, *var_decl, life);

			/* clean up any memory consumed during global construction */
			life->release_vars(status, builder, scope, lf_statement);
		}
	}
}
#endif

void type_check_module_types(
		status_t &status,
        compiler_t &compiler,
        llvm::IRBuilder<> &builder,
        const ast::module_t &obj,
        scope_t::ref program_scope)
{
	if (!!status) {
		indent_logger indent(2, string_format("type-checking types in module " c_module("%s"),
					obj.module_key.str().c_str()));

		/* get module level scope types */
		module_scope_t::ref module_scope = compiler.get_module_scope(obj.module_key);

		auto unchecked_types_ordered = module_scope->get_unchecked_types_ordered();
		for (auto unchecked_type : unchecked_types_ordered) {
			auto node = unchecked_type->node;
			if (!module_scope->has_checked(node)) {
				assert(!dyncast<const ast::function_defn_t>(node));

				/* prevent recurring checks */
				debug_above(5, log(log_info, "checking module level type %s", node->get_token().str().c_str()));

				/* these next lines create type definitions, regardless of
				 * their genericity.  type expressions will be added as
				 * environment variables in the type system.  this step is
				 * MUTATING the type environment of the module, and the
				 * program. */
				if (auto user_defined_type = dyncast<const ast::user_defined_type_t>(node)) {
					status_t local_status;
					user_defined_type->register_type(local_status, builder, module_scope);

					/* take note of whether this failed or not */
					status |= local_status;
				} else {
					assert(!"unhandled unchecked type node at module scope");
				}
			} else {
				debug_above(3, log(log_info, "skipping %s because it's already been checked",
						   	node->get_token().str().c_str()));
			}
		}
	}
}

void type_check_program_variables(
		status_t &status,
        compiler_t &compiler,
        llvm::IRBuilder<> &builder,
        program_scope_t::ref program_scope)
{
	indent_logger indent(2, string_format("resolving variables in program"));

	auto unchecked_vars_ordered = program_scope->get_unchecked_vars_ordered();
    for (auto unchecked_var : unchecked_vars_ordered) {
		debug_above(5, log(log_info, "checking whether to check %s",
					unchecked_var->str().c_str()));

		auto node = unchecked_var->node;
		if (!unchecked_var->module_scope->has_checked(node)) {
			/* prevent recurring checks */
			debug_above(4, log(log_info, "checking module level variable %s",
					   	node->get_token().str().c_str()));

			status_t local_status;
			if (auto fn = dyncast<const ast::function_defn_t>(node)) {
				fn->resolve_function_defn(local_status, builder, unchecked_var->module_scope);
				status |= local_status;
			} else if (auto stmt = dyncast<const ast::statement_t>(node)) {
				assert(false && "We should not have statements at this level.");
			} else if (auto data_ctor = dyncast<const ast::struct_t>(node)) {
				/* ignore until instantiation at a callsite */
			} else {
				assert(!"unhandled unchecked node at module scope");
			}
		} else {
			debug_above(3, log(log_info, "skipping %s because it's already been checked",
					   	node->get_token().str().c_str()));
		}
    }
}

#if 0
void type_check_all_module_var_slots(
		status_t &status,
		compiler_t &compiler,
		llvm::IRBuilder<> &builder,
		const ast::program_t &obj,
		program_scope_t::ref program_scope)
{
	/* build the global __init_module_vars function */
	if (!!status) {
		llvm::IRBuilderBase::InsertPointGuard ipg(builder);
		bound_var_t::ref init_module_vars = llvm_start_function(status,
				builder, 
				program_scope,
				static_cast<const ast::item_t&>(obj).shared_from_this(),
				{},
				program_scope->get_bound_type({"void"}),
				"__init_module_vars");

		if (!!status) {
			program_scope->put_bound_variable(status, "__init_module_vars", init_module_vars);

			if (!!status) {
				for (auto &module : obj.modules) {
					type_check_module_vars(status, compiler, builder, *module, program_scope);
				}

				builder.CreateRetVoid();
			}
		}
	}
}
#endif

void type_check_program(
		status_t &status,
		llvm::IRBuilder<> &builder,
		const ast::program_t &obj,
		compiler_t &compiler)
{
	indent_logger indent(2, string_format(
				"type-checking program %s",
				compiler.get_program_name().c_str()));

	ptr<program_scope_t> program_scope = compiler.get_program_scope();
	debug_above(11, log(log_info, "type_check_program program scope:\n%s", program_scope->str().c_str()));

	/* pass to resolve all module-level types */
	for (auto &module : obj.modules) {
		type_check_module_types(status, compiler, builder, *module, program_scope);
		if (!status) {
			break;
		}
	}

	if (!!status) {
		/* pass to resolve all module-level links */
		for (auto &module : obj.modules) {
			type_check_module_links(status, compiler, builder, *module, program_scope);
		}

		if (!!status) {
			assert(compiler.main_module != nullptr);

			/* pass to resolve all main module-level variables.  technically we only
			 * need to check the primary module, since that is the one that is expected
			 * to have the entry point ... at least for now... */
			type_check_program_variables(status, compiler, builder, program_scope);
		}
	}
}

void type_check_assignment(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		bound_var_t::ref lhs_var,
		bound_var_t::ref rhs_var,
		location_t location)
{
	if (!!status) {
		indent_logger indent(5, string_format(
					"type checking assignment %s = %s",
					lhs_var->str().c_str(),
					rhs_var->str().c_str()));

		unification_t unification = unify(
				lhs_var->type->get_type(),
				rhs_var->type->get_type(), scope->get_typename_env());

		if (unification.result) {
			/* ensure that whatever was being pointed to by this LHS
			 * is released after this statement */
			auto prior_lhs_value = lhs_var->resolve_bound_value(builder);

			if (lhs_var->is_global()) {
				/* handle assignment to globals */
				builder.CreateStore(
						llvm_maybe_pointer_cast(builder,
							rhs_var->resolve_value(builder),
							lhs_var->type->get_llvm_specific_type()),
						lhs_var->get_llvm_value());
			} else if (lhs_var->is_lhs()) {
				llvm::AllocaInst *llvm_alloca = llvm::dyn_cast<llvm::AllocaInst>(
						lhs_var->get_llvm_value());
				assert(llvm_alloca != nullptr);

				builder.CreateStore(
						llvm_maybe_pointer_cast(builder,
							rhs_var->resolve_value(builder),
							lhs_var->type->get_llvm_specific_type()),
						llvm_alloca);
			} else {
				user_error(status, location, "left-hand side of assignment is immutable");
			}

			if (!!status) {
				if (rhs_var->type->is_managed()) {
					/* only bother addref/release if these are different
					 * things */
					if (rhs_var->get_llvm_value() != prior_lhs_value->get_llvm_value()) {
						call_addref_var(status, builder, scope, rhs_var, "addref after assignment");
						call_release_var(status, builder, scope, prior_lhs_value, "release after assignment");
					}
				}

				return;
			}
		} else {
			user_error(status, location, "left-hand side is incompatible with the right-hand side (%s)",
					unification.str().c_str());
		}
	}

	assert(!status);
	return;
}

void ast::assignment_t::resolve_statement(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
	assert(token.text == K(set));

	auto lhs_var = lhs->resolve_expression(status, builder, scope, life);
	if (!!status) {
		auto rhs_var = rhs->resolve_expression(status, builder, scope, life);
		type_check_assignment(status, builder, scope, life, lhs_var, rhs_var, token.location);
		if (!!status) {
			return;
		}
	}

	assert(!status);
	return;
}

void ast::break_flow_t::resolve_statement(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
	if (auto runnable_scope = dyncast<runnable_scope_t>(scope)) {
		llvm::BasicBlock *break_bb = runnable_scope->get_innermost_loop_break();
		if (break_bb != nullptr) {
			assert(!builder.GetInsertBlock()->getTerminator());

			/* release everything held back to the loop we're in */
			life->release_vars(status, builder, scope, lf_loop);

			builder.CreateBr(break_bb);
			return;
		} else {
			user_error(status, get_location(), c_control("break") " outside of a loop");
		}
	} else {
		panic("we should not be looking at a break statement here!");
	}
	assert(!status);
	return;
}

void ast::continue_flow_t::resolve_statement(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
	if (auto runnable_scope = dyncast<runnable_scope_t>(scope)) {
		llvm::BasicBlock *continue_bb = runnable_scope->get_innermost_loop_continue();
		if (continue_bb != nullptr) {
			assert(!builder.GetInsertBlock()->getTerminator());

			/* release everything held back to the loop we're in */
			life->release_vars(status, builder, scope, lf_loop);

			builder.CreateBr(continue_bb);
			return;
		} else {
			user_error(status, get_location(), c_control("continue") " outside of a loop");
		}
	} else {
		panic("we should not be looking at a continue statement here!");
	}
	assert(!status);
	return;
}

void ast::return_statement_t::resolve_statement(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
	life = life->new_life(status, lf_statement);

	/* obviously... */
	*returns = true;

	/* let's figure out if we have a return value, and what its type is */
    bound_var_t::ref return_value;
    bound_type_t::ref return_type;

    if (expr != nullptr) {
        /* if there is a return expression resolve it into a value */
		return_value = expr->resolve_expression(status, builder, scope, life);

		/* addref this return value on behalf of the caller */
		call_addref_var(status, builder, scope, return_value, "return statement");

        if (!!status) {
            /* get the type suggested by this return value */
            return_type = return_value->type;
        }
    } else {
        /* we have an empty return, let's just use void */
        return_type = scope->get_program_scope()->get_bound_type({"void"});
    }

    if (!!status) {
		/* release all variables from all lives */
		life->release_vars(status, builder, scope, lf_function);

        runnable_scope_t::ref runnable_scope = dyncast<runnable_scope_t>(scope);
        assert(runnable_scope != nullptr);

		/* make sure this return type makes sense, or keep track of it if we
		 * didn't yet know the return type for this function */
		runnable_scope->check_or_update_return_type_constraint(status,
				shared_from_this(), return_type);

		if (return_value != nullptr) {
			auto llvm_return_value = llvm_maybe_pointer_cast(builder,
					return_value->resolve_value(builder),
					runnable_scope->get_return_type_constraint());

			if (llvm_return_value->getName().str().size() == 0) {
				llvm_return_value->setName("return.value");
			}
			debug_above(8, log("emitting a return of %s", llvm_print(llvm_return_value).c_str()));

			builder.CreateRet(llvm_return_value);
		} else {
			assert(types::is_type_id(return_type->get_type(), "void"));

			// TODO: release live variables in scope
			builder.CreateRetVoid();
		}

        return;
    }
    assert(!status);
    return;
}

void ast::block_t::resolve_statement(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
        local_scope_t::ref *new_scope,
		bool *returns_) const
{
	/* it's important that we keep track of returns */
	bool placeholder_returns = false;
	bool *returns = returns_;
	if (returns == nullptr) {
		returns = &placeholder_returns;
	}

    scope_t::ref current_scope = scope;

	assert(builder.GetInsertBlock() != nullptr);

	/* create a new life for tracking value lifetimes across this block */
	life = life->new_life(status, lf_block);

	for (auto &statement : statements) {
		if (*returns) {
			user_error(status, statement->get_location(), "this statement will never run");
			break;
		}

		local_scope_t::ref next_scope;

		debug_above(9, log(log_info, "type checking statement\n%s", statement->str().c_str()));

		/* create a new life for tracking the rhs values (temp values) in this statement */
		auto stmt_life = life->new_life(status, lf_statement);

		/* resolve the statement */
		statement->resolve_statement(status, builder, current_scope, stmt_life,
				&next_scope, returns);

		if (!*returns) {
			/* inject release operations for rhs values out of extent */
			stmt_life->release_vars(status, builder, scope, lf_statement);
		}

		if (!!status) {
			if (next_scope != nullptr) {
				/* the statement just executed wants to create a new nested scope.
				 * let's allow this by just keeping track of the current scope. */
				current_scope = next_scope;
				next_scope = nullptr;
				debug_above(10, log(log_info, "got a new scope %s", current_scope->str().c_str()));
			}
		}

		if (!status) {
			if (!status.reported_on_error_at(statement->get_location())) {
				user_error(status, statement->get_location(), "while checking %s",
						statement->str().c_str());
			}
			break;
		}
    }

	if (!*returns) {
		life->release_vars(status, builder, scope, lf_block);
	}

    /* blocks don't really have values */
    return;
}

void ast::loop_block_t::resolve_statement(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		local_scope_t::ref *new_scope,
		bool *returns) const
{
#if 0
	/* while scope allows us to set up new variables inside while conditions */
	local_scope_t::ref while_scope;

	if (condition != nullptr) {
		assert(token.text == "while");

		llvm::Function *llvm_function_current = llvm_get_function(builder);

		llvm::BasicBlock *while_cond_bb = llvm::BasicBlock::Create(builder.getContext(), "while.cond", llvm_function_current);

		assert(!builder.GetInsertBlock()->getTerminator());
		builder.CreateBr(while_cond_bb);
		builder.SetInsertPoint(while_cond_bb);

		/* demarcate a loop boundary here */
		life = life->new_life(status, lf_loop);

		auto cond_life = life->new_life(status, lf_statement);

		/* evaluate the condition for branching */
		bound_var_t::ref condition_value = condition->resolve_instantiation(
				status, builder, scope, cond_life, &while_scope, nullptr);

		if (!!status) {
			debug_above(5, log(log_info,
						"getting raw condition for value %s",
						condition_value->str().c_str()));
			llvm::Value *llvm_raw_condition_value = get_raw_condition_value(status,
					builder, scope, condition, condition_value);

			if (!!status) {
				assert(llvm_raw_condition_value != nullptr);

				/* generate some new blocks */
				llvm::BasicBlock *while_block_bb = llvm::BasicBlock::Create(builder.getContext(), "while.block", llvm_function_current);
				llvm::BasicBlock *while_end_bb = llvm::BasicBlock::Create(builder.getContext(), "while.end");

				/* keep track of the "break" and "continue" jump locations */
				loop_tracker_t loop_tracker(dyncast<runnable_scope_t>(scope), while_cond_bb, while_end_bb);

				/* we don't have an else block, so we can just continue on */
				llvm_create_if_branch(status, builder, scope,
						IFF_ELSE, cond_life, llvm_raw_condition_value,
						while_block_bb, while_end_bb);

				if (!!status) {
					assert(builder.GetInsertBlock()->getTerminator());

					/* let's generate code for the "then" block */
					builder.SetInsertPoint(while_block_bb);
					assert(!builder.GetInsertBlock()->getTerminator());

					llvm::Value *llvm_bool_overload_value = maybe_get_bool_overload_value(status,
							builder, scope, cond_life, condition, condition_value);

					if (!!status) {
						if (llvm_bool_overload_value != nullptr) {
							/* we've got a second condition to check, let's do it */
							auto deep_while_bb = llvm::BasicBlock::Create(builder.getContext(),
									"deep-while", llvm_function_current);

							llvm_create_if_branch(status, builder, scope,
									IFF_BOTH, cond_life,
									llvm_bool_overload_value, deep_while_bb,
									while_end_bb);
							builder.SetInsertPoint(deep_while_bb);
						} else {
							cond_life->release_vars(status, builder, scope, lf_statement);
						}

						if (!!status) {
							block->resolve_instantiation(status, builder,
									while_scope ? while_scope : scope, life, nullptr,
									nullptr);

							/* the loop can't store values */
							assert(life->values.size() == 0 && life->life_form == lf_loop);

							if (!!status) {
								if (!builder.GetInsertBlock()->getTerminator()) {
									builder.CreateBr(while_cond_bb);
								}
								builder.SetInsertPoint(while_end_bb);

								/* we know we'll need to fall through to the merge
								 * block, let's add it to the end of the function
								 * and let's set it as the next insert point. */
								llvm_function_current->getBasicBlockList().push_back(while_end_bb);
								builder.SetInsertPoint(while_end_bb);

								assert(!!status);
								return nullptr;
							}
						}
					}
				}
			}
		}
	} else {
		/* this should never happen */
		not_impl();
	}
#endif

    assert(!status);
    return;
}

void ast::if_block_t::resolve_statement(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
	assert(life->life_form == lf_statement);

	/* if scope allows us to set up new variables inside if conditions */
	local_scope_t::ref if_scope;

	bool if_block_returns = false, else_block_returns = false;

	assert(condition != nullptr);

	assert(token.text == "if" || token.text == "elif");
	bound_var_t::ref condition_value;

	auto cond_life = life->new_life(status, lf_statement);

	/* evaluate the condition for branching */
	condition_value = condition->resolve_expression(
			status, builder, scope, cond_life);

	if (!!status) {
		/* if the condition value is a maybe type, then we'll need multiple
		 * anded conditions to be true in order to actuall fall into the then
		 * block, let's figure out those conditions */
		llvm::Value *llvm_raw_condition_value = get_raw_condition_value(status,
				builder, scope, condition, condition_value);

		if (!!status && llvm_raw_condition_value != nullptr) {
			/* test that the if statement doesn't return */
			llvm::Function *llvm_function_current = llvm_get_function(builder);

			/* generate some new blocks */
			llvm::BasicBlock *then_bb = llvm::BasicBlock::Create(builder.getContext(), "then", llvm_function_current);

			/* we have to keep track of whether we need a merge block
			 * because our nested branches could all return */
			bool insert_merge_bb = false;

			llvm::BasicBlock *else_bb = llvm::BasicBlock::Create(builder.getContext(), "else", llvm_function_current);

			/* put the merge block after the else block */
			llvm::BasicBlock *merge_bb = llvm::BasicBlock::Create(builder.getContext(), "ifcont");

			/* create the actual branch instruction */
			llvm_create_if_branch(status, builder, scope, IFF_ELSE, cond_life,
					llvm_raw_condition_value, then_bb, else_bb);

			if (!!status) {
				builder.SetInsertPoint(else_bb);

				if (else_ != nullptr) {
					else_->resolve_statement(status, builder, scope, life,
							nullptr, &else_block_returns);
				}

				if (!else_block_returns) {
					/* keep track of the fact that we have to have a
					 * merged block to land in after the else block */
					insert_merge_bb = true;

					/* go ahead and jump there */
					if (!builder.GetInsertBlock()->getTerminator()) {
						builder.CreateBr(merge_bb);
					}
				}

				if (!!status) {
					/* let's generate code for the "then" block */
					builder.SetInsertPoint(then_bb);
					llvm::Value *llvm_bool_overload_value = maybe_get_bool_overload_value(status,
							builder, scope, cond_life, condition, condition_value);

					if (!!status) {
						if (llvm_bool_overload_value != nullptr) {
							/* we've got a second condition to check, let's do it */
							auto deep_then_bb = llvm::BasicBlock::Create(builder.getContext(), "deep-then", llvm_function_current);

							llvm_create_if_branch(status, builder, scope,
									IFF_THEN | IFF_ELSE, cond_life,
									llvm_bool_overload_value, deep_then_bb,
									else_bb ? else_bb : merge_bb);
							builder.SetInsertPoint(deep_then_bb);
						} else {
							cond_life->release_vars(status, builder, scope, lf_statement);
						}

						if (!!status) {
							block->resolve_statement(status, builder,
									if_scope ? if_scope : scope, life, nullptr, &if_block_returns);

							if (!!status) {
								if (!if_block_returns) {
									insert_merge_bb = true;
									if (!builder.GetInsertBlock()->getTerminator()) {
										builder.CreateBr(merge_bb);
									}
									builder.SetInsertPoint(merge_bb);
								}

								if (insert_merge_bb) {
									/* we know we'll need to fall through to the merge
									 * block, let's add it to the end of the function
									 * and let's set it as the next insert point. */
									llvm_function_current->getBasicBlockList().push_back(merge_bb);
									builder.SetInsertPoint(merge_bb);

								}

								/* track whether the branches return */
								*returns |= (if_block_returns && else_block_returns);

								assert(!!status);
								return;
							}
						}
					}
				}
			}
		}
	}

	assert(!status);
    return;
}

void ast::var_decl_t::resolve_statement(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
		local_scope_t::ref *new_scope,
		bool * /*returns*/) const
{
	if (auto runnable_scope = dyncast<runnable_scope_t>(scope)) {
		/* variable declarations begin new scopes */
		local_scope_t::ref fresh_scope = runnable_scope->new_local_scope(
				string_format("variable-%s", token.text.c_str()));

		scope = fresh_scope;

		/* check to make sure this var decl is sound */
		bound_var_t::ref var_decl_value = type_check_bound_var_decl(
				status, builder, fresh_scope, *this, life, false /*maybe_unbox*/);

		if (!!status) {
			*new_scope = fresh_scope;
			return;
		}
	} else {
		panic("we should not be trying to instantiate a var decl outside of a runnable scope");
	}

	assert(!status);
	return;
}

bound_var_t::ref ast::literal_expr_t::resolve_expression(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life) const
{
    scope_t::ref program_scope = scope->get_program_scope();

    switch (token.tk) {
	case tk_integer:
        {
			int64_t value = atoll(token.text.c_str());

			/* create a native integer */
			if (starts_with(token.text, "0x")) {
				value = strtoll(token.text.c_str() + 2, nullptr, 16);
			} else {
				value = atoll(token.text.c_str());
			}
			bound_type_t::ref native_type = program_scope->get_bound_type({INT_TYPE});
			return bound_var_t::create(
					INTERNAL_LOC(), "raw_int_literal", native_type,
					llvm_create_int(builder, value),
					make_code_id(token), false /*is_lhs*/,
					false /*is_global*/);
        }
		break;
    case tk_string:
		{
			std::string value = unescape_json_quotes(token.text);
			bound_type_t::ref native_type = program_scope->get_bound_type({STR_TYPE});
			return bound_var_t::create(
					INTERNAL_LOC(), "raw_str_literal", native_type,
					llvm_create_global_string(builder, value),
					make_code_id(token), false /*is_lhs*/,
					false /*is_global*/);
		}
		break;
	case tk_float:
		{
			double value = atof(token.text.c_str());
			bound_type_t::ref native_type = program_scope->get_bound_type({FLOAT_TYPE});
			return bound_var_t::create(
					INTERNAL_LOC(), "raw_float_literal", native_type,
					llvm_create_double(builder, value),
					make_code_id(token), false/*is_lhs*/,
					false /*is_global*/);
		}
		break;
    default:
        assert(false);
    };

    assert(!status);
    return nullptr;
}

bound_var_t::ref ast::reference_expr_t::resolve_overrides(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		const ptr<const ast::item_t> &callsite,
		const bound_type_t::refs &args) const
{
	indent_logger indent(5, string_format(
				"reference_expr_t::resolve_overrides for %s",
				callsite->str().c_str()));

	// scope->dump(std::cerr);
	auto name = join_with(path, ".", [](identifier::ref x) -> std::string {
		return x->get_name().str();
	});

	/* ok, we know we've got some variable here */
	auto bound_var = get_callable(status, builder, scope, name,
			shared_from_this(), get_args_type(args));
	if (!!status) {
		return bound_var;
	} else {
		user_error(status, callsite->get_location(), "while checking %s with %s",
				callsite->str().c_str(),
				::str(args).c_str());
		return nullptr;
	}
}

bound_var_t::ref ast::cast_expr_t::resolve_expression(
		status_t &status,
	   	llvm::IRBuilder<> &builder,
	   	scope_t::ref scope,
		life_t::ref life) const
{
	bound_var_t::ref bound_var = lhs->resolve_expression(status, builder, scope, life);
	if (!!status) {
		bound_type_t::ref bound_type = upsert_bound_type(status, builder, scope, type_cast);
		if (!!status) {
			// TODO: consider checking for certain casting
			llvm::Value *llvm_var_value = llvm_maybe_pointer_cast(builder,
					bound_var->resolve_value(builder), bound_type);

			return bound_var_t::create(INTERNAL_LOC(), "cast",
					bound_type, llvm_var_value, make_iid("cast"),
					false /*is_lhs*/,
					false /*is_global*/);
		}
	}

	assert(!status);
	return nullptr;
}

void dump_builder(llvm::IRBuilder<> &builder) {
	std::cerr << llvm_print_function(llvm_get_function(builder)) << std::endl;
}
