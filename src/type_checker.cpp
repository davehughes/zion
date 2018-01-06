#include "zion.h"
#include "atom.h"
#include "logger.h"
#include "type_checker.h"
#include "utils.h"
#include "callable.h"
#include "compiler.h"
#include "llvm_zion.h"
#include "llvm_utils.h"
#include "ast.h"
#include "llvm_types.h"
#include "parser.h"
#include "unification.h"
#include "code_id.h"
#include "patterns.h"
#include <iostream>
#include "type_kind.h"
#include "null_check.h"
#include <time.h>
#include "coercions.h"

/*
 * The basic idea here is that type checking is a graph operation which can be
 * ordered topologically based on dependencies between callers and callees.
 * Luckily our AST has exactly that structure.  We will perform a topological
 * sort by resolving types as we return from our depth first traversal.
 */


/************************************************************************/

bound_type_t::ref get_fully_bound_param_info(
		status_t &status,
		llvm::IRBuilder<> &builder,
		const ast::var_decl_t &obj,
		scope_t::ref scope,
		std::string &var_name,
		std::set<std::string> &generics,
		int &generic_index)
{
	if (!!status) {
		/* get the name of this parameter */
		var_name = obj.token.text;

		assert(obj.type != nullptr);

		/* the user specified a type */
		if (!!status) {
			debug_above(6, log(log_info, "upserting type for param %s at %s",
						obj.type->str().c_str(),
						obj.type->get_location().str().c_str()));
			return upsert_bound_type(status, builder, scope, obj.type);
		}
	}

	assert(!status);
	return nullptr;
}


llvm::Value *resolve_init_var(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		const ast::like_var_decl_t &obj,
		const std::string &symbol,
		types::type_t::ref declared_type,
		llvm::Function *llvm_function,
		bound_var_t::ref init_var,
		bound_type_t::ref value_type,
		bool is_managed)
{
	/* assumption here is that init_var has already been unified against the declared type.
	 * if this function returns an AllocaInst then that will imply that the variable should be
	 * treated as a ref that can be changed. */
	if (!!status) {
		llvm::AllocaInst *llvm_alloca;
		if (is_managed) {
			/* we need stack space, and we have to track it for garbage collection */
			llvm_alloca = llvm_call_gcroot(llvm_function, value_type, symbol);
		} else if (obj.is_let()) {
			/* we don't need a stack var */
			llvm_alloca = nullptr;
		} else {
			/* we need some stack space because this name is mutable */
			llvm_alloca = llvm_create_entry_block_alloca(llvm_function, value_type, symbol);
		}

		if (init_var == nullptr) {
			if (dyncast<const types::type_maybe_t>(declared_type)) {
				/* this can be null, and we do not allow user-defined __init__ for maybe types, so let's initialize it as null */
				llvm::Constant *llvm_null_value = llvm::Constant::getNullValue(value_type->get_llvm_specific_type());
				if (llvm_alloca == nullptr) {
					return llvm_null_value;
				} else {
					assert(!obj.is_let());
					assert(llvm_alloca != nullptr);
					builder.CreateStore(llvm_null_value, llvm_alloca);
					return llvm_alloca;
				}
			} else {
				/* this is not a maybe type */
				/* the user didn't supply an initializer, let's see if this type has one */
				auto init_fn = get_callable(
						status,
						builder,
						scope->get_module_scope(),
						"__init__",
						obj.get_location(),
						type_args({}, {}),
						value_type->get_type());

				if (!!status) {
					init_var = make_call_value(status, builder, obj.get_location(), scope,
							life, init_fn, {} /*arguments*/);
				} else {
					user_error(status, obj.get_location(), "missing initializer");
				}
			}
		}

		if (!!status) {
			if (init_var != nullptr) {
				llvm::Value *llvm_init_value;
				if (!init_var->type->get_type()->is_null()) {
					llvm_init_value = coerce_value(status, builder, scope, 
							obj.get_location(), value_type->get_type(), init_var);
				} else {
					llvm_init_value = llvm::Constant::getNullValue(value_type->get_llvm_specific_type());
				}

				if (llvm_init_value->getName().size() == 0) {
					llvm_init_value->setName(string_format("%s.initializer", symbol.c_str()));
				}

				if (llvm_alloca == nullptr) {
					/* this is a native 'let' */
					assert(obj.is_let());
					assert(!is_managed);
					return llvm_init_value;
				} else {
					debug_above(6, log(log_info, "creating a store instruction %s := %s",
								llvm_print(llvm_alloca).c_str(),
								llvm_print(llvm_init_value).c_str()));

					builder.CreateStore(llvm_init_value, llvm_alloca);
					if (obj.is_let()) {
						/* this is a managed 'let' */
						assert(is_managed);
						return builder.CreateLoad(llvm_alloca);
					} else {
						/* this is a native or managed 'var' */
						return llvm_alloca;
					}
				}
			}
		}
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref generate_stack_variable(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		const ast::like_var_decl_t &obj,
		const std::string &symbol,
		types::type_t::ref declared_type,
		bool maybe_unbox)
{
	/* 'init_var' is keeping track of the value we are assigning to our new
	 * variable (if any exists.) */
	bound_var_t::ref init_var;

	/* only check initializers inside a runnable scope */
	assert(dyncast<runnable_scope_t>(scope) != nullptr);

	if (obj.has_initializer()) {
		/* we have an initializer */
		init_var = obj.resolve_initializer(status, builder, scope, life);
		if (!!status) {
			if (init_var->type->is_void()) {
				user_error(status, obj.get_location(),
						"cannot initialize a variable with void, since it has no value");
			}
		}
	}

	/* 'stack_var_type' is keeping track of what the stack variable's type will be (hint: it should
	 * just be a ref to the value_type) */
	bound_type_t::ref stack_var_type;

	/* 'value_type' is keeping track of what the variable's ending type will be */
	bound_type_t::ref value_type;

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
						scope->get_nominal_env());

				if (unification.result) {
					/* the lhs is a supertype of the rhs */
					declared_type = declared_type->rebind(unification.bindings);
				} else {
					/* report that the variable type does not match the initializer type */
					user_error(status, obj.get_location(),
							"declared type of `" c_var("%s") "` does not match type of initializer",
							obj.get_symbol().c_str());
					user_info(status, init_var->get_location(), c_type("%s") " != " c_type("%s") " because %s",
							declared_type->str().c_str(),
							init_var->type->str().c_str(),
							unification.reasons.c_str());
				}
			} else {
				/* we must get the type from the initializer */
				declared_type = init_var->type->get_type();
			}
		}

		assert(declared_type != nullptr);

		if (maybe_unbox) {
			debug_above(3, log(log_info, "attempting to unbox %s", obj.get_symbol().c_str()));

			/* try to see if we can unbox this if it's a Maybe */
			if (init_var == nullptr) {
				user_error(status, obj.get_location(), "missing initialization value");
			} else {
				/* since we are maybe unboxing, then let's first off see if
				 * this is even a maybe type. */
				if (auto maybe_type = dyncast<const types::type_maybe_t>(declared_type)) {
					/* looks like the initialization variable is a supertype
					 * of the null type */
					unboxed = true;

					stack_var_type = upsert_bound_type(status, builder, scope,
							type_ref(maybe_type->just));
					if (!!status) {
						value_type = upsert_bound_type(status, builder, scope, maybe_type->just);
					}
				} else {
					/* this is not a maybe, so let's just move along */
				}
			}
		}

		if (stack_var_type == nullptr) {
			stack_var_type = upsert_bound_type(status, builder, scope, type_ref(declared_type));
			if (!!status) {
				value_type = upsert_bound_type(status, builder, scope, declared_type);
			}
		}
	}

	if (!!status) {
		/* generate the mutable stack-based variable for this var */
		llvm::Function *llvm_function = llvm_get_function(builder);

		// NOTE: we don't make this a gcroot until a little later on
		bool is_managed;
		value_type->is_managed_ptr(status, builder, scope, is_managed);
		llvm::Value *llvm_value = nullptr;
		if (!!status) {
			llvm_value = resolve_init_var(status, builder, scope, life, obj, symbol, declared_type,
					llvm_function, init_var, value_type, is_managed);
		}

		if (!!status) {
			/* the reference_expr that looks at this llvm_value will need to
			 * know to use store/load semantics, not just pass-by-value */
			bound_var_t::ref var_decl_variable = bound_var_t::create(
					INTERNAL_LOC(),
					symbol,
					llvm::dyn_cast<llvm::AllocaInst>(llvm_value) ? stack_var_type : value_type,
					llvm_value,
					make_type_id_code_id(obj.get_location(), obj.get_symbol()));

			/* memory management */
			if (!!status) {
				life->track_var(status, builder, scope, var_decl_variable, lf_block);

				if (!!status) {
					/* on our way out, stash the variable in the current scope */
					scope->put_bound_variable(status, var_decl_variable->name, var_decl_variable);

					if (!!status) {
						if (unboxed) {
							/* 'condition_value' refers to whether this was an unboxed maybe */
							bound_var_t::ref condition_value;

							assert(init_var != nullptr);
							assert(maybe_unbox);

							/* get the maybe type so that we can use it as a conditional */
							bound_type_t::ref condition_type = upsert_bound_type(status, builder, scope, declared_type);
							llvm::Value *llvm_resolved_value = init_var->resolve_bound_var_value(builder);

							if (!!status) {
								/* we're unboxing a Maybe{any}, so let's return
								 * whether this was Empty or not... */
								return bound_var_t::create(INTERNAL_LOC(), symbol,
										condition_type, llvm_resolved_value,
										make_type_id_code_id(obj.get_location(), obj.get_symbol()));
							}
						} else {
							return var_decl_variable;
						}
					}
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
		module_scope_t::ref module_scope,
		const ast::var_decl_t &var_decl,
		std::string symbol)
{
	auto program_scope = module_scope->get_program_scope();

	assert(!program_scope->has_checked(var_decl.shared_from_this()));
	program_scope->mark_checked(status, builder, var_decl.shared_from_this());

	if (!!status) {
		/* 'declared_type' tells us the user-declared type on the left-hand side of
		 * the assignment. */
		types::type_t::ref declared_type = var_decl.type->rebind(module_scope->get_type_variable_bindings());
		if (declared_type == nullptr || declared_type->ftv_count() != 0) {
			user_error(status, var_decl.get_location(), "module variables must have explicitly declared types");
			return nullptr;
		}

		assert(declared_type != nullptr);
		bound_type_t::ref bound_type = upsert_bound_type(status, builder, module_scope, declared_type);

		if (!!status) {
			auto bound_global_type = upsert_bound_type(status, builder, module_scope, type_ref(bound_type->get_type()));
			if (!!status) {
				llvm::Constant *llvm_constant = nullptr;
				if (bound_type->get_llvm_specific_type()->isPointerTy()) {
					llvm_constant = llvm::Constant::getNullValue(bound_type->get_llvm_specific_type());
				} else if (bound_type->get_llvm_specific_type()->isIntegerTy()) {
					llvm_constant = llvm::ConstantInt::get(bound_type->get_llvm_specific_type(), 0, false);
				} else {
					user_error(status, var_decl, "unsupported type for module variable %s",
							bound_type->str().c_str());
				}

				if (!!status) {
					llvm::Module *llvm_module = module_scope->get_llvm_module();
					llvm::GlobalVariable *llvm_global_variable = llvm_get_global(
							llvm_module,
							symbol,
							llvm_constant,
							false /*is_constant*/);

					bound_var_t::ref var_decl_variable = bound_var_t::create(INTERNAL_LOC(), symbol,
							bound_global_type, llvm_global_variable, make_code_id(var_decl.token));

					/* preemptively stash the variable in the module scope */
					module_scope->put_bound_variable(status, var_decl_variable->name,
							var_decl_variable);

					if (!!status) {
						function_scope_t::ref function_scope = module_scope->new_function_scope(
								std::string("__init_module_vars_") + symbol);

						/* 'init_var' is keeping track of the value we are assigning to our new
						 * variable (if any exists.) */
						bound_var_t::ref init_var;

						llvm::IRBuilderBase::InsertPointGuard ipg(builder);
						program_scope->set_insert_point_to_init_module_vars_function(
								status,
								builder,
								var_decl.token.text);

						if (!!status) {
							assert(llvm_get_function(builder) != nullptr);

							auto life = (
									make_ptr<life_t>(status, lf_function)
									->new_life(status, lf_block)
									->new_life(status, lf_statement));

							if (var_decl.initializer) {
								/* we have an initializer */
								init_var = var_decl.initializer->resolve_expression(status, builder,
										function_scope, life, false /*as_ref*/);
							}

							if (!!status) {
								if (init_var != nullptr) {
									/* we have an initializer */
									/* ensure 'init_var' <: 'declared_type' */
									unification_t unification = unify(
											declared_type,
											init_var->get_type(),
											module_scope->get_nominal_env());

									if (!unification.result) {
										/* report that the variable type does not match the initializer type */
										user_error(status, var_decl, "declared type of `" c_var("%s") "` does not match type of initializer",
												var_decl.token.text.c_str());
										user_message(log_info, status, init_var->get_location(), c_type("%s") " != " c_type("%s") " because %s",
												declared_type->str().c_str(),
												init_var->type->str().c_str(),
												unification.reasons.c_str());
									}
								}

								if (!!status) {
									if (init_var == nullptr) {
										/* the user didn't supply an initializer, let's see if this type has one */
										var_t::refs fns;
										auto init_fn = maybe_get_callable(
												status,
												builder,
												module_scope,
												"__init__",
												var_decl.get_location(),
												type_args({}, {}),
												declared_type,
												fns);

										if (!!status) {
											if (init_fn != nullptr) {
												init_var = make_call_value(status, builder, var_decl.get_location(),
														function_scope, life, init_fn, {} /*arguments*/);
											}
										}
									}

									if (!!status) {
										if (init_var != nullptr) {
											debug_above(6, log(log_info, "creating a store instruction %s := %s",
														llvm_print(llvm_global_variable).c_str(),
														llvm_print(init_var->get_llvm_value()).c_str()));

											llvm::Value *llvm_init_value = coerce_value(status, builder, module_scope, 
													var_decl.get_location(), declared_type, init_var);

											if (llvm_init_value->getName().str().size() == 0) {
												llvm_init_value->setName(string_format("%s.initializer", symbol.c_str()));
											}

											builder.CreateStore(
													llvm_maybe_pointer_cast(builder, llvm_init_value,
														bound_type->get_llvm_specific_type()),
													llvm_global_variable);
										} else {
											bool is_managed = false;
											var_decl_variable->type->is_managed_ptr(
													status,
													builder,
													module_scope,
													is_managed);

											if (!!status) {
												if (is_managed) {
													if (!var_decl_variable->type->is_maybe()) {
														user_error(status, var_decl, "module var " c_id("%s") " missing initializer",
																symbol.c_str());
													}
												}
											}
										}

										if (!!status) {
											life->release_vars(
													status,
													builder,
													function_scope,
													lf_function);
											if (!!status) {
												return var_decl_variable;
											}
										}
									}
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

bound_var_t::ref type_check_bound_var_decl(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		const ast::like_var_decl_t &obj,
		life_t::ref life,
		bool maybe_unbox)
{
	const std::string symbol = obj.get_symbol();

	debug_above(4, log(log_info, "type_check_bound_var_decl is looking for a type for variable " c_var("%s") " : %s",
				symbol.c_str(), obj.get_symbol().c_str()));

	assert(dyncast<module_scope_t>(scope) == nullptr);
	bound_var_t::ref bound_var;
	if (scope->symbol_exists_in_running_scope(symbol, bound_var)) {
		user_error(status, obj.get_location(), "symbol '" c_id("%s") "' cannot be redeclared",
				symbol.c_str());
		user_info(status, bound_var->get_location(), "see earlier symbol declaration %s",
				bound_var->str().c_str());
		return nullptr;
	}

	if (!!status) {
		assert(obj.get_type() != nullptr);

		/* 'declared_type' tells us the user-declared type on the left-hand side of
		 * the assignment. this is generally used to allow a variable to be more
		 * generalized than the specific right-hand side initial value might be. */
		types::type_t::ref declared_type = obj.get_type()->rebind(scope->get_type_variable_bindings());

		assert(dyncast<runnable_scope_t>(scope) != nullptr);

		return generate_stack_variable(status, builder, scope, life,
				obj, symbol, declared_type, maybe_unbox);
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref type_check_module_var_decl(
		status_t &status,
		llvm::IRBuilder<> &builder,
		module_scope_t::ref module_scope,
		const ast::var_decl_t &var_decl)
{
	auto program_scope = module_scope->get_program_scope();
	const std::string symbol = var_decl.token.text;

	debug_above(4, log(log_info, "type_check_module_var_decl is looking for a type for variable " c_var("%s") " : %s",
				symbol.c_str(), var_decl.str().c_str()));

	if (!program_scope->has_checked(var_decl.shared_from_this())) {
		return generate_module_variable(status, builder, module_scope, var_decl, symbol);
	} else {
		auto bound_var = module_scope->get_bound_variable(status, var_decl.get_location(), symbol, false /*search_parents*/);
		assert(bound_var != nullptr);
		return bound_var;
	}
}

std::vector<std::string> get_param_list_decl_variable_names(ast::param_list_decl_t::ref obj) {
	std::vector<std::string> names;
	for (auto param : obj->params) {
		names.push_back({param->token.text});
	}
	return names;
}

bound_type_t::named_pairs zip_named_pairs(
		std::vector<std::string> names,
		bound_type_t::refs args)
{
	bound_type_t::named_pairs named_args;
	assert(names.size() == args.size());
	for (size_t i = 0; i < args.size(); ++i) {
		named_args.push_back({names[i], args[i]});
	}
	return named_args;
}

status_t get_fully_bound_param_list_decl_variables(
		llvm::IRBuilder<> &builder,
		ast::param_list_decl_t &obj,
		scope_t::ref scope,
		bound_type_t::named_pairs &params)
{
	status_t status;

	/* we keep track of the generic parameters to ensure equivalence */
	std::set<std::string> generics;
	int generic_index = 1;

	for (auto param : obj.params) {
		std::string var_name;
		bound_type_t::ref param_type = get_fully_bound_param_info(status,
				builder, *param, scope, var_name, generics, generic_index);

		if (!!status) {
			params.push_back({var_name, param_type});
		}
	}
	return status;
}

bound_type_t::ref get_return_type_from_return_type_expr(
		status_t &status,
		llvm::IRBuilder<> &builder,
		types::type_t::ref type,
		scope_t::ref scope)
{
	/* lookup the alias, default to void */
	if (type != nullptr) {
		return upsert_bound_type(status, builder, scope, type);
	} else {
		/* user specified no return type, default to void */
		return scope->get_program_scope()->get_bound_type({"void"});
	}

	assert(!status);
	return nullptr;
}

void type_check_fully_bound_function_decl(
		status_t &status,
		llvm::IRBuilder<> &builder,
		const ast::function_decl_t &obj,
		scope_t::ref scope,
		bound_type_t::named_pairs &params,
		bound_type_t::ref &return_value)
{
	/* returns the parameters and the return value types fully resolved */
	debug_above(4, log(log_info, "type checking function decl %s", obj.token.str().c_str()));

	if (obj.param_list_decl) {
		/* the parameter types as per the decl */
		status |= get_fully_bound_param_list_decl_variables(builder,
				*obj.param_list_decl, scope, params);

		if (!!status) {
			return_value = get_return_type_from_return_type_expr(status,
					builder, obj.return_type, scope);

			/* we got the params, and the return value */
			return;
		}
	} else {
		user_error(status, obj, "no param_list_decl was present");
	}

	assert(!status);
}

bool type_is_unbound(types::type_t::ref type, const types::type_t::map &bindings) {
	return type->rebind(bindings)->ftv_count() > 0;
}

bool is_function_defn_generic(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		const ast::function_defn_t &obj)
{
	if (!!status) {
		if (obj.decl->param_list_decl) {
			/* check the parameters' genericity */
			auto &params = obj.decl->param_list_decl->params;
			for (auto &param : params) {
				if (!param->type) {
					debug_above(6, log(log_info, "found a missing parameter type on %s, defaulting it to an unnamed generic",
								param->str().c_str()));
					return true;
				}

				if (!!status) {
					if (type_is_unbound(param->type, scope->get_type_variable_bindings())) {
						debug_above(6, log(log_info, "found a generic parameter type on %s",
									param->str().c_str()));
						return true;
					}
				} else {
					/* failed to check type genericity */
					panic("what now hey?");
					return true;
				}
			}
		} else {
			panic("function declaration has no parameter list");
		}

		if (!!status) {
			if (obj.decl->return_type != nullptr) {
				/* check the return type's genericity */
				return obj.decl->return_type->ftv_count() > 0;
			} else {
				/* default to void, which is fully bound */
				return false;
			}
		}
	}

	assert(!status);
	return false;
}

function_scope_t::ref make_param_list_scope(
		status_t &status,
		llvm::IRBuilder<> &builder,
		const ast::function_decl_t &obj,
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

		assert(obj.param_list_decl->params.size() == params.size());

		llvm::Function *llvm_function = llvm::cast<llvm::Function>(function_var->get_llvm_value());
		llvm::Function::arg_iterator args = llvm_function->arg_begin();

		int i = 0;

		for (auto &param : params) {
			llvm::Value *llvm_param = &(*args++);
			if (llvm_param->getName().str().size() == 0) {
				llvm_param->setName(param.first);
			}

			assert(!param.second->is_ref());

			bool allow_reassignment = false;
			auto param_type = param.second->get_type();
			if (!param_type->is_ref() && !param_type->is_null()) {
				allow_reassignment = true;
			}

			/* create a slot for the final param value to be determined */
			llvm::Value *llvm_param_final = llvm_param;

			if (allow_reassignment) {
				param_type = type_ref(param_type);
				/* create an alloca in order to be able to reassign the named
				 * parameter to a new value. this does not mean that the parameter
				 * is an out param, we are simply enabling reuse of the name */
				llvm::AllocaInst *llvm_alloca = llvm_create_entry_block_alloca(
						llvm_function, param.second, param.first);

				// REVIEW: how to manage memory for named parameters? if we allow
				// changing their value then we have to enforce addref/release
				// semantics on them...
				debug_above(6, log(log_info, "creating a local alloca for parameter %s := %s",
							llvm_print(llvm_alloca).c_str(),
							llvm_print(llvm_param).c_str()));
				builder.CreateStore(llvm_param, llvm_alloca);	
				llvm_param_final = llvm_alloca;
			}

			auto bound_stack_var_type = upsert_bound_type(status, builder,
					scope, param_type);
			if (!!status) {
				auto param_var = bound_var_t::create(INTERNAL_LOC(), param.first, bound_stack_var_type,
						llvm_param_final, make_code_id(obj.param_list_decl->params[i++]->token));

				bound_type_t::ref return_type = get_function_return_type(status, builder, scope, function_var->type);

				if (!!status) {
					life->track_var(status, builder, scope, param_var, lf_function);

					if (!!status) {
						/* add the parameter argument to the current scope */
						new_scope->put_bound_variable(status, param.first, param_var);
					}
				}
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

void ast::expression_t::resolve_statement(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		local_scope_t::ref *new_scope,
		bool *returns) const
{
	/* expressions as statements just pass through to evaluating the expr */
	resolve_expression(status, builder, scope, life, false /*as_ref*/);
}

void ast::link_module_statement_t::resolve_statement(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		local_scope_t::ref *new_scope,
		bool *returns) const
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
				linked_module_name, make_code_id(token), linked_module_scope);

		module_scope->put_bound_variable(status, link_as_name.text, module_variable);

		if (!!status) {
			return;
		}
	} else {
		/* some modules may not create a module scope if they are marked as global modules */
		return;
	}

	assert(!status);
	return;
}

bound_var_t::ref ast::link_var_statement_t::resolve_expression(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		bool as_ref) const
{
	module_scope_t::ref module_scope = dyncast<module_scope_t>(scope);
	if (module_scope == nullptr) {
		user_error(status, get_location(), "link var cannot be used outside of module scope");
	}

	if (!!status) {
		return var_decl->resolve_as_link(status, builder, module_scope);
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::link_function_statement_t::resolve_expression(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		bool as_ref) const
{
	assert(!as_ref);

	/* FFI */
	module_scope_t::ref module_scope = dyncast<module_scope_t>(scope);
	assert(module_scope);

	bound_type_t::named_pairs named_args;
	bound_type_t::ref return_value;

	type_check_fully_bound_function_decl(status, builder, *extern_function, scope, named_args, return_value);
	if (!!status) {
		assert(return_value != nullptr);

		if (!!status) {
			bound_type_t::refs args;
			for (auto &named_arg_pair : named_args) {
				args.push_back(named_arg_pair.second);
			}

			// TODO: rearrange this, and get the pointer type
			llvm::FunctionType *llvm_func_type = llvm_create_function_type(
					status, builder, args, return_value);

			/* try to find this function, if it already exists... and make sure we use the "link to" name, if specified. */
			llvm::Module *llvm_module = module_scope->get_llvm_module();
			llvm::Value *llvm_value = llvm_module->getOrInsertFunction(extern_function->link_to_name.text, llvm_func_type);

			assert(llvm_print(llvm_value->getType()) != llvm_print(llvm_func_type));

			/* get the full function type */
			types::type_function_t::ref function_sig = get_function_type(args, return_value);
			debug_above(3, log(log_info, "%s has type %s",
						extern_function->get_function_name().c_str(),
						function_sig->str().c_str()));

			/* actually create or find the finalized bound type for this function */
			bound_type_t::ref bound_function_type = upsert_bound_type(
					status, builder, scope, function_sig);

			return bound_var_t::create(
					INTERNAL_LOC(),
					scope->make_fqn(extern_function->token.text),
					bound_function_type,
					llvm_value,
					make_code_id(extern_function->token));
		}
	}

	assert(!status);
	return nullptr;
}

void ast::link_name_t::resolve_statement(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		local_scope_t::ref *new_scope,
		bool *returns) const
{
	not_impl();
}

bound_var_t::ref ast::dot_expr_t::resolve_overrides(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		const ptr<const ast::item_t> &callsite,
		const bound_type_t::refs &args) const
{
	INDENT(5, string_format(
				"dot_expr_t::resolve_overrides for %s with %s",
				callsite->str().c_str(),
				::str(args).c_str()));

	/* check the left-hand side first, it should be a type_namespace */
	bound_var_t::ref lhs_var = lhs->resolve_expression(
			status, builder, scope, life, false /*as_ref*/);

	if (!!status) {
		if (auto bound_module = dyncast<const bound_module_t>(lhs_var)) {
			assert(bound_module->module_scope != nullptr);

			/* let's see if the associated module has a method that can handle this callsite */
			return get_callable(status, builder, bound_module->module_scope,
					rhs.text, callsite->get_location(),
					get_args_type(args),
					nullptr);
		} else {
			bound_var_t::ref bound_fn = this->resolve_expression(status, builder, scope, life, false /*as_ref*/);

			if (!!status) {
				unification_t unification = unify(
						bound_fn->type->get_type(),
						get_function_type(args, type_variable(INTERNAL_LOC())),
						scope->get_nominal_env());

				if (unification.result) {
					return bound_fn;
				} else {
					user_error(status, *lhs,
							"function %s is not compatible with arguments %s",
							bound_fn->str().c_str(),
							::str(args).c_str());
				}
			}
		}
	}

	assert(!status);
	return nullptr;
}

ptr<ast::callsite_expr_t> expand_callsite_string_literal(
		token_t token,
		std::string module,
		std::string function_name,
		std::string param)
{
	param = clean_ansi_escapes(param);
	/* create the function name, which is a fully qualified module.function expression */
	auto dot_expr = ast::create<ast::dot_expr_t>(token);
	dot_expr->lhs = ast::create<ast::reference_expr_t>(token_t{token.location, tk_identifier, module});
	dot_expr->rhs = token_t{token.location, tk_identifier, function_name};

	/* have the dot expr call with the `param` value as its one parameter */
	auto callsite = ast::create<ast::callsite_expr_t>(token);
	callsite->function_expr = dot_expr;
	callsite->params = std::vector<ptr<ast::expression_t>>{
		ast::create<ast::literal_expr_t>(token_t{token.location, tk_raw_string, escape_json_quotes(param) + "r"})
	};

	return callsite;
}

bound_var_t::ref resolve_assert_macro(
		status_t &status,
		llvm::IRBuilder<> &builder, 
		scope_t::ref scope, 
		life_t::ref life,
		token_t token,
		ptr<ast::expression_t> condition)
{
	auto if_block = ast::create<ast::if_block_t>(token);
	auto not_expr = ast::create<ast::prefix_expr_t>(
			token_t{token.location, tk_identifier, "not"});
	not_expr->rhs = condition;
	if_block->condition = not_expr;

	auto callsite = expand_callsite_string_literal(token, "runtime", "on_assert_failure", 
				string_format("%s: assertion %s failed",
					token.location.str(true, true).c_str(),
					condition->str().c_str()));

	auto block = ast::create<ast::block_t>(token);
	block->statements.push_back(callsite);
	if_block->block = block;

	bool if_block_returns = false;
	if_block->resolve_statement(
			status,
			builder,
			scope,
			life,
			nullptr,
			&if_block_returns);

	if (!!status) {
		return nullptr;
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::callsite_expr_t::resolve_expression(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		bool as_ref) const
{
	/* get the value of calling a function */
	bound_type_t::refs param_types;
	bound_var_t::refs arguments;

	if (auto symbol = dyncast<ast::reference_expr_t>(function_expr)) {
		if (symbol->token.text == "static_print") {
			if (params.size() == 1) {
				auto param = params[0];
				bound_var_t::ref param_var = param->resolve_expression(
						status, builder, scope, life, true /*as_ref*/);

				if (!!status) {
					user_message(log_info, status, param->get_location(),
							"%s : %s", param->str().c_str(),
							param_var->type->str().c_str());
					return nullptr;
				}

				assert(!status);
				return nullptr;
			} else {
				user_error(status, *shared_from_this(),
						"static_print requires one and only one parameter");

				assert(!status);
				return nullptr;
			}
		} else if (symbol->token.text == "assert") {
			/* do a crude macro expansion here and evaluate that */
			if (params.size() == 1) {
				auto param = params[0];
				return resolve_assert_macro(status, builder, scope, life, symbol->token, param);

			} else {
				user_error(status, *shared_from_this(), "assert accepts and requires one parameter");

				assert(!status);
				return nullptr;
			}
		} else if (symbol->token.text == "__is_non_null__") {
			return resolve_null_check(status, builder, scope, life, get_location(), params, nck_is_non_null);
		} else if (symbol->token.text == "__is_null__") {
			return resolve_null_check(status, builder, scope, life, get_location(), params, nck_is_null);
		}
	}

	/* iterate through the parameters and add their types to a vector */
	for (auto &param : params) {
		// TODO: consider changing the semantics of as_ref to be "allow_ref"
		// which would disallow the parameter from being a ref type, even if
		// it is naturally.
		bound_var_t::ref param_var = param->resolve_expression(
				status, builder, scope, life, false /*as_ref*/);

		if (!status) {
			break;
		}
		debug_above(6, log("argument %s -> %s", param->str().c_str(), param_var->type->str().c_str()));

		assert(!param_var->get_type()->is_ref());

		arguments.push_back(param_var);
		param_types.push_back(param_var->type);
	}

	if (!!status) {
		if (auto can_reference_overloads = dyncast<can_reference_overloads_t>(function_expr)) {
			/* we need to figure out which overload to call, if there are any */
			debug_above(6, log("arguments to resolve in callsite are %s",
						::str(arguments).c_str()));
			bound_var_t::ref function = can_reference_overloads->resolve_overrides(
					status, builder, scope, life, shared_from_this(),
					bound_type_t::refs_from_vars(arguments));

			if (!!status) {
				debug_above(5, log(log_info, "function chosen is %s", function->str().c_str()));

				return make_call_value(status, builder, get_location(), scope,
						life, function, arguments);
			}
		} else {
			user_error(status, *function_expr,
					"%s being called like a function. arguments are %s",
					function_expr->str().c_str(),
					::str(arguments).c_str());
			return nullptr;
		}
	}


	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::typeinfo_expr_t::resolve_expression(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		bool as_ref) const
{
	auto bindings = scope->get_type_variable_bindings();
	auto full_type = type->rebind(bindings);
	debug_above(3, log("evaluating typeinfo(%s)",
				full_type->str().c_str()));
	auto bound_type = upsert_bound_type(status, builder, scope, full_type);
	if (!!status) {
		types::type_t::ref expanded_type = full_eval(full_type, scope->get_total_env());
		debug_above(3, log("type evaluated to %s", expanded_type->str().c_str()));
		
		/* destructure the structure that this should have */
		if (auto pointer = dyncast<const types::type_ptr_t>(expanded_type)) {
			if (auto managed = dyncast<const types::type_managed_t>(pointer->element_type)) {
				expanded_type = managed->element_type;
			} else {
				assert(false);
				return null_impl();
			}
		} else if (auto ref = dyncast<const types::type_ref_t>(expanded_type)) {
			// bug in not handling this above?
			assert(false);
		}

		/* at this point we should have a struct type in expanded_type */
		if (auto struct_type = dyncast<const types::type_struct_t>(expanded_type)) {
			bound_type_t::refs args = upsert_bound_types(status,
					builder, scope, struct_type->dimensions);

			dbg();
			// TODO: find the dtor
			return upsert_type_info(
					status,
					builder,
					scope,
					struct_type->repr().c_str(),
					full_type->get_location(),
					bound_type,
					args,
					nullptr,
					nullptr);
		} else if (auto extern_type = dyncast<const types::type_extern_t>(expanded_type)) {
			/* we need this in order to be able to get runtime type information */
			auto program_scope = scope->get_program_scope();
			std::string type_info_var_name = extern_type->inner->repr();
			bound_type_t::ref var_ptr_type = program_scope->get_runtime_type(status, builder, "var_t", true /*get_ptr*/);
			if (!!status) {
				/* before we go create this type info, let's see if it already exists */
				auto bound_type_info = program_scope->get_bound_variable(status, full_type->get_location(),
						type_info_var_name);

				if (!!status) {
					if (bound_type_info != nullptr) {
						/* we've already created this bound type info, so let's just return it */
						return bound_type_info;
					}

					/* we have to create it */
					auto bound_underlying_type = upsert_bound_type(status, builder, scope, extern_type->underlying_type);
					if (!!status) {
						auto llvm_linked_type = bound_underlying_type->get_llvm_type();
						llvm::Module *llvm_module = llvm_get_module(builder);

#if 0
						llvm::FunctionType *llvm_var_fn_type = llvm::FunctionType::get(
								builder.getVoidTy(),
								llvm::ArrayRef<llvm::Type*>(
									std::vector<llvm::Type*>{var_ptr_type->get_llvm_type()}),
								false /*isVarArg*/);
#endif

						/* get references to the functions named by the user */
						bound_var_t::ref finalize_fn = get_callable(
								status,
								builder,
								scope,
								extern_type->link_finalize_fn->get_name(),
								get_location(),
								type_args({var_ptr_type->get_type()}, {}),
								type_void());
						if (!!status) {
							llvm::Constant *llvm_finalize_fn = llvm::dyn_cast<llvm::Constant>(finalize_fn->get_llvm_value());

							bound_var_t::ref mark_fn = get_callable(
									status,
									builder,
									scope,
									extern_type->link_mark_fn->get_name(),
									get_location(),
									type_args({var_ptr_type->get_type()}, {}),
									type_void());
							if (!!status) {
								llvm::Constant *llvm_mark_fn = llvm::dyn_cast<llvm::Constant>(mark_fn->get_llvm_value());

								bound_type_t::ref type_info = program_scope->get_runtime_type(status, builder, "type_info_t");
								bound_type_t::ref type_info_mark_fn = program_scope->get_runtime_type(status, builder, "type_info_mark_fn_t");
								if (!!status) {
									llvm::StructType *llvm_type_info_type = llvm::cast<llvm::StructType>(
											type_info->get_llvm_type());

									llvm::Constant *llvm_sizeof_tuple = llvm_sizeof_type(builder, llvm_linked_type);
									auto signature = full_type->get_signature();

									llvm::Constant *llvm_type_info = llvm_create_constant_struct_instance(
											llvm_type_info_type,
											{
											/* the type_id */
											builder.getInt32(atomize(signature)),

											/* the kind of this type_info */
											builder.getInt32(type_kind_use_mark_fn),

											/* allocation size */
											llvm_sizeof_tuple,

											/* name this variable */
											(llvm::Constant *)builder.CreateGlobalStringPtr(type_info_var_name),
											});

									llvm::Constant *llvm_type_info_mark_fn = llvm_create_struct_instance(
											string_format("__type_info_mark_fn_%s", signature.c_str()),
											llvm_module,
											llvm::dyn_cast<llvm::StructType>(type_info_mark_fn->get_llvm_type()),
											{
											/* the type info header */
											llvm_type_info,

											/* finalize_fn */
											llvm_finalize_fn,

											/* mark_fn */
											llvm_mark_fn,
											});

									debug_above(5, log(log_info, "llvm_type_info_mark_fn = %s",
												llvm_print(llvm_type_info_mark_fn).c_str()));

									bound_type_t::ref type_info_ptr_type = program_scope->get_runtime_type(status, builder, "type_info_t", true /*get_ptr*/);
									if (!!status) {
										auto bound_type_info_var = bound_var_t::create(
												INTERNAL_LOC(),
												type_info_var_name,
												type_info_ptr_type,
												llvm::ConstantExpr::getPointerCast(
													llvm_type_info_mark_fn,
													type_info_ptr_type->get_llvm_type()),
												make_iid("type info value"));

										program_scope->put_bound_variable(status,
												type_info_var_name,
												bound_type_info_var);
										return bound_type_info_var;
									}
								}
							}
						}
					}
				}
			}
		} else {
			not_impl();
		}

		assert(!status);
		return nullptr;
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::reference_expr_t::resolve_expression(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		bool as_ref) const
{
	/* we wouldn't be referencing a variable name here unless it was unique
	 * override resolution only happens on callsites, and we don't allow
	 * passing around unresolved overload references */
	bound_var_t::ref var = scope->get_bound_variable(status, get_location(), token.text);

	/* get_bound_variable can return nullptr without an user_error */
	if (var != nullptr) {
		assert(!!status);

		if (!as_ref) {
			return var->resolve_bound_value(status, builder, scope);
		} else {
			return var;
		}
	} else {
		indent_logger indent(get_location(), 5, string_format("looking for reference_expr " c_id("%s"),
					token.text.c_str()));
		var_t::refs fns;
		auto function = maybe_get_callable(status, builder, scope, token.text,
				get_location(), type_variable(get_location()), type_variable(get_location()), fns);
		if (!!status && function != nullptr) {
			debug_above(5, log("reference expression for " c_id("%s") " resolved to %s",
						token.text.c_str(), function->str().c_str()));
			return function;
		} else {
			debug_above(5, log("could not find reference expression for " c_id("%s") " (found %d fns, though)",
						token.text.c_str(), fns.size()));
		}
	}

	user_error(status, *this, "undefined symbol " c_id("%s"), token.text.c_str());
	return nullptr;
}

bound_var_t::ref ast::array_index_expr_t::resolve_expression(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		bool as_ref) const
{
	return resolve_assignment(status, builder, scope, life, as_ref, nullptr);
}

bound_var_t::ref resolve_pointer_array_index(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		types::type_t::ref element_type,
		ast::expression_t::ref index,
		bound_var_t::ref index_val,
		ast::expression_t::ref lhs,
		bound_var_t::ref lhs_val,
		bool as_ref,
		ast::expression_t::ref rhs)
{
	/* this is a native pointer - aka an array in memory */
	if (!dyncast<const types::type_managed_t>(element_type)) {
		debug_above(5, log("__getitem__ found that we are looking for items of type %s",
					element_type->str().c_str()));

		// REVIEW: consider just checking the LLVM type for whether it's an integer type
		unification_t index_unification = unify(
				index_val->type->get_type(),
				type_integer(type_variable(INTERNAL_LOC()), type_variable(INTERNAL_LOC())),
				scope->get_nominal_env());

		if (index_unification.result) {
			debug_above(5, log(log_info,
						"dereferencing %s[%s] with a GEP",
						lhs->str().c_str(), 
						index_val->str().c_str()));

			/* create the GEP instruction */
			std::vector<llvm::Value *> gep_path = std::vector<llvm::Value *>{index_val->get_llvm_value()};

			llvm::Value *llvm_gep = builder.CreateGEP(lhs_val->get_llvm_value(), gep_path);

			debug_above(5, log(log_info,
						"created dereferencing GEP %s : %s (element type is %s)",
						llvm_print(*llvm_gep).c_str(),
						llvm_print(llvm_gep->getType()).c_str(),
						element_type->str().c_str()));

			if (rhs == nullptr) {
				/* get the element type (taking as_ref into consideration) */
				bound_type_t::ref bound_element_type = upsert_bound_type(
						status, builder, scope,
						as_ref ? type_ref(element_type) : element_type);

				if (!!status) {
					return bound_var_t::create(
							INTERNAL_LOC(),
							{"dereferenced.pointer"},
							bound_element_type,
							as_ref ? llvm_gep : builder.CreateLoad(llvm_gep),
							make_iid_impl("dereferenced.pointer", lhs_val->get_location()));
				}
			} else {
				/* we are assigning to a native pointer dereference */
				auto value = rhs->resolve_expression(status, builder, scope, life, false /*as_ref*/);
				llvm::Value *llvm_value = coerce_value(status, builder, scope, lhs->get_location(),
						element_type, value);
				if (!!status) {
					builder.CreateStore(llvm_value, llvm_gep);
					return nullptr;
				}
			}
		} else {
			user_error(status, index->get_location(),
					"pointer index must be of an integer type. your index is of type %s",
					index_val->type->get_type()->str().c_str());
		}
	}

	assert(!status);
	return nullptr;
}

types::type_args_t::ref get_function_args_types(bound_type_t::ref function_type) {
	if (auto type_function = dyncast<const types::type_function_t>(function_type->get_type())) {
		return dyncast<const types::type_args_t>(type_function->args);
	}

	assert(false);
	return nullptr;
}

types::type_struct_t::ref get_struct_type_from_bound_type(
		status_t &status,
		scope_t::ref scope,
		location_t location,
		bound_type_t::ref bound_type)
{
	auto type = full_eval(bound_type->get_type(), scope->get_total_env());

	if (auto maybe_type = dyncast<const types::type_maybe_t>(type)) {
		user_error(status, location, "maybe types cannot be dereferenced. try checking whether it's not equal to null first");
		return nullptr;
	} else if (auto tuple_type = dyncast<const types::type_tuple_t>(type)) {
		return type_struct(tuple_type->dimensions, {});
	} else if (auto ptr_type = dyncast<const types::type_ptr_t>(type)) {
		if (auto managed_type = dyncast<const types::type_managed_t>(ptr_type->element_type)) {
			if (auto struct_type = dyncast<const types::type_struct_t>(managed_type->element_type)) {
				return struct_type;
			}
		} else {
			if (auto struct_type = dyncast<const types::type_struct_t>(ptr_type->element_type)) {
				return struct_type;
			}
		}
	}

	user_error(status, location,
			"could not find any member variables within %s",
			bound_type->str().c_str());

	assert(!status);
	return nullptr;
}

bound_var_t::ref extract_member_by_index(
		status_t &status, 
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		location_t location,
		bound_var_t::ref bound_var,
		bound_type_t::ref bound_obj_type,
		int index,
		std::string member_name,
		bool as_ref)
{
	types::type_struct_t::ref struct_type = get_struct_type_from_bound_type(
			status, scope, location, bound_obj_type);

	if (!!status) {
		if (index < 0 || index >= (int)struct_type->dimensions.size()) {
			user_error(status, location, "tuple index is out of bounds. tuple %s has %d elements",
					struct_type->str().c_str(), (int)struct_type->dimensions.size());
		}

		if (!!status) {
			/* get an GEP-able version of the object */
			llvm::Value *llvm_var_value = llvm_maybe_pointer_cast(builder,
				   	bound_var->resolve_bound_var_value(builder),
					bound_obj_type->get_llvm_specific_type());

			/* the following code is heavily coupled to the physical layout of
			 * managed vs. native structures */

			/* GEP and load the member value from the structure */
			llvm::Value *llvm_gep = llvm_make_gep(builder,
					llvm_var_value, index,
					types::is_managed_ptr(bound_var->get_type(),
						scope->get_total_env()) /* managed */);
			if (llvm_gep->getName().str().size() == 0) {
				llvm_gep->setName(string_format("address_of.%s", member_name.c_str()));
			}

			llvm::Value *llvm_item = as_ref ? llvm_gep : builder.CreateLoad(llvm_gep);

			if (llvm_item->getName().str().size() == 0) {
				/* add a helpful descriptive name to this local value */
				auto value_name = string_format(".%s", member_name.c_str());
				llvm_item->setName(value_name);
			}

			/* get the type of the dimension being referenced */
			bound_type_t::ref member_type = upsert_bound_type(status, builder, scope, 
					as_ref
					? type_ref(struct_type->dimensions[index])
					: struct_type->dimensions[index]);

			if (!!status) {
				return bound_var_t::create(
						INTERNAL_LOC(), "tuple.element",
						member_type, llvm_item, make_iid("tuple.element"));
			}
		}
	}
	assert(!status);
	return nullptr;
}

int64_t parse_int_value(status_t &status, token_t token) {
	switch (token.tk) {
	case tk_integer:
		{
			int64_t value;
			if (token.text.size() > 2 && token.text.substr(0, 2) == "0x") {
				value = strtoll(token.text.substr(2).c_str(), nullptr, 16);
			} else {
				value = atoll(token.text.c_str());
			}
			return value;
		}
	case tk_raw_integer:
		{
			/* create a native integer */
			int64_t value = atoll(token.text.substr(0, token.text.size() - 1).c_str());
			if (token.text.size() > 3 && token.text.substr(0, 2) == "0x") {
				value = strtoll(token.text.substr(0, token.text.size() - 1).c_str(),
						nullptr, 16);
			} else {
				value = atoll(token.text.substr(0, token.text.size() - 1).c_str());
			}
			return value;
		}
	default:
		user_error(status, token.location, "unable to read an integer value from %s", token.str().c_str());
		break;
	}

	assert(!status);
	return 0;
}

int get_constant_int(status_t &status, ast::item_t::ref item) {
	return parse_int_value(status, item->token);
}

bound_var_t::ref type_check_assignment(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		bound_var_t::ref lhs_var,
		bound_var_t::ref rhs_var,
		location_t location)
{
	if (!lhs_var->is_ref()) {
		user_error(status, location,
				"you cannot re-assign the name " c_id("%s") " to anything else here. it is not assignable.",
				lhs_var->name.c_str());
		user_info(status, lhs_var->get_location(),
				"see declaration of " c_id("%s") " with type %s",
				lhs_var->name.c_str(),
				lhs_var->type->get_type()->str().c_str());
	}

	if (!!status) {
		INDENT(5, string_format(
					"type checking assignment %s = %s",
					lhs_var->str().c_str(),
					rhs_var->str().c_str()));

		auto lhs_unreferenced_type = dyncast<const types::type_ref_t>(lhs_var->type->get_type())->element_type;
		bound_type_t::ref lhs_unreferenced_bound_type = upsert_bound_type(status, builder, scope, lhs_unreferenced_type);

		if (!!status) {
			unification_t unification = unify(
					lhs_unreferenced_type,
					rhs_var->type->get_type(),
				   	scope->get_nominal_env());

			if (unification.result) {
				llvm::Value *llvm_rhs_value = coerce_value(status, builder, scope, 
									location, lhs_unreferenced_type, rhs_var);
				if (!!status) {
					assert(llvm::dyn_cast<llvm::AllocaInst>(lhs_var->get_llvm_value())
							|| llvm::dyn_cast<llvm::GlobalVariable>(lhs_var->get_llvm_value())
                            || llvm_value_is_pointer(lhs_var->get_llvm_value()));

					builder.CreateStore(llvm_rhs_value, lhs_var->get_llvm_value());

					if (!!status) {
						return lhs_var;
					}
				}
			} else {
				user_error(status, location, "left-hand side is incompatible with the right-hand side (%s)",
						unification.str().c_str());
			}
		}
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::array_index_expr_t::resolve_assignment(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		bool as_ref,
		const ast::expression_t::ref &rhs) const
{
	/* this expression looks like this (the rhs is optional)
	 *
	 *   lhs[index] = rhs
	 *
	 */

	if (rhs != nullptr) {
		/* make sure to treat the array dereference as a reference if we are doing an assignment */
		as_ref = true;
	}

	if (!!status) {
		bound_var_t::ref lhs_val = lhs->resolve_expression(status, builder,
				scope, life, false /*as_ref*/);

		if (!!status) {
			if (auto tuple_type = dyncast<const types::type_tuple_t>(lhs_val->type->get_type())) {
				int member_index = get_constant_int(status, index);
				if (!!status) {
					bound_var_t::ref value = extract_member_by_index(status, builder, scope, life,
							get_location(), lhs_val, lhs_val->type, member_index, string_format("%d", member_index), as_ref);
					if (!!status) {
						if (rhs != nullptr) {
							/* let's assign into this tuple slot */
							bound_var_t::ref rhs_val = rhs->resolve_expression(status, builder, scope, life, false /*as_ref*/);

							if (!!status) {
								type_check_assignment(status, builder, scope, life, value,
										rhs_val, token.location);
								return nullptr;
							}
						} else {
							return value;
						}
					}
				}
			} else {
				bound_var_t::ref index_val = index->resolve_expression(status, builder,
						scope, life, false /*as_ref*/);

				if (!!status) {
					identifier::ref element_type_var = types::gensym();

					/* check to see if we are employing pointer arithmetic here */
					unification_t unification = unify(
							lhs_val->type->get_type(),
							type_ptr(type_variable(element_type_var)),
							scope->get_nominal_env());

					if (unification.result) {
						/* this is a native pointer, let's generate code to write or read, or reference it */
						types::type_t::ref element_type = unification.bindings[element_type_var->get_name()];
						return resolve_pointer_array_index(status, builder, scope, life, element_type, index, index_val,
								lhs, lhs_val, as_ref, rhs);
					} else if (rhs == nullptr) {
						/* this is not a native pointer we are dereferencing */
						debug_above(5, log("attempting to call " c_id("__getitem__")
									" on %s and %s", lhs_val->str().c_str(), index_val->str().c_str()));

						// TODO: consider whether to allow as_ref
						// assert(!as_ref);

						/* get or instantiate a function we can call on these arguments */
						return call_program_function(status, builder, scope, life,
								"__getitem__", shared_from_this(), {lhs_val, index_val});
					} else {
						/* we're assigning to a managed array index expression */

						/* let's solve for the rhs */
						bound_var_t::ref rhs_val = rhs->resolve_expression(status, builder, scope, life, false /*as_ref*/);

						if (!!status) {
							/* we have a rhs to assign into this lhs, let's find the function we should be calling to do
							 * the update. */
							bound_var_t::ref setitem_function = get_callable(
									status,
									builder,
									scope,
									"__setitem__",
									get_location(),
									type_args({lhs_val->type->get_type(), index_val->type->get_type(), rhs_val->type->get_type()}),
									type_variable(INTERNAL_LOC()));
							if (!!status) {
								std::vector<llvm::Value *> llvm_args = get_llvm_values(
										status,
										builder,
										scope,
										get_location(),
										get_function_args_types(setitem_function->type),
										{lhs_val, index_val, rhs_val});

								bound_type_t::ref return_type = get_function_return_type(
										status,
										builder,
										scope,
										setitem_function->type);
								return bound_var_t::create(
										INTERNAL_LOC(),
										"array.index.assignment",
										return_type,
										llvm_create_call_inst(
											status, builder, lhs->get_location(),
											setitem_function,
											llvm_args),
										make_iid("array.index.assignment"));
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

bound_var_t::ref create_bound_vector_literal(
		status_t &status, 
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		location_t location,
		types::type_t::ref element_type,
		bound_var_t::refs bound_items)
{
	if (!!status) {
		auto program_scope = scope->get_program_scope();

		auto bound_var_ptr_type = program_scope->get_runtime_type(
				status, builder, "var_t", true /*get_ptr*/);

		if (!!status) {
			auto bound_var_ptr_ptr_type = upsert_bound_type(
					status, builder, scope,
					type_ptr(bound_var_ptr_type->get_type()));

			if (!!status) {
				/* create the type for this vector */
				types::type_t::ref vector_type = type_operator(
						type_id(make_iid_impl("vector.vector", location)),
						element_type);
				bound_type_t::ref bound_vector_type = upsert_bound_type(
						status, builder, scope, vector_type);

				/* get the function to allocate a vector and reserve enough space */
				bound_var_t::ref get_vector_init_function = get_callable(
						status,
						builder,
						scope,
						"vector.__init_vector__",
						location,
						type_args({type_id(make_iid("size_t"))}),
						vector_type);

				/* get the raw pointer type to vectors */
				bound_type_t::ref bound_base_vector_type = upsert_bound_type(status, builder, scope,
						type_ptr(type_id(make_iid_impl("vector.vector_t", location))));

				if (!!status) {
					/* get the append function for vectors */
					bound_var_t::ref get_vector_append_function = get_callable(
							status,
							builder,
							scope,
							"vector.__vector_unsafe_append__",
							location,
							type_args({type_ptr(type_id(make_iid("vector.vector_t"))),
								bound_var_ptr_type->get_type()}),
							type_id(make_iid("void")));

					if (!!status) {
						/* get a new vector of the given size */
						llvm::CallInst *llvm_vector = llvm_create_call_inst(
								status, builder, location, get_vector_init_function,
								{builder.getInt64(bound_items.size())});

						llvm::Value *llvm_raw_vector = llvm_maybe_pointer_cast(builder, llvm_vector, bound_base_vector_type->get_llvm_type());

						/* append all of the items */
						for (auto bound_item : bound_items) {
							/* call the append function */
							llvm_create_call_inst(status, builder, bound_item->get_location(), get_vector_append_function,
									{llvm_raw_vector, llvm_maybe_pointer_cast(builder, bound_item->get_llvm_value(), bound_var_ptr_type->get_llvm_type())});
							if (!status) {
								break;
							}
						}
						/* the type of the resultant vector */
						if (!!status) {
							return bound_var_t::create(
									INTERNAL_LOC(),
									"vector.literal",
									bound_vector_type,
									llvm_vector,
									make_iid_impl("vector.literal", location));
						}
					}
				}
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
		life_t::ref life,
		bool /*as_ref*/) const
{
	bound_var_t::refs bound_items;
	types::type_t::refs element_types;
	for (auto item : items) {
		auto bound_item = item->resolve_expression(status, builder, scope, life, false /*as_ref*/);
		if (!!status) {
			bound_items.push_back(bound_item);
			element_types.push_back(bound_item->type->get_type());
			bool is_managed;
			bound_item->type->is_managed_ptr(
					status,
					builder,
					scope,
					is_managed);
			if (!!status) {
				if (!is_managed) {
					user_error(status, bound_item->get_location(), "items in a vector literal must be managed objects. this one is a %s, which is not managed",
							bound_item->type->get_type()->str().c_str());
					break;
				}
			}
		}
		if (!status) {
			break;
		}
	}

	if (!!status) {
		types::type_t::ref element_type = type_sum_safe(
				element_types, get_location(), scope->get_nominal_env());
		debug_above(6, log("creating vector literal of %s",
					element_type->str().c_str()));
		return create_bound_vector_literal(
				status, builder, scope, life,
				get_location(), element_type, bound_items);
	}

	assert(!status);
	return nullptr;
}

enum rnpbc_t {
	rnpbc_eq,
	rnpbc_ineq,
	rnpbc_lt,
	rnpbc_lte,
	rnpbc_gt,
	rnpbc_gte,
};

bool rnpbc_equality_is_truth(rnpbc_t rnpbc) {
	switch (rnpbc) {
	case rnpbc_eq:
		return true;
	case rnpbc_ineq:
		return true;
	case rnpbc_lt:
		return false;
	case rnpbc_lte:
		return true;
	case rnpbc_gt:
		return false;
	case rnpbc_gte:
		return true;
	}
}

bool rnpbc_rhs_non_null_is_truth(rnpbc_t rnpbc) {
	switch (rnpbc) {
	case rnpbc_eq:
		return false;
	case rnpbc_ineq:
		return true;
	case rnpbc_lt:
		return true;
	case rnpbc_lte:
		return true;
	case rnpbc_gt:
		return false;
	case rnpbc_gte:
		return false;
	}
}

bool rnpbc_lhs_non_null_is_truth(rnpbc_t rnpbc) {
	switch (rnpbc) {
	case rnpbc_eq:
		return false;
	case rnpbc_ineq:
		return true;
	case rnpbc_lt:
		return false;
	case rnpbc_lte:
		return false;
	case rnpbc_gt:
		return true;
	case rnpbc_gte:
		return true;
	}
}

bound_var_t::ref resolve_native_pointer_binary_compare(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		location_t location,
		ast::expression_t::ref lhs_node,
		bound_var_t::ref lhs_var,
		ast::expression_t::ref rhs_node,
		bound_var_t::ref rhs_var,
		rnpbc_t rnpbc,
		local_scope_t::ref *scope_if_true,
		local_scope_t::ref *scope_if_false)
{
	if (lhs_var->type->get_type()->is_null()) {
		if (rhs_var->type->get_type()->is_null()) {
			return scope->get_program_scope()->get_bound_variable(
					status,
					location,
					rnpbc_equality_is_truth(rnpbc) ? "__true__" : "__false__",
					false /*search_parents*/);
		} else {
			auto null_check = rnpbc_rhs_non_null_is_truth(rnpbc) ? nck_is_non_null : nck_is_null;
			return resolve_null_check(status, builder, scope, life, location, rhs_node, rhs_var, null_check, scope_if_true, scope_if_false);
		}
	} else if (rhs_var->type->get_type()->is_null()) {
		auto null_check = rnpbc_lhs_non_null_is_truth(rnpbc) ? nck_is_non_null : nck_is_null;
		return resolve_null_check(status, builder, scope, life, location, lhs_node, lhs_var, null_check, scope_if_true, scope_if_false);
	} else {
		/* neither side is null */
		if (!lhs_var->is_pointer()) { std::cerr << lhs_var->str() << " " << llvm_print(lhs_var->get_llvm_value()) << std::endl; dbg(); }
		if (!rhs_var->is_pointer()) { std::cerr << rhs_var->str() << " " << llvm_print(rhs_var->get_llvm_value()) << std::endl; dbg(); }

		auto env = scope->get_nominal_env();
		if (
				!unifies(lhs_var->type->get_type(), rhs_var->type->get_type(), env) &&
				!unifies(rhs_var->type->get_type(), lhs_var->type->get_type(), env))
		{
			user_error(status, location, "values of types (%s and %s) cannot be compared",
					lhs_var->type->get_type()->str().c_str(),
					rhs_var->type->get_type()->str().c_str());
			return nullptr;
		}

		auto program_scope = scope->get_program_scope();
		llvm::Type *llvm_char_ptr_type = builder.getInt8Ty()->getPointerTo();

		llvm::Value *llvm_value;
		switch (rnpbc) {
		case rnpbc_eq:
			llvm_value = builder.CreateICmpEQ(
					builder.CreateBitCast(lhs_var->get_llvm_value(), llvm_char_ptr_type),
					builder.CreateBitCast(rhs_var->get_llvm_value(), llvm_char_ptr_type));
			break;
		case rnpbc_ineq:
			llvm_value = builder.CreateICmpNE(
					builder.CreateBitCast(lhs_var->get_llvm_value(), llvm_char_ptr_type),
					builder.CreateBitCast(rhs_var->get_llvm_value(), llvm_char_ptr_type));
			break;
		case rnpbc_lt:
			llvm_value = builder.CreateICmpULT(
					builder.CreateBitCast(lhs_var->get_llvm_value(), llvm_char_ptr_type),
					builder.CreateBitCast(rhs_var->get_llvm_value(), llvm_char_ptr_type));
			break;
		case rnpbc_lte:
			llvm_value = builder.CreateICmpULE(
					builder.CreateBitCast(lhs_var->get_llvm_value(), llvm_char_ptr_type),
					builder.CreateBitCast(rhs_var->get_llvm_value(), llvm_char_ptr_type));
			break;
		case rnpbc_gt:
			llvm_value = builder.CreateICmpUGT(
					builder.CreateBitCast(lhs_var->get_llvm_value(), llvm_char_ptr_type),
					builder.CreateBitCast(rhs_var->get_llvm_value(), llvm_char_ptr_type));
			break;
		case rnpbc_gte:
			llvm_value = builder.CreateICmpUGE(
					builder.CreateBitCast(lhs_var->get_llvm_value(), llvm_char_ptr_type),
					builder.CreateBitCast(rhs_var->get_llvm_value(), llvm_char_ptr_type));
			break;
		}

		auto bool_type = program_scope->get_bound_type(BOOL_TYPE);
		return bound_var_t::create(
				INTERNAL_LOC(),
				{"equality.cond"},
				bool_type,
				builder.CreateSExtOrTrunc(llvm_value, bool_type->get_llvm_specific_type()),
				make_iid_impl(std::string{"equality.cond"}, location));
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref resolve_native_pointer_binary_operation(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		location_t location,
		ast::expression_t::ref lhs_node,
		bound_var_t::ref lhs_var,
		ast::expression_t::ref rhs_node,
		bound_var_t::ref rhs_var,
		std::string function_name,
		local_scope_t::ref *scope_if_true,
		local_scope_t::ref *scope_if_false)
{
	if (function_name == "__eq__") {
		return resolve_native_pointer_binary_compare(status, builder, scope, life, location, lhs_node, lhs_var, rhs_node, rhs_var, rnpbc_eq, scope_if_true, scope_if_false);
	} else if (function_name == "__ineq__") {
		return resolve_native_pointer_binary_compare(status, builder, scope, life, location, lhs_node, lhs_var, rhs_node, rhs_var, rnpbc_ineq, scope_if_true, scope_if_false);
	} else if (function_name == "__lt__") {
		return resolve_native_pointer_binary_compare(status, builder, scope, life, location, lhs_node, lhs_var, rhs_node, rhs_var, rnpbc_lt, scope_if_true, scope_if_false);
	} else if (function_name == "__lte__") {
		return resolve_native_pointer_binary_compare(status, builder, scope, life, location, lhs_node, lhs_var, rhs_node, rhs_var, rnpbc_lte, scope_if_true, scope_if_false);
	} else if (function_name == "__gt__") {
		return resolve_native_pointer_binary_compare(status, builder, scope, life, location, lhs_node, lhs_var, rhs_node, rhs_var, rnpbc_gt, scope_if_true, scope_if_false);
	} else if (function_name == "__gte__") {
		return resolve_native_pointer_binary_compare(status, builder, scope, life, location, lhs_node, lhs_var, rhs_node, rhs_var, rnpbc_gte, scope_if_true, scope_if_false);
	} else {
		user_error(status, location, "native pointers cannot be combined using the " c_id("%s") " function", function_name.c_str());
	}
	assert(!status);
	return nullptr;
}

bound_var_t::ref type_check_binary_integer_op(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		location_t location,
		bound_var_t::ref lhs,
		bound_var_t::ref rhs,
		std::string function_name)
{
	auto typename_env = scope->get_nominal_env();

	static unsigned int_bit_size = 64;
	static bool int_signed = true;
	static bool initialized = false;

	bound_type_t::ref bound_int_type = upsert_bound_type(status, builder, scope, type_id(make_iid("int_t")));
	if (!status) { return nullptr; }

	if (!initialized) {

		types::get_integer_attributes(status, bound_int_type->get_type(), typename_env, int_bit_size, int_signed);
		if (!status) {
			panic("could not figure out the bit size and signedness of int_t!");
		}
		initialized = true;
	}

	unsigned lhs_bit_size, rhs_bit_size;
	bool lhs_signed = false, rhs_signed = false;
	types::get_integer_attributes(status, lhs->type->get_type(), typename_env, lhs_bit_size, lhs_signed);
	if (!!status) {
		types::get_integer_attributes(status, rhs->type->get_type(), typename_env, rhs_bit_size, rhs_signed);

		bound_type_t::ref final_integer_type;
		bool final_integer_signed = false;
		if (lhs_bit_size == rhs_bit_size) {
			if (!lhs_signed == !rhs_signed) {
				final_integer_signed = lhs_signed;
				final_integer_type = upsert_bound_type(status, builder, scope,
					   	type_integer(
							type_id(make_iid(string_format("%d", lhs_bit_size))),
							type_id(make_iid(string_format("%s", lhs_signed ? "signed" : "unsigned")))));
			} else {
				final_integer_signed = true;
				final_integer_type = upsert_bound_type(status, builder, scope,
					   	type_integer(
							type_id(make_iid(string_format("%d", lhs_bit_size))),
							type_id(make_iid("signed"))));
			}
		} else {
			final_integer_signed = true;
			final_integer_type = upsert_bound_type(status, builder, scope,
					type_integer(
						type_id(make_iid(string_format("%d", lhs_bit_size))),
						type_id(make_iid("signed"))));
		}

		if (!!status) {
			llvm::Value *llvm_lhs = lhs->get_llvm_value();
			llvm::Value *llvm_rhs = rhs->get_llvm_value();
			assert(llvm_lhs->getType()->isIntegerTy());
			assert(llvm_rhs->getType()->isIntegerTy());

#ifdef ZION_DEBUG
			auto llvm_lhs_type = llvm::dyn_cast<llvm::IntegerType>(llvm_lhs->getType());
			assert(llvm_lhs_type != nullptr);
			assert(llvm_lhs_type->getBitWidth() == lhs_bit_size);
			auto llvm_rhs_type = llvm::dyn_cast<llvm::IntegerType>(llvm_rhs->getType());
			assert(llvm_rhs_type != nullptr);
			assert(llvm_rhs_type->getBitWidth() == rhs_bit_size);
#endif

			unsigned computation_bit_size = std::max(std::max(lhs_bit_size, rhs_bit_size), int_bit_size);
			if (lhs_bit_size < computation_bit_size) {
				if (lhs_signed) {
					llvm_lhs = builder.CreateSExtOrTrunc(llvm_lhs, builder.getIntNTy(computation_bit_size));
				} else {
					llvm_lhs = builder.CreateZExtOrTrunc(llvm_lhs, builder.getIntNTy(computation_bit_size));
				}
			}
			if (lhs_bit_size < computation_bit_size) {
				if (lhs_signed) {
					llvm_lhs = builder.CreateSExtOrTrunc(llvm_lhs, builder.getIntNTy(computation_bit_size));
				} else {
					llvm_lhs = builder.CreateZExtOrTrunc(llvm_lhs, builder.getIntNTy(computation_bit_size));
				}
			}
			if (rhs_bit_size < computation_bit_size) {
				if (rhs_signed) {
					llvm_rhs = builder.CreateSExtOrTrunc(llvm_rhs, builder.getIntNTy(computation_bit_size));
				} else {
					llvm_rhs = builder.CreateZExtOrTrunc(llvm_rhs, builder.getIntNTy(computation_bit_size));
				}
			}

			auto bound_bool_type = scope->get_program_scope()->get_bound_type("bool_t");
			llvm::Value *llvm_value = nullptr;
			if (function_name == "__plus__") {
				llvm_value = builder.CreateAdd(llvm_lhs, llvm_rhs);
			} else if (function_name == "__minus__") {
				llvm_value = builder.CreateSub(llvm_lhs, llvm_rhs);
			} else if (function_name == "__times__") {
				llvm_value = builder.CreateMul(llvm_lhs, llvm_rhs);
			} else if (function_name == "__mod__") {
				if (final_integer_signed) {
					llvm_value = builder.CreateSRem(llvm_lhs, llvm_rhs);
				} else {
					llvm_value = builder.CreateURem(llvm_lhs, llvm_rhs);
				}
			} else if (function_name == "__divide__") {
				if (final_integer_signed) {
					llvm_value = builder.CreateSDiv(llvm_lhs, llvm_rhs);
				} else {
					llvm_value = builder.CreateUDiv(llvm_lhs, llvm_rhs);
				}
			} else if (function_name == "__lt__") {
				return bound_var_t::create(
						INTERNAL_LOC(),
						function_name + ".value",
						bound_bool_type,
						builder.CreateZExtOrTrunc(
						final_integer_signed
							? builder.CreateICmpSLT(llvm_lhs, llvm_rhs)
							: builder.CreateICmpULT(llvm_lhs, llvm_rhs),
							bound_bool_type->get_llvm_type()),
						make_iid(function_name + ".value"));
			} else if (function_name == "__lte__") {
				return bound_var_t::create(
						INTERNAL_LOC(),
						function_name + ".value",
						bound_bool_type,
						builder.CreateZExtOrTrunc(
						final_integer_signed
							? builder.CreateICmpSLE(llvm_lhs, llvm_rhs)
							: builder.CreateICmpULE(llvm_lhs, llvm_rhs),
							bound_bool_type->get_llvm_type()),
						make_iid(function_name + ".value"));
			} else if (function_name == "__gt__") {
				return bound_var_t::create(
						INTERNAL_LOC(),
						function_name + ".value",
						bound_bool_type,
						builder.CreateZExtOrTrunc(
						final_integer_signed
							? builder.CreateICmpSGT(llvm_lhs, llvm_rhs)
							: builder.CreateICmpUGT(llvm_lhs, llvm_rhs),
							bound_bool_type->get_llvm_type()),
						make_iid(function_name + ".value"));
			} else if (function_name == "__gte__") {
				return bound_var_t::create(
						INTERNAL_LOC(),
						function_name + ".value",
						bound_bool_type,
						builder.CreateZExtOrTrunc(
						final_integer_signed
							? builder.CreateICmpSGE(llvm_lhs, llvm_rhs)
							: builder.CreateICmpUGE(llvm_lhs, llvm_rhs),
							bound_bool_type->get_llvm_type()),
						make_iid(function_name + ".value"));
			} else if (function_name == "__ineq__") {
				return bound_var_t::create(
						INTERNAL_LOC(),
						function_name + ".value",
						bound_bool_type,
						builder.CreateZExtOrTrunc(builder.CreateICmpNE(llvm_lhs, llvm_rhs), bound_bool_type->get_llvm_type()),
						make_iid(function_name + ".value"));
			} else if (function_name == "__eq__") {
				return bound_var_t::create(
						INTERNAL_LOC(),
						function_name + ".value",
						bound_bool_type,
						builder.CreateZExtOrTrunc(builder.CreateICmpEQ(llvm_lhs, llvm_rhs), bound_bool_type->get_llvm_type()),
						make_iid(function_name + ".value"));
			} else if (function_name == "__shr__") {
				if (lhs_signed) {
					return bound_var_t::create(
							INTERNAL_LOC(),
							function_name + ".value",
							lhs->type,
							builder.CreateAShr(llvm_lhs, llvm_rhs),
							make_iid(function_name + ".value"));
				} else {
					return bound_var_t::create(
							INTERNAL_LOC(),
							function_name + ".value",
							lhs->type,
							builder.CreateLShr(llvm_lhs, llvm_rhs),
							make_iid(function_name + ".value"));
				}
			} else if (function_name == "__shl__") {
				return bound_var_t::create(
						INTERNAL_LOC(),
						function_name + ".value",
						lhs->type,
						builder.CreateShl(llvm_lhs, llvm_rhs),
						make_iid(function_name + ".value"));
			} else if (function_name == "__bitwise_and__") {
				return bound_var_t::create(
						INTERNAL_LOC(),
						function_name + ".value",
						lhs->type,
						builder.CreateAnd(llvm_lhs, llvm_rhs),
						make_iid(function_name + ".value"));
			} else if (function_name == "__bitwise_or__") {
				return bound_var_t::create(
						INTERNAL_LOC(),
						function_name + ".value",
						lhs->type,
						builder.CreateOr(llvm_lhs, llvm_rhs),
						make_iid(function_name + ".value"));
			} else if (function_name == "__xor__") {
				return bound_var_t::create(
						INTERNAL_LOC(),
						function_name + ".value",
						lhs->type,
						builder.CreateXor(llvm_lhs, llvm_rhs),
						make_iid(function_name + ".value"));
			} else {
				assert(false);
			}

			return bound_var_t::create(
					INTERNAL_LOC(),
					function_name + ".value",
					final_integer_type,
					final_integer_signed
						? builder.CreateSExtOrTrunc(llvm_value, final_integer_type->get_llvm_type())
						: builder.CreateZExtOrTrunc(llvm_value, final_integer_type->get_llvm_type()),
					make_iid(function_name + ".value"));
		}
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref type_check_binary_operator(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		ast::expression_t::ref lhs_node,
		bound_var_t::ref lhs,
		ast::expression_t::ref rhs_node,
		bound_var_t::ref rhs,
		ast::item_t::ref obj,
		std::string function_name,
		local_scope_t::ref *scope_if_true,
		local_scope_t::ref *scope_if_false)
{
	indent_logger indent(obj->get_location(), 6, string_format("checking binary operator " c_id("%s") " with operands %s and %s",
				function_name.c_str(),
				lhs->str().c_str(),
				rhs->str().c_str()));
	auto typename_env = scope->get_nominal_env();
	if (
			types::is_integer(lhs->type->get_type(), typename_env) &&
			types::is_integer(rhs->type->get_type(), typename_env))
	{
		/* we are dealing with two integers, standard function resolution rules do not apply */
		return type_check_binary_integer_op(
				status,
				builder,
				scope,
				life,
				obj->get_location(),
				lhs,
				rhs,
				function_name);
	} else {
		bool lhs_is_null = lhs->type->get_type()->is_null();
		bool rhs_is_null = rhs->type->get_type()->is_null();

		/* see whether we should just do a binary value comparison */
		if (
				(lhs->type->is_function()
				 || lhs->type->is_ptr(scope)
				 || lhs_is_null) &&
				(rhs->type->is_function()
				 || rhs->type->is_ptr(scope)
				 || rhs_is_null))
		{
			bool lhs_is_managed;
			lhs->type->is_managed_ptr(
					status,
					builder,
					scope,
					lhs_is_managed);
			if (!!status) {
				if (!lhs_is_managed || rhs_is_null) {
					bool rhs_is_managed;
					rhs->type->is_managed_ptr(
							status,
							builder,
							scope,
							rhs_is_managed);
					if (!!status) {
						if (!rhs_is_managed || lhs_is_null) {
							/* yeah, it looks like we are operating on two native pointers */
							return resolve_native_pointer_binary_operation(status, builder, scope,
									life, obj->get_location(), lhs_node, lhs, rhs_node, rhs, function_name, scope_if_true, scope_if_false);
						}
					}
				}
			}
		}

		if (!!status) {
			/* get or instantiate a function we can call on these arguments */
			return call_program_function(
					status, builder, scope, life, function_name,
					obj, {lhs, rhs});
		}
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref type_check_binary_operator(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		ptr<const ast::expression_t> lhs,
		ptr<const ast::expression_t> rhs,
		ast::item_t::ref obj,
		std::string function_name,
		local_scope_t::ref *scope_if_true,
		local_scope_t::ref *scope_if_false)
{
	if (!!status) {
		assert(function_name.size() != 0);

		bound_var_t::ref lhs_var, rhs_var;
		lhs_var = lhs->resolve_expression(status, builder, scope, life, false /*as_ref*/);
		if (!!status) {
			assert(!lhs_var->type->is_ref());

			if (!!status) {
				rhs_var = rhs->resolve_expression(status, builder, scope, life, false /*as_ref*/);

				if (!!status) {
					assert(!rhs_var->type->is_ref());

					return type_check_binary_operator(
							status, builder, scope, life, lhs, lhs_var, rhs, rhs_var, obj,
							function_name, scope_if_true, scope_if_false);
				}
			}
		}
	}
	assert(!status);
	return nullptr;
}


bound_var_t::ref type_check_binary_equality(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		ptr<const ast::expression_t> lhs,
		ptr<const ast::expression_t> rhs,
		ast::item_t::ref obj,
		std::string function_name,
		local_scope_t::ref *scope_if_true,
		local_scope_t::ref *scope_if_false)
{
	if (!!status) {
		bound_var_t::ref lhs_var, rhs_var;
		lhs_var = lhs->resolve_expression(status, builder, scope, life,
				false /*as_ref*/);
		if (!!status) {
			rhs_var = rhs->resolve_expression(status, builder, scope, life,
					false /*as_ref*/);

			if (!!status) {
				assert(!lhs_var->is_ref());
				assert(!rhs_var->is_ref());
				bool negated = (function_name == "__ineq__" || function_name == "__isnot__");
				return resolve_native_pointer_binary_compare(status, builder, scope, life, obj->get_location(), lhs, lhs_var, rhs, rhs_var,
					   	negated ? rnpbc_ineq : rnpbc_eq, scope_if_true, scope_if_false);
			}
		}
	}
	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::binary_operator_t::resolve_expression(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		bool as_ref) const
{
	if (token.is_ident(K(is))) {
		return type_check_binary_equality(status, builder, scope, life, lhs, rhs,
				shared_from_this(), function_name, nullptr, nullptr);
	}

	return type_check_binary_operator(status, builder, scope, life, lhs, rhs,
			shared_from_this(), function_name, nullptr, nullptr);
}

bound_var_t::ref ast::binary_operator_t::resolve_condition(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		local_scope_t::ref *scope_if_true,
		local_scope_t::ref *scope_if_false) const
{
	if (token.is_ident(K(is))) {
		return type_check_binary_equality(status, builder, scope, life, lhs, rhs,
				shared_from_this(), function_name, scope_if_true, scope_if_false);
	}

	return type_check_binary_operator(status, builder, scope, life, lhs, rhs,
			shared_from_this(), function_name, scope_if_true, scope_if_false);
}



bound_var_t::ref ast::tuple_expr_t::resolve_expression(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		bool as_ref) const
{
	assert(!as_ref);

	/* let's get the actual values in our tuple. */
	bound_var_t::refs vars;
	vars.reserve(values.size());

	for (auto &value: values) {
		bound_var_t::ref var = value->resolve_expression(status, builder,
				scope, life, false /*as_ref*/);
		if (!!status) {
			vars.push_back(var);
		}
	}

	if (!!status) {
		bound_type_t::refs args = get_bound_types(vars);

		/* let's get the type for this tuple wrapped as an object */
		types::type_tuple_t::ref tuple_type = get_tuple_type(args);

		/* now, let's see if we already have a ctor for this tuple type, if not
		 * we'll need to create a data ctor for this unnamed tuple type */
		auto program_scope = scope->get_program_scope();

		std::pair<bound_var_t::ref, bound_type_t::ref> tuple = upsert_tuple_ctor(
				status, builder, scope, tuple_type, shared_from_this());

		if (!!status) {
			/* now, let's call our unnamed tuple ctor and return that value */
			return create_callsite(status, builder, scope, life,
					tuple.first, tuple_type->repr(),
					token.location, vars);
		}
	}

	assert(!status);
	return nullptr;
}

llvm::Value *_get_raw_condition_value(
		status_t &status,
	   	llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		location_t location,
		bound_var_t::ref condition_value)
{
	// REVIEW: this function seems suspect now
	if (condition_value->is_int()) {
		return condition_value->resolve_bound_var_value(builder);
	} else if (condition_value->is_pointer()) {
		return condition_value->resolve_bound_var_value(builder);
	} else {
		user_error(status, location,
				"staring at the %s will bring you no closer to the truth",
				condition_value->type->str().c_str());
	}

	assert(!status);
	return nullptr;
}

llvm::Value *_maybe_get_bool_overload_value(
		status_t &status,
	   	llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		ast::item_t::ref condition,
		bound_var_t::ref condition_value)
{
	condition_value = condition_value->resolve_bound_value(status, builder, scope);
	if (!!status) {
		assert(life->life_form == lf_statement);

		llvm::Value *llvm_condition_value = nullptr;
		// TODO: check whether we are checking a raw value or not

		debug_above(2, log(log_info,
					"attempting to resolve a " c_var("%s") " override if condition %s, ",
					BOOL_TYPE,
					condition->str().c_str()));

		assert(false);
		/* we only ever get in here if we are definitely non-null, so we can discard
		 * maybe type specifiers */
		types::type_t::ref condition_type;
		if (auto maybe = dyncast<const types::type_maybe_t>(condition_value->type->get_type())) {
			condition_type = maybe->just;
		} else {
			condition_type = condition_value->type->get_type();
		}

		var_t::refs fns;
		auto bool_fn = maybe_get_callable(status, builder, scope, "__bool__",
				condition->get_location(), type_args({condition_type}), type_id(make_iid(BOOL_TYPE)), fns);

		if (!!status) {
			if (bool_fn != nullptr) {
				/* we've found a bool function that will take our condition as input */
				assert(bool_fn != nullptr);
				types::type_function_t::ref bool_fn_type = dyncast<const types::type_function_t>(bool_fn->type->get_type());
				assert(bool_fn_type != nullptr);
				types::type_args_t::ref bool_fn_args_type = dyncast<const types::type_args_t>(bool_fn_type->args);
				assert(bool_fn_args_type != nullptr);
				assert(bool_fn_args_type->args.size() == 1);

				debug_above(7, log(log_info, "generating a call to " c_var("bool") "(%s) for if condition evaluation (type %s)",
							condition->str().c_str(), bool_fn->type->str().c_str()));

				llvm::Value *llvm_value = coerce_value(
						status, builder, scope,
						condition_value->get_location(),
						bool_fn_args_type->args[0],
						condition_value);

				if (!!status) {
					/* let's call this bool function */
					llvm_condition_value = llvm_create_call_inst(
							status, builder, condition->get_location(),
							bool_fn,
							{llvm_value});

					if (!!status) {
						/* NB: no need to track this value in a life because it's
						 * returning an integer type */
						assert(llvm_condition_value->getType()->isIntegerTy());
						return llvm_condition_value;
					}
				}
			} else {
				/* treat all values without overloaded bool functions as truthy */
				return nullptr;
			}
		}
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref resolve_cond_expression( /* ternary expression */
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		bool as_ref,
		ast::condition_t::ref condition,
		ast::expression_t::ref when_true,
		ast::expression_t::ref when_false,
		identifier::ref value_name,
		local_scope_t::ref *scope_if_true,
		local_scope_t::ref *scope_if_false)
{
	/* these scopes are calculated for the interior conditional branching in order to provide refined types for the
	 * when_true or when_false branches */
	local_scope_t::ref inner_scope_if_true;
	local_scope_t::ref inner_scope_if_false;

	indent_logger indent(condition->get_location(), 6, string_format("resolving ternary expression (%s) ? (%s) : (%s)",
				condition->str().c_str(), when_true->str().c_str(), when_false->str().c_str()));

	/* if scope allows us to set up new variables inside if conditions */
	bound_var_t::ref condition_value = condition->resolve_condition(status, builder, scope, life, &inner_scope_if_true, &inner_scope_if_false);

	if ((condition != when_false) && (condition != when_true)) {
		/* this is a regular ternary expression. for now, there are 4 paths through in terms of truthiness and
		 * falsiness, and there is no way to propagate out the learned truths from the inner conditional branch because
		 * the when_true and when_false branches may yield differing notions of t/f which cannot be distilled into 2
		 * matching truths. */
	}

	/* evaluate the condition for branching */
	debug_above(7, log("conditional expression has condition of type %s", condition_value->type->str().c_str()));

	if (!!status) {
		assert(!condition_value->type->is_ref());

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

		/* create the inner branch instruction */
		llvm_create_if_branch(status, builder, scope, 0,
				nullptr, condition->get_location(), condition_value, then_bb, else_bb);

		if (!!status) {
			/* calculate the false path's value in the else block */
			builder.SetInsertPoint(else_bb);
			bound_var_t::ref false_path_value;
			if (condition == when_false) {
				/* this is an AND expression, so don't recompute the false value */
				false_path_value = condition_value;
			} else if (condition == when_true) {
				/* this is an OR expression, so compute the second term, and build upon any type
				 * refinements we've acquired so far. */
				if (inner_scope_if_false != nullptr) {
					*scope_if_false = inner_scope_if_false;
				}
				false_path_value = when_false->resolve_condition(
						status, builder, inner_scope_if_false ? inner_scope_if_false : scope, life,
						nullptr, scope_if_false);
			} else {
				/* this is a TERNARY expression, so compute the third term, and do not return any
				 * type refinements, because there is no way to discern where the truthy or
				 * falseyness of this entire expression came from (in the context of our parent
				 * conditional form. */
				false_path_value = when_false->resolve_condition(
						status, builder, inner_scope_if_false ? inner_scope_if_false : scope, life,
						nullptr, nullptr);
			}

			/* after calculation, the code should jump to the phi node's basic block */
			llvm::Instruction *false_merge_branch = builder.CreateBr(merge_bb);

			if (!!status) {
				/* let's generate code for the "true-path" block */
				builder.SetInsertPoint(then_bb);

				/* get the bound_var for the truthy path */
				bound_var_t::ref true_path_value;
				if (condition == when_true) {
					/* this is an OR expression, so don't recompute the true value */
					true_path_value = condition_value;
				} else if (condition == when_false) {
					/* this is an AND expression, so compute the second term, and build upon any
					 * type refinements we've acquired so far. */
					if (inner_scope_if_true != nullptr) {
						*scope_if_true = inner_scope_if_true;
					}
					true_path_value = when_true->resolve_condition(
							status, builder, inner_scope_if_true ? inner_scope_if_true : scope, life,
							scope_if_true, nullptr);
				} else {
					/* this is a TERNARY expression, so compute the third term, and do not return
					 * any type refinements, because there is no way to discern where the truthy or
					 * falseyness of this entire expression came from (in the context of our parent
					 * conditional form. */
					true_path_value = when_true->resolve_condition(
							status, builder, inner_scope_if_true ? inner_scope_if_true : scope, life,
							nullptr, nullptr);
				}

				if (!!status) {
					bound_type_t::ref ternary_type;
					// If the when-true is same as condition, we can eliminate any
					// falsey types.

					types::type_t::ref truthy_path_type = true_path_value->type->get_type();
					types::type_t::ref falsey_path_type = false_path_value->type->get_type();

					auto env = scope->get_nominal_env();
					if (condition == when_true) {
						/* we can remove falsey types from the truthy path type */
						truthy_path_type = truthy_path_type->boolean_refinement(false, env);
					} else if (condition == when_false) {
						/* we can remove truthy types from the truthy path type */
						falsey_path_type = falsey_path_type->boolean_refinement(true, env);
					}

					auto condition_type = condition_value->type->get_type();
					if (condition_type->boolean_refinement(false, env) == nullptr) {
						/* the condition value was definitely falsey */
						/* factor out the truthy path type entirely */
						truthy_path_type = nullptr;
					} else if (condition_type->boolean_refinement(true, env) == nullptr) {
						/* the condition value was definitely truthy */
						/* factor out the falsey path type entirely */
						falsey_path_type = nullptr;
					}

					assert((truthy_path_type != nullptr) || (falsey_path_type != nullptr));

					types::type_t::refs options;
					if (truthy_path_type != nullptr) {
						options.push_back(truthy_path_type);
					}
					if (falsey_path_type != nullptr) {
						options.push_back(falsey_path_type);
					}

					/* the when_true and when_false values have different
					 * types, let's create a sum type to represent this */
					auto ternary_sum_type = type_sum_safe(options, condition->get_location(), env);
					assert(ternary_sum_type != nullptr);

					if (!!status) {
						ternary_type = upsert_bound_type(status,
								builder, scope, ternary_sum_type);
					}

					if (!!status) {
						llvm::Instruction *truthy_merge_branch = builder.CreateBr(merge_bb);
						builder.SetInsertPoint(merge_bb);

						llvm::PHINode *llvm_phi_node = llvm::PHINode::Create(
								ternary_type->get_llvm_specific_type(),
								2, "ternary.phi.node", merge_bb);

						llvm::Value *llvm_truthy_path_value = nullptr;
						/* BLOCK */ {
							/* make sure that we cast the incoming phi value to the
							 * final type in the incoming BB, not in the merge BB */
							llvm::IRBuilder<> builder(truthy_merge_branch);
							llvm_truthy_path_value = llvm_maybe_pointer_cast(builder,
									true_path_value->resolve_bound_var_value(builder),
									ternary_type);
						}
						llvm_phi_node->addIncoming(llvm_truthy_path_value, then_bb);

						llvm::Value *llvm_false_path_value = nullptr;
						/* BLOCK */ {
							/* make sure that we cast the incoming phi value to the
							 * final type in the incoming BB, not in the merge BB */
							llvm::IRBuilder<> builder(false_merge_branch);
							llvm_false_path_value = llvm_maybe_pointer_cast(builder,
									false_path_value->resolve_bound_var_value(builder),
									ternary_type);
						}

						llvm_phi_node->addIncoming(llvm_false_path_value, else_bb);

						debug_above(6, log("ternary expression resolved to type %s",
									ternary_type->str().c_str()));
						return bound_var_t::create(
								INTERNAL_LOC(),
								{"ternary.value"},
								ternary_type,
								llvm_phi_node,
								value_name);
					}
				}
			}
		}
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::ternary_expr_t::resolve_expression(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		bool as_ref) const
{
	return resolve_cond_expression(status, builder, scope, life, as_ref,
			condition, when_true, when_false,
			make_code_id(this->token), nullptr, nullptr);
}

bound_var_t::ref ast::ternary_expr_t::resolve_condition(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		local_scope_t::ref *scope_if_true,
		local_scope_t::ref *scope_if_false) const
{
	return resolve_cond_expression(status, builder, scope, life, false /*as_ref*/,
			condition, when_true, when_false,
			make_code_id(this->token), scope_if_true, scope_if_false);
}

bound_var_t::ref ast::or_expr_t::resolve_expression(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		bool as_ref) const
{
	return resolve_cond_expression(status, builder, scope, life, as_ref,
			lhs, lhs, rhs, make_iid("or.value"), nullptr, nullptr);
}

bound_var_t::ref ast::or_expr_t::resolve_condition(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		local_scope_t::ref *scope_if_true,
		local_scope_t::ref *scope_if_false) const
{
	return resolve_cond_expression(status, builder, scope, life, false /*as_ref*/,
			lhs, lhs, rhs, make_iid("or.value"), scope_if_true, scope_if_false);
}

bound_var_t::ref ast::and_expr_t::resolve_expression(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		bool as_ref) const
{
	return resolve_cond_expression(status, builder, scope, life, as_ref,
			lhs, rhs, lhs, make_iid("and.value"), nullptr, nullptr);
}

bound_var_t::ref ast::and_expr_t::resolve_condition(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		local_scope_t::ref *scope_if_true,
		local_scope_t::ref *scope_if_false) const
{
	return resolve_cond_expression(status, builder, scope, life, false /*as_ref*/,
			lhs, rhs, lhs, make_iid("and.value"), scope_if_true, scope_if_false);
}

types::type_t::ref extract_matching_type(
		identifier::ref type_var_name,
	   	types::type_t::ref actual_type,
		types::type_t::ref pattern_type)
{
	unification_t unification = unify(actual_type, pattern_type, {});
	if (unification.result) {
		return unification.bindings[type_var_name->get_name()];
	} else {
		return nullptr;
	}
}

bound_var_t::ref extract_member_variable(
		status_t &status, 
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		ast::item_t::ref node,
		bound_var_t::ref bound_var,
		std::string member_name,
		bool as_ref)
{
	bound_var = bound_var->resolve_bound_value(status, builder, scope);

	if (!!status) {
		auto expanded_type = full_eval(bound_var->type->get_type(), scope->get_total_env());
		bound_type_t::ref bound_obj_type = upsert_bound_type(status, builder, scope, expanded_type);

		if (!!status) {
			types::type_struct_t::ref struct_type = get_struct_type_from_bound_type(
					status, scope, node->get_location(), bound_obj_type);
			if (!!status) {
				debug_above(5, log(log_info, "looking for member " c_id("%s") " in %s", member_name.c_str(),
							bound_obj_type->str().c_str()));

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
								struct_type->str().c_str(),
								index));

					debug_above(5, log(log_info, "looking at bound_var %s : %s",
								bound_var->str().c_str(),
								llvm_print(bound_var->type->get_llvm_type()).c_str()));

					return extract_member_by_index(status, builder, scope, 
							life, node->get_location(), bound_var, bound_obj_type, index, member_name, as_ref);

				} else {
					auto bindings = scope->get_type_variable_bindings();
					auto full_type = bound_var->type->get_type()->rebind(bindings);
					user_error(status, node->get_location(),
							"%s has no dimension called " c_id("%s"),
							full_type->str().c_str(),
							member_name.c_str());
					user_message(log_info, status, bound_var->type->get_location(), "%s has dimension(s) [%s]",
							full_type->str().c_str(),
							join_with(member_index, ", ", [] (std::pair<std::string, int> index) -> std::string {
								return std::string(C_ID) + index.first + C_RESET;
								}).c_str());
				}
			}
		}
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref resolve_module_variable_reference(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		location_t location,
		std::string module_name,
		std::string symbol,
		bool as_ref)
{
	std::string qualified_id = string_format("%s%s%s",
			module_name.c_str(),
			SCOPE_SEP,
			symbol.c_str());

	debug_above(5,
			log("attempt to find global id " c_id("%s"),
				qualified_id.c_str()));
	bound_var_t::ref var = scope->get_bound_variable(status, location, qualified_id);

	if (!!status) {
		/* if we couldn't resolve that id, let's look for unchecked variables */
		program_scope_t::ref program_scope = scope->get_program_scope();
		if (var == nullptr) {
			if (unchecked_var_t::ref unchecked_var = program_scope->get_unchecked_variable(qualified_id)) {
				if (ast::var_decl_t::ref var_decl = dyncast<const ast::var_decl_t>(unchecked_var->node)) {
					var = generate_module_variable(
							status,
							builder,
							unchecked_var->module_scope,
							*var_decl,
							symbol);
				} else {
					assert(false);
				}
			}
		}
	}

	if (!!status) {
		/* now, let's make sure to avoid returning refs if !as_ref */
		if (var != nullptr) {
			if (!as_ref) {
				/* if we're not asking for a ref, then get rid of it if it's there */
				return var->resolve_bound_value(status, builder, scope);
			} else {
				return var;
			}
		} else {
			/* check for unbound module variable */
			user_error(status, location, "could not find symbol " c_id("%s"), qualified_id.c_str());
		}
	}
	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::dot_expr_t::resolve_expression(
        status_t &status,
        llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		bool as_ref) const
{
	debug_above(6, log("resolving dot_expr %s", str().c_str()));
	bound_var_t::ref lhs_val = lhs->resolve_expression(status,
			builder, scope, life, false /*as_ref*/);

	if (!!status) {
		types::type_t::ref member_type;

		if (lhs_val->type->is_module()) {
			return resolve_module_variable_reference(status, builder, scope, get_location(),
					lhs_val->name, rhs.text, as_ref);
		} else {
			return extract_member_variable(status, builder, scope, life, shared_from_this(),
					lhs_val, rhs.text, as_ref);
		}
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref cast_bound_var(
		status_t &status,
	   	llvm::IRBuilder<> &builder,
	   	scope_t::ref scope,
		life_t::ref life,
		location_t location,
		bound_var_t::ref bound_var,
		types::type_t::ref type_cast)
{
	assert(!bound_var->is_ref());
	bound_type_t::ref bound_type = upsert_bound_type(status, builder, scope, type_cast);
	if (!!status) {
		debug_above(7, log("upserted bound type in cast expr is %s", bound_type->str().c_str()));
		indent_logger indent(location, 5, string_format("casting %s: %s (%s) to a %s (%s)",
					bound_var->name.c_str(),
					bound_var->type->get_type()->str().c_str(),
					llvm_print(bound_var->get_llvm_value()->getType()).c_str(),
					type_cast->str().c_str(),
					llvm_print(bound_type->get_llvm_specific_type()).c_str()));
		if (!!status) {
			llvm::Value *llvm_source_val = bound_var->resolve_bound_var_value(builder);
			llvm::Type *llvm_source_type = llvm_source_val->getType();

			llvm::Value *llvm_dest_val = nullptr;
			llvm::Type *llvm_dest_type = bound_type->get_llvm_specific_type();

			// TODO: put some more constraints on this...
			if (llvm_dest_type->isIntegerTy()) {
				/* we want an integer at the end... */
				if (llvm_source_type->isPointerTy()) {
					llvm_dest_val = builder.CreatePtrToInt(llvm_source_val, llvm_dest_type);
				} else {
					assert(llvm_source_type->isIntegerTy());
					llvm_dest_val = builder.CreateSExtOrTrunc(llvm_source_val, llvm_dest_type);
				}
			} else if (llvm_dest_type->isPointerTy()) {
				/* we want a pointer at the end... */
				if (llvm_source_type->isPointerTy()) {
					llvm_dest_val = builder.CreateBitCast(llvm_source_val, llvm_dest_type);
				} else {
					if (!llvm_source_type->isIntegerTy()) {
						log("source type for cast is %s (while casting to %s)",
								llvm_print(llvm_source_type).c_str(),
								type_cast->str().c_str());
						dbg();
					}
					llvm_dest_val = builder.CreateIntToPtr(llvm_source_val, llvm_dest_type);
				}
			} else {
				user_error(status, location, "invalid cast: cannot cast %s to %s",
						bound_var->type->str().c_str(),
						type_cast->str().c_str());
			}

			if (!!status) {
				return bound_var_t::create(INTERNAL_LOC(), "cast",
						bound_type, llvm_dest_val, make_iid("cast"));
			}
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
	resolved_value = resolved_value->resolve_bound_value(status, builder, scope);
	if (!!status) {
		indent_logger indent(callsite->get_location(), 4, string_format("getting typeid of %s",
					resolved_value->type->str().c_str()));
		auto program_scope = scope->get_program_scope();

		if (!!status) {
			bool is_managed = false;
			resolved_value->type->is_managed_ptr(
					status,
					builder,
					scope,
					is_managed);
			if (!!status) {
				bound_var_t::ref bound_managed_var = cast_bound_var(
						status,
						builder,
						scope,
						life,
						callsite->get_location(),
						resolved_value,
						type_ptr(type_id(make_iid("var_t"))));
				if (!!status) {
					auto name = string_format("typeid(%s)", resolved_value->str().c_str());

					if (is_managed) {
						bound_var_t::ref get_typeid_function = get_callable(
								status,
								builder,
								scope,
								"runtime.get_var_type_id",
								callsite->get_location(),
								type_args({bound_managed_var->type->get_type()}),
								type_variable(INTERNAL_LOC()));

						if (!!status) {
							assert(get_typeid_function != nullptr);
							return create_callsite(
									status,
									builder,
									scope,
									life,
									get_typeid_function,
									name,
									id->get_location(),
									{bound_managed_var});
						}
					} else {
						return bound_var_t::create(
								INTERNAL_LOC(),
								string_format("typeid(%s)", resolved_value->str().c_str()),
								program_scope->get_bound_type({TYPEID_TYPE}),
								llvm_create_int32(builder, atomize(resolved_value->type->get_type()->get_signature())),
								id);
					}
				}
			}
		}
	}

	assert(!status);
	return nullptr;
}


bound_var_t::ref ast::typeid_expr_t::resolve_expression(
		status_t &status,
	   	llvm::IRBuilder<> &builder,
	   	scope_t::ref scope,
		life_t::ref life,
		bool as_ref) const
{
	assert(!as_ref);

	auto resolved_value = expr->resolve_expression(status,
			builder,
			scope,
			life,
			false /*as_ref*/);

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
		life_t::ref life,
		bool as_ref) const
{
	assert(!as_ref);

	/* calculate the size of the object being referenced assume native types */
	bound_type_t::ref bound_type = upsert_bound_type(status, builder, scope, type);
	if (!!status) {
		bound_type_t::ref size_type = upsert_bound_type(status, builder, scope->get_program_scope(), type_id(make_iid("size_t")));
		if (!!status) {
			llvm::Value *llvm_size = llvm_sizeof_type(builder,
					llvm_deref_type(bound_type->get_llvm_specific_type()));

			return bound_var_t::create(
					INTERNAL_LOC(), type->str(), size_type, llvm_size,
					make_iid("sizeof"));
		}
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::function_defn_t::resolve_expression(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
		bool as_ref) const
{
	assert(!as_ref);

	return resolve_function(status, builder, scope, life, nullptr, nullptr);
}

void ast::function_defn_t::resolve_statement(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
		local_scope_t::ref *new_scope,
		bool *returns) const
{
	resolve_function(status, builder, scope, life, new_scope, returns);
}

bound_var_t::ref ast::function_defn_t::resolve_function(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref,
		local_scope_t::ref *new_scope,
		bool *returns) const
{
	// TODO: handle first-class functions by handling life for functions, and closures.
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
	INDENT(2, string_format(
				"type checking %s in %s", token.str().c_str(),
				scope->get_name().c_str()));

	/* see if we can get a monotype from the function declaration */
	bound_type_t::named_pairs args;
	bound_type_t::ref return_type;
	type_check_fully_bound_function_decl(status, builder, *decl, scope, args, return_type);

	if (!!status) {
		return instantiate_with_args_and_return_type(status, builder, scope, life,
				new_scope, args, return_type);
	} else {
		user_error(status, *this, "unable to instantiate function %s due to earlier errors",
				token.str().c_str());
	}

	assert(!status);
	return nullptr;
}

#define USER_MAIN_FN "user/main"

std::string switch_std_main(std::string name) {
    if (!getenv("NO_STD_LIB")) {
        if (name == "main") {
            return USER_MAIN_FN;
        } else if (name == "__main__") {
            return "main";
        }
    }
    return name;
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
	program_scope_t::ref program_scope = scope->get_program_scope();
	std::string function_name = switch_std_main(token.text);
	indent_logger indent(get_location(), 5, string_format(
				"instantiating function " c_id("%s"),
				function_name.c_str()));

	llvm::IRBuilderBase::InsertPointGuard ipg(builder);
	assert(!!status);
	assert(life->life_form == lf_function);
	assert(life->values.size() == 0);

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

		/* Create a user-defined function */
		llvm::Function *llvm_function = llvm::Function::Create(
				(llvm::FunctionType *)llvm_type,
				llvm::Function::ExternalLinkage, function_name,
				scope->get_llvm_module());

		// TODO: enable inlining for various functions
		// llvm_function->addFnAttr(llvm::Attribute::AlwaysInline);
		llvm_function->setGC(GC_STRATEGY);
		llvm_function->setDoesNotThrow();

		/* start emitting code into the new function. caller should have an
		 * insert point guard */
		llvm::BasicBlock *llvm_entry_block = llvm::BasicBlock::Create(builder.getContext(),
				"entry", llvm_function);
		llvm::BasicBlock *llvm_body_block = llvm::BasicBlock::Create(builder.getContext(),
				"body", llvm_function);

		builder.SetInsertPoint(llvm_entry_block);
		/* leave an empty entry block so that we can insert GC stuff in there, but be able to
		 * seek to the end of it and not get into business logic */
		builder.CreateBr(llvm_body_block);

		builder.SetInsertPoint(llvm_body_block);


		if (getenv("TRACE_FNS") != nullptr) {
			std::stringstream ss;
			ss << decl->token.location.str(true, true) << ": " << decl->str() << " : " << bound_function_type->str();
			auto callsite_debug_function_name_print = expand_callsite_string_literal(
					token,
					"posix",
					"puts",
					ss.str());
			callsite_debug_function_name_print->resolve_statement(status, builder, scope, life, nullptr, nullptr);
		}

		/* set up the mapping to this function for use in recursion */
		bound_var_t::ref function_var = bound_var_t::create(
				INTERNAL_LOC(), token.text, bound_function_type, llvm_function,
				make_code_id(token));

		/* we should be able to check its block as a callsite. note that this
		 * code will also run for generics but only after the
		 * sbk_generic_substitution mechanism has run its course. */
		auto params_scope = make_param_list_scope(status, builder, *decl, scope,
				life, function_var, args);

		/* now put this function declaration into the containing scope in case
		 * of indirect recursion */
		if (function_var->name.size() != 0) {
			/* inline function definitions are scoped to the virtual block in which
			 * they appear */
			if (auto local_scope = dyncast<local_scope_t>(scope)) {
				*new_scope = local_scope->new_local_scope(
						string_format("function-%s", function_name.c_str()));

				(*new_scope)->put_bound_variable(status, function_var->name, function_var);
			} else {
				module_scope_t::ref module_scope = dyncast<module_scope_t>(scope);

				if (module_scope == nullptr) {
					if (auto subst_scope = dyncast<generic_substitution_scope_t>(scope)) {
						module_scope = dyncast<module_scope_t>(subst_scope->get_parent_scope());
					}
				}

				if (module_scope != nullptr) {
					auto extends_module = decl->extends_module;
					if (extends_module) {
						auto name = extends_module->get_name();
						if (name == GLOBAL_SCOPE_NAME) {
							program_scope->put_bound_variable(status, function_var->name, function_var);
						} else if (auto injection_module_scope = program_scope->lookup_module(name)) {
							/* we're injecting this function into some other scope */
							injection_module_scope->put_bound_variable(status, function_var->name, function_var);
						} else {
							assert(false);
						}
					} else {
						/* before recursing directly or indirectly, let's just add
						 * this function to the module scope we're in */
						module_scope->put_bound_variable(status, function_var->name, function_var);
					}

					if (!!status) {
						program_scope->mark_checked(status, builder, shared_from_this());
						assert(!!status);
					}
				}
			}
		} else {
			user_error(status, *this, "function definitions need names");
		}

		if (!!status) {
			/* keep track of whether this function returns */
			bool all_paths_return = false;
			debug_above(7, log("setting return_type_constraint in %s to %s", function_var->name.c_str(),
						return_type->str().c_str()));
			params_scope->return_type_constraint = return_type;

			block->resolve_statement(status, builder, params_scope, life,
					nullptr, &all_paths_return);

			if (!!status) {
				debug_above(10, log(log_info, "module dump from %s\n%s",
							__PRETTY_FUNCTION__,
							llvm_print_module(*llvm_get_module(builder)).c_str()));

				if (all_paths_return) {
					llvm_verify_function(status, token.location, llvm_function);
					if (!!status) {
						return function_var;
					}
				} else {
					/* not all control paths return */
					if (return_type->is_void()) {
						/* if this is a void let's give the user a break and insert
						 * a default void return */

						life->release_vars(status, builder, scope, lf_function);
						if (!!status) {
							builder.CreateRetVoid();
							llvm_verify_function(status, token.location, llvm_function);
							if (!!status) {
								return function_var;
							}
						}
					} else {
						/* no breaks here, we don't know what to return */
						user_error(status, *this, "not all control paths return a value");
					}
				}
			} else {
				user_error(status, get_location(), "while checking %s", function_var->str().c_str());
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
		INDENT(3, string_format("resolving links in " c_module("%s"),
					obj.module_key.c_str()));

		/* get module level scope variable */
		module_scope_t::ref scope = compiler.get_module_scope(obj.module_key);

		for (const ptr<ast::link_module_statement_t> &link : obj.linked_modules) {
			link->resolve_statement(status, builder, scope, nullptr, nullptr,
					nullptr);
		}

		if (!!status) {
			for (const ptr<ast::link_function_statement_t> &link : obj.linked_functions) {
				bound_var_t::ref link_value = link->resolve_expression(
						status, builder, scope, nullptr, false /*as_ref*/);

				if (!!status) {
					if (link->extern_function->token.text.size() != 0) {
						scope->put_bound_variable(status, link->extern_function->token.text, link_value);
					} else {
						user_error(status, *link, "module level link definitions need names");
					}
				}
			}
		}

		if (!!status) {
			for (const ptr<ast::link_var_statement_t> &link : obj.linked_vars) {
				bound_var_t::ref link_value = link->resolve_expression(
						status, builder, scope, nullptr, false /*as_ref*/);

				if (!!status) {
					scope->put_bound_variable(status, link->var_decl->get_symbol(), link_value);
				}
			}
		}
	}
}

void type_check_module_vars(
		status_t &status,
        compiler_t &compiler,
		llvm::IRBuilder<> &builder,
		const ast::module_t &obj,
		scope_t::ref program_scope,
		std::vector<bound_var_t::ref> &global_vars)
{
	indent_logger indent(obj.get_location(), 2, string_format("resolving module variables in " c_module("%s"),
				obj.module_key.c_str()));

	/* get module level scope variable */
	module_scope_t::ref module_scope = compiler.get_module_scope(obj.module_key);
	if (!!status) {
		for (auto &var_decl : obj.var_decls) {
			INDENT(3, string_format("resolving module var " c_id("%s") " in " c_module("%s"),
						module_scope->make_fqn(var_decl->token.text).c_str(),
						obj.module_key.c_str()));

			/* the idea here is to put this variable into module scope,
			 * available globally, but to initialize it in the
			 * __init_module_vars function */
			global_vars.push_back(
					type_check_module_var_decl(status, builder, module_scope, *var_decl));
		}
	}
}

void resolve_unchecked_type(
		status_t &status,
		llvm::IRBuilder<> &builder,
		module_scope_t::ref module_scope,
		unchecked_type_t::ref unchecked_type)
{
	auto program_scope = module_scope->get_program_scope();
	auto node = unchecked_type->node;

	/* prevent recurring checks */
	if (!program_scope->has_checked(node)) {
		program_scope->mark_checked(status, builder, node);
		assert(!dyncast<const ast::function_defn_t>(node));

		debug_above(5, log(log_info, "checking module level type %s", node->token.str().c_str()));

		/* these next lines create type definitions, regardless of
		 * their genericity.  type expressions will be added as
		 * environment variables in the type system.  this step is
		 * MUTATING the type environment of the module, and the
		 * program. */
		if (auto type_def = dyncast<const ast::type_def_t>(node)) {
			type_def->resolve_statement(status, builder,
					module_scope, nullptr, nullptr, nullptr);
		} else if (auto tag = dyncast<const ast::tag_t>(node)) {
			tag->resolve_statement(status, builder, module_scope,
					nullptr, nullptr, nullptr);
		} else {
			panic("unhandled unchecked type node at module scope");
		}
	} else {
		debug_above(3, log(log_info, "skipping %s because it's already been checked", node->token.str().c_str()));
	}
}

void type_check_module_types(
		status_t &status,
		compiler_t &compiler,
		llvm::IRBuilder<> &builder,
		const ast::module_t &obj,
		scope_t::ref program_scope)
{
	if (!!status) {
		INDENT(2, string_format("type-checking types in module " c_module("%s"),
					obj.module_key.c_str()));

		/* get module level scope types */
		module_scope_t::ref module_scope = compiler.get_module_scope(obj.module_key);

		auto unchecked_types_ordered = module_scope->get_unchecked_types_ordered();
		for (unchecked_type_t::ref unchecked_type : unchecked_types_ordered) {
			resolve_unchecked_type(status, builder, module_scope, unchecked_type);
		}
	}
}

void type_check_program_variable(
		status_t &status,
		llvm::IRBuilder<> &builder,
		program_scope_t::ref program_scope,
		unchecked_var_t::ref unchecked_var)
{
	debug_above(8, log(log_info, "checking whether to check %s",
				unchecked_var->str().c_str()));

	auto node = unchecked_var->node;
	if (!program_scope->has_checked(node)) {
		/* prevent recurring checks */
		debug_above(7, log(log_info, "checking module level variable %s",
					node->token.str().c_str()));
		if (auto function_defn = dyncast<const ast::function_defn_t>(node)) {
			// TODO: decide whether we need treatment here
			status_t local_status;
			if (is_function_defn_generic(local_status, builder,
						unchecked_var->module_scope, *function_defn))
			{
				/* this is a generic function, or we've already checked
				 * it so let's skip checking it */
				status |= local_status;
				return;
			}
		}

		if (auto function_defn = dyncast<const ast::function_defn_t>(node)) {
			if (getenv("MAIN_ONLY") != nullptr && node->token.text != "__main__") {
				debug_above(8, log(log_info, "skipping %s because it's not '__main__'",
							node->str().c_str()));
				return;
			}
		}

		if (auto stmt = dyncast<const ast::statement_t>(node)) {
			status_t local_status;
			stmt->resolve_statement(
					local_status, builder, unchecked_var->module_scope,
					nullptr, nullptr, nullptr);
			status |= local_status;
		} else if (auto data_ctor = dyncast<const ast::type_product_t>(node)) {
			/* ignore until instantiation at a callsite */
		} else {
			panic("unhandled unchecked node at module scope");
		}
	} else {
		debug_above(3, log(log_info, "skipping %s because it's already been checked", node->token.str().c_str()));
	}
}

void type_check_program_variables(
		status_t &status,
        llvm::IRBuilder<> &builder,
        program_scope_t::ref program_scope)
{
	INDENT(2, string_format("resolving variables in program"));

	auto unchecked_vars_ordered = program_scope->get_unchecked_vars_ordered();
	for (auto unchecked_var : unchecked_vars_ordered) {
		status_t local_status;
		type_check_program_variable(status, builder, program_scope, unchecked_var);
		status |= local_status;
	}
}

void create_visit_module_vars_function(
		status_t &status,
	   	llvm::IRBuilder<> &builder,
	   	program_scope_t::ref program_scope,
		std::vector<bound_var_t::ref> global_vars)
{
	/* build the global __init_module_vars function */
	llvm::IRBuilderBase::InsertPointGuard ipg(builder);

	bound_type_t::ref bound_callback_fn_type = upsert_bound_type(
			status, builder, program_scope, 
			type_function(
				type_args({type_ptr(type_id(make_iid("var_t")))}, {}),
				type_id(make_iid("void"))));

	/* we are creating this function, but we'll be adding to it elsewhere */
	auto visit_module_vars_fn = llvm_start_function(
			status,
			builder, 
			program_scope,
			INTERNAL_LOC(),
			{bound_callback_fn_type},
			program_scope->get_bound_type({"void"}),
			"__visit_module_vars");

	if (!!status) {
		llvm::Function *llvm_function = llvm::dyn_cast<llvm::Function>(visit_module_vars_fn->get_llvm_value());
		assert(llvm_function != nullptr);
		assert(llvm_function->arg_size() == 1);

		if (!!status) {
			llvm::Value *llvm_visitor_fn = &(*llvm_function->arg_begin());
			auto user_visitor_fn = bound_var_t::create(
					INTERNAL_LOC(),
					"user_visitor_fn",
					bound_callback_fn_type,
					llvm_visitor_fn,
					make_iid("user_visitor_fn"));

			auto bound_var_ptr_type = program_scope->get_runtime_type(status, builder, "var_t", true /*get_ptr*/);

			if (!!status) {
				for (auto global_var : global_vars) {
					/* for each managed global_var, call the visitor function on it */
					bool is_managed;
					global_var->type->is_managed_ptr(status, builder, program_scope, is_managed);

					if (!!status) {
						if (is_managed) {
							llvm_create_call_inst(
									status,
									builder,
									INTERNAL_LOC(),
									user_visitor_fn,
									std::vector<llvm::Value *>{
										llvm_maybe_pointer_cast(
												builder,
												global_var->resolve_bound_var_value(builder),
												bound_var_ptr_type->get_llvm_type())});
						}
					}

					if (!status) {
						break;
					}
				}

				/* we're done with __visit_module_vars, let's make sure to return */
				builder.CreateRetVoid();

				if (!!status) {
					program_scope->put_bound_variable(status, "__visit_module_vars", visit_module_vars_fn);

					if (!!status) {
						return;
					}
				}
			}
		}
	}
	assert(!status);
	return;
}

void type_check_all_module_var_slots(
		status_t &status,
		compiler_t &compiler,
		llvm::IRBuilder<> &builder,
		const ast::program_t &obj,
		program_scope_t::ref program_scope)
{
	std::vector<bound_var_t::ref> global_vars;

	for (auto &module : obj.modules) {
		if (module->module_key == "runtime") {
			type_check_module_vars(status, compiler, builder, *module, program_scope,
					global_vars);
			break;
		}
	}

	/* initialized the module-level variable declarations. make sure that we initialize the
	 * runtime variables last. this will add them to the top of the __init_module_vars function. */
	if (!!status) {
		for (auto &module : obj.modules) {
			if (module->module_key != "runtime") {
				if (!!status) {
					type_check_module_vars(status, compiler, builder, *module, program_scope,
							global_vars);
				}
			}
		}
	}

	if (!!status) {
		create_visit_module_vars_function(status, builder, program_scope, global_vars);
	}
}

void type_check_program(
		status_t &status,
		llvm::IRBuilder<> &builder,
		const ast::program_t &obj,
		compiler_t &compiler)
{
	INDENT(2, string_format(
				"type-checking program %s",
				compiler.get_program_name().c_str()));

	ptr<program_scope_t> program_scope = compiler.get_program_scope();
	debug_above(11, log(log_info, "type_check_program program scope:\n%s", program_scope->str().c_str()));

	/* pass to resolve all module-level types */
	for (const ast::module_t::ref &module : obj.modules) {
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
	}

	if (!!status) {
		/* pass to resolve all module-level vars */
		type_check_all_module_var_slots(status, compiler, builder, obj, program_scope);
	}

	if (!!status) {
		assert(compiler.main_module != nullptr);

		/* pass to resolve all main module-level variables.  technically we only
		 * need to check the primary module, since that is the one that is expected
		 * to have the entry point ... at least for now... */
		type_check_program_variables(status, builder, program_scope);
	}
}

void ast::tag_t::resolve_statement(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
        local_scope_t::ref *new_scope,
		bool * /*returns*/) const
{
	indent_logger indent(get_location(), 5, string_format("resolving tag %s",
			str().c_str()));

	if (type_variables.size() != 0) {
		return;
	}

	std::string tag_name = token.text;
	std::string fqn_tag_name = scope->make_fqn(tag_name);
	auto qualified_id = make_iid_impl(fqn_tag_name, token.location);

	auto tag_type = type_id(qualified_id);

	/* it's a nullary enumeration or "tag", let's create a global value to
	 * represent this tag. */

	if (!!status) {
		auto var_ptr_type = scope->get_program_scope()->get_runtime_type(status, builder, "var_t", true /*get_ptr*/);
		if (!!status) {
			assert(var_ptr_type != nullptr);

			/* start by making a type for the tag */
			bound_type_t::ref bound_tag_type = bound_type_t::create(
					tag_type,
					token.location,
					/* all tags use the var_t* type */
					var_ptr_type->get_llvm_type());

			scope->put_structural_typename(status, tag_name, type_ptr(type_managed(type_struct({}, {}))));
			if (!!status) {
				scope->get_program_scope()->put_bound_type(status, bound_tag_type);
				if (!!status) {
					bound_var_t::ref tag = llvm_create_global_tag(
							status, builder, scope, bound_tag_type, fqn_tag_name,
							make_code_id(token));
					if (!!status) {
						/* record this tag variable for use later */
						scope->put_bound_variable(status, tag_name, tag);

						if (!!status) {
							debug_above(7, log(log_info, "instantiated nullary data ctor %s",
										tag->str().c_str()));
							return;
						}
					}
				}
			}
		}
	}

	assert(!status);
	return;
}

void ast::type_def_t::resolve_statement(
		status_t &status,
		llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
        local_scope_t::ref *new_scope,
		bool * /*returns*/) const
{
	/* the goal of this function is to
	 * construct a type, and its requisite parts - not limited to type
	 * definition - such as ctors, accessors, etc, and instantiate those
	 * components into the eligible scopes.  the current type we're defining
	 * should provide a definition that is defined in terms of fully qualified
	 * names.  the type will eventually be able to be referenced by its
	 * name. types can be imported across module boundaries, and type
	 * definitions can be generic in declaration, but concrete in resolution.
	 * this function is the declaration step. */

	std::string type_name = type_decl->token.text;
	auto already_bound_type = scope->get_bound_type(type_name);
	if (already_bound_type != nullptr) {
		debug_above(1, log(log_warning, "found predefined bound type for %s -> %s",
			   	type_decl->token.str().c_str(),
				already_bound_type->str().c_str()));
		
		// this is probably fine in practice, but maybe we should check whether
		// the already existing type was created in this scope
		dbg();
	}

	if (auto runnable_scope = dyncast<runnable_scope_t>(scope)) {
		assert(new_scope != nullptr);

		/* type definitions begin new scopes */
		local_scope_t::ref fresh_scope = runnable_scope->new_local_scope(
				string_format("type-%s", token.text.c_str()));

		/* update current scope for writing */
		scope = fresh_scope;

		/* have the caller update their current scope */
		*new_scope = fresh_scope;
	}

	type_algebra->register_type(status, builder,
			make_code_id(token), type_decl->type_variables, scope);

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
	assert(token.text == "=");

	if (auto array_index = dyncast<const ast::array_index_expr_t>(lhs)) {
		/* handle assignments into arrays */
		array_index->resolve_assignment(status, builder, scope, life, false /*as_ref*/, rhs);
		return;
	} else {
		auto lhs_var = lhs->resolve_expression(status, builder, scope, life, true /*as_ref*/);
		if (!!status) {
			auto rhs_var = rhs->resolve_expression(status, builder, scope, life, false /*as_ref*/);
			type_check_assignment(status, builder, scope, life, lhs_var,
					rhs_var, token.location);
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

bound_var_t::ref type_check_binary_op_assignment(
		status_t &status,
	   	llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		ast::item_t::ref op_node,
		ast::expression_t::ref lhs,
		ast::expression_t::ref rhs,
		location_t location,
		std::string function_name)
{
	auto lhs_var = lhs->resolve_expression(status, builder, scope, life, true /*as_ref*/);
	if (!!status) {
		bound_var_t::ref lhs_val = lhs_var->resolve_bound_value(status, builder, scope);

		if (!!status) {
			bound_var_t::ref rhs_var = rhs->resolve_expression(status, builder, scope, life, false /*as_ref*/);

			if (!!status) {
				assert(!rhs_var->is_ref());
				auto computed_var = type_check_binary_operator(status, builder, scope, life, lhs, lhs_val, rhs, rhs_var, op_node,
						function_name, nullptr, nullptr);

				if (!!status) {
					return type_check_assignment(status, builder, scope, life, lhs_var,
							computed_var, location);
				}
			}
		}
	}

	assert(!status);
	return nullptr;
}

void ast::mod_assignment_t::resolve_statement(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
	type_check_binary_op_assignment(status, builder, scope, life,
			shared_from_this(), lhs, rhs, token.location, "__mod__");
}

void ast::plus_assignment_t::resolve_statement(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
	type_check_binary_op_assignment(status, builder, scope, life,
			shared_from_this(), lhs, rhs, token.location, "__plus__");
}

void ast::minus_assignment_t::resolve_statement(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
	type_check_binary_op_assignment(status, builder, scope, life,
			shared_from_this(), lhs, rhs, token.location, "__minus__");
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

	/* let's figure out if we have a return value, and what it's type is */
    bound_var_t::ref return_value;
    bound_type_t::ref return_type;

	runnable_scope_t::ref runnable_scope = dyncast<runnable_scope_t>(scope);
	assert(runnable_scope != nullptr);

	auto return_type_constraint = runnable_scope->get_return_type_constraint();

    if (expr != nullptr) {
        /* if there is a return expression resolve it into a value. also, be
		 * sure to retain whether the function signature necessitates a ref type */
		return_value = expr->resolve_expression(status, builder, scope, life,
				return_type_constraint ? return_type_constraint->is_ref() : false /*as_ref*/);

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

		/* make sure this return type makes sense, or keep track of it if we
		 * didn't yet know the return type for this function */
		runnable_scope->check_or_update_return_type_constraint(status,
				shared_from_this(), return_type);

		if (return_value != nullptr) {
			if (return_value->type->is_void()) {
				user_error(status, get_location(),
						"return expressions cannot be " c_type("void") ". use an empty return statement to return from this function");
			} else {
				auto llvm_return_value = llvm_maybe_pointer_cast(builder,
						return_value->resolve_bound_var_value(builder),
						runnable_scope->get_return_type_constraint());

				if (llvm_return_value->getName().str().size() == 0) {
					llvm_return_value->setName("return.value");
				}
				debug_above(8, log("emitting a return of %s", llvm_print(llvm_return_value).c_str()));

				// TODO: release live variables in scope, except the one being
				// returned
				builder.CreateRet(llvm_return_value);
			}
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

void ast::times_assignment_t::resolve_statement(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
	type_check_binary_op_assignment(status, builder, scope, life,
			shared_from_this(), lhs, rhs, token.location, "__times__");
}

void ast::divide_assignment_t::resolve_statement(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
	type_check_binary_op_assignment(status, builder, scope, life,
			shared_from_this(), lhs, rhs, token.location, "__divide__");
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
			user_error(status, *statement, "this statement will never run");
			break;
		}

		local_scope_t::ref next_scope;

		debug_above(9, log(log_info, "type checking statement\n%s", statement->str().c_str()));

		/* create a new life for tracking the rhs values (temp values) in this statement */
		auto stmt_life = life->new_life(status, lf_statement);

		{
			indent_logger indent(statement->get_location(), 5, string_format("while checking statement %s",
						statement->str().c_str()));

			if (getenv("TRACE_STATEMENTS") != nullptr) {
				std::stringstream ss;
				ss << statement->token.location.str(true, true) << ": " << statement->str();
				auto callsite_debug_function_name_print = expand_callsite_string_literal(
						token,
						"posix",
						"puts",
						ss.str());
				callsite_debug_function_name_print->resolve_statement(status, builder, scope, life, nullptr, nullptr);
			}
			/* resolve the statement */
			statement->resolve_statement(status, builder, current_scope, stmt_life,
					&next_scope, returns);
		}

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
				user_error(status, statement->get_location(), "while checking statement");
			}
			break;
		}
    }

	if (!*returns) {
		/* if the block ensured that all code paths returned, then the lifetimes
		 * of the related objects was managed. otherwise, let's do it here. */
		life->release_vars(status, builder, scope, lf_block);
	}

    return;
}

struct for_like_var_decl_t : public ast::like_var_decl_t {
	std::string symbol;
	location_t location;
	const ast::expression_t &collection;
	types::type_t::ref type;

	for_like_var_decl_t(std::string symbol, location_t location, const ast::expression_t &collection) :
		symbol(symbol), location(location), collection(collection), type(::type_variable(location))
	{
	}

	virtual std::string get_symbol() const {
		return symbol;
	}

	virtual location_t get_location() const {
		return location;
	}

	virtual types::type_t::ref get_type() const {
		return type;
	}

	virtual bool has_initializer() const {
		return true;
	}

	virtual bound_var_t::ref resolve_initializer(
			status_t &status,
			llvm::IRBuilder<> &builder,
			scope_t::ref scope,
			life_t::ref life) const
	{
		return null_impl();
	}
};

void ast::for_block_t::resolve_statement(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		local_scope_t::ref *new_scope,
		bool *returns) const
{
	/* this next bit is some code-gen to rewrite the for loop as a while loop */

	token_t iterable_token = token_t(var_token.location, tk_identifier, types::gensym()->get_name());
	token_t iterator_token = token_t(var_token.location, tk_identifier, types::gensym()->get_name());
	token_t iterator_end_token = token_t(var_token.location, tk_identifier, types::gensym()->get_name());
	token_t iteration_value_token = var_token;

	/* create the iteratable object by referencing the user-supplied iterable */
	auto iterable_var_decl = create<var_decl_t>(iterable_token);
	iterable_var_decl->type = type_variable(var_token.location);
	iterable_var_decl->initializer = iterable;

	/* create the iterator from the iterable */
	auto iterator_var_decl = create<var_decl_t>(iterator_token);
	iterator_var_decl->type = type_variable(iterator_token.location);

	/* create the call to begin iteration */
	auto iter_begin_call = create<callsite_expr_t>(in_token);
	iter_begin_call->function_expr = create<reference_expr_t>(token_t(in_token.location, tk_identifier, "__iter_begin__"));
	iter_begin_call->params.push_back(create<reference_expr_t>(iterable_token));

	/* finish creating the iterator_var_decl */
	iterator_var_decl->initializer = iter_begin_call;

	/* create the end iterator from the iterable */
	auto iterator_end_var_decl = create<var_decl_t>(iterator_end_token);
	iterator_end_var_decl->type = type_variable(iterator_end_token.location);

	/* create the call to end iteration */
	auto iter_end_call = create<callsite_expr_t>(in_token);
	iter_end_call->function_expr = create<reference_expr_t>(token_t(in_token.location, tk_identifier, "__iter_end__"));
	iter_end_call->params.push_back(create<reference_expr_t>(iterable_token));

	/* finish creating the iterator_end_var_decl */
	iterator_end_var_decl->initializer = iter_end_call;
	iterator_end_var_decl->is_let_var = true;

	/* create the while condition */
	auto iter_valid_call = create<callsite_expr_t>(in_token);
	iter_valid_call->function_expr = create<reference_expr_t>(token_t(in_token.location, tk_identifier, "__iter_valid__"));
	iter_valid_call->params.push_back(create<reference_expr_t>(iterator_token));
	iter_valid_call->params.push_back(create<reference_expr_t>(iterator_end_token));

	/* create the iterate call to advance iteration */
	auto iterate_call = create<callsite_expr_t>(in_token);
	iterate_call->function_expr = create<reference_expr_t>(token_t(in_token.location, tk_identifier, "__iterate__"));
	iterate_call->params.push_back(create<reference_expr_t>(iterator_token));

	/* create the end iterator from the iterable */
	auto value_var_decl = create<var_decl_t>(var_token);
	value_var_decl->type = type_variable(var_token.location);

	/* create the call to get the item from the iterator */
	auto iter_item_call = create<callsite_expr_t>(var_token);
	iter_item_call->function_expr = create<reference_expr_t>(token_t(var_token.location, tk_identifier, "__iter_item__"));
	iter_item_call->params.push_back(create<reference_expr_t>(iterator_token));

	/* finish creating the iterator_end_var_decl */
	value_var_decl->initializer = iter_item_call;

	/* create the while loop */
	auto while_block = create<while_block_t>(token);
	while_block->condition = iter_valid_call;
	while_block->block = create<block_t>(block->token);
	while_block->block->statements.push_back(value_var_decl);
	for (auto statement : block->statements) {
		while_block->block->statements.push_back(statement);
	}
	while_block->block->statements.push_back(iterate_call);

	/* finally, type_check all of the generated code */
	auto block = create<block_t>(token);
	block->statements.push_back(iterable_var_decl);
	block->statements.push_back(iterator_var_decl);
	block->statements.push_back(iterator_end_var_decl);
	block->statements.push_back(while_block);
	debug_above(7, log("for loop %s generated new code\n%s",
				str().c_str(), block->str().c_str()));

	/* dispatch type checking and code gen to this newly generated ast */
	return block->resolve_statement(status, builder, scope,
			life, new_scope, returns);
}

bound_var_t::ref ast::expression_t::resolve_condition(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref block_scope,
		life_t::ref life,
		local_scope_t::ref *,
		local_scope_t::ref *) const
{
	return resolve_expression(status, builder, block_scope, life, false /*as_ref*/);
}

void ast::while_block_t::resolve_statement(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		local_scope_t::ref *,
		bool *returns) const
{
	/* while scope allows us to set up new variables inside while conditions */
	local_scope_t::ref while_scope;

	assert(token.text == "while" || token.text == "for");

	llvm::Function *llvm_function_current = llvm_get_function(builder);

	llvm::BasicBlock *while_cond_bb = llvm::BasicBlock::Create(builder.getContext(), "while.cond", llvm_function_current);

	assert(!builder.GetInsertBlock()->getTerminator());
	builder.CreateBr(while_cond_bb);
	builder.SetInsertPoint(while_cond_bb);

	/* demarcate a loop boundary here */
	life = life->new_life(status, lf_loop);

	life_t::ref cond_life = life->new_life(status, lf_statement);
	bound_var_t::ref condition_value;

	/* evaluate the condition for branching */
	/* our user is attempting any assorted collection of ergonomic improvements to their life by
	 * asserting possible type modifications to their variables, or by injecting new variables
	 * into the nested scope. */
	condition_value = condition->resolve_condition(
			status, builder, scope, cond_life, &while_scope, nullptr /*scope_if_false*/);

	if (!!status) {
		/* generate some new blocks */
		llvm::BasicBlock *while_block_bb = llvm::BasicBlock::Create(builder.getContext(), "while.block", llvm_function_current);
		llvm::BasicBlock *while_end_bb = llvm::BasicBlock::Create(builder.getContext(), "while.end");

		/* keep track of the "break" and "continue" jump locations */
		loop_tracker_t loop_tracker(dyncast<runnable_scope_t>(scope), while_cond_bb, while_end_bb);

		/* we don't have an else block, so we can just continue on */
		llvm_create_if_branch(status, builder, scope,
				IFF_ELSE, cond_life, condition->get_location(), condition_value,
				while_block_bb, while_end_bb);

		if (!!status) {
			assert(builder.GetInsertBlock()->getTerminator());

			/* let's generate code for the "then" block */
			builder.SetInsertPoint(while_block_bb);
			assert(!builder.GetInsertBlock()->getTerminator());

			cond_life->release_vars(status, builder, scope, lf_statement);

			if (!!status) {
				block->resolve_statement(status, builder,
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
					return;
				}
			}
		}
	}

	assert(!status);
	return;
}

void ast::if_block_t::resolve_statement(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
        local_scope_t::ref *,
		bool *returns) const
{
	assert(life->life_form == lf_statement);

	/* if scope allows us to set up new variables inside if conditions */
	local_scope_t::ref scope_if_true, scope_if_false;

	bool if_block_returns = false, else_block_returns = false;

	assert(token.text == "if" || token.text == "elif" || token.text == "assert");
	bound_var_t::ref condition_value;

	auto cond_life = life->new_life(status, lf_statement);

	/* evaluate the condition for branching */
	condition_value = condition->resolve_condition(
			status, builder, scope, cond_life, &scope_if_true, &scope_if_false);

	/*
	 * var maybe_vector Vector? = maybe_a_vector()
	 *
	 * if v := maybe_vector
	 *   print("x-value is " + v.x)
	 * else
	 *   print("no x-value available")
	 *
	 * if null is a subtype of maybe_vector, then the above code
	 * effectively becomes:
	 *
	 * if __not_null__(maybe_vector)
	 *   v := __discard_null__(maybe_vector)
	 *   // if there is a __bool__ function defined for type(v), add another
	 *   // if statement:
	 *   if not v
	 *     goto l_else
	 *   print("x-axis is " + v.x)
	 * else
	 * l_else:
	 *   print("no x-value available")
	 *
	 * if null is not a subtype of maybe_vector, for example, for a Vector
	 * class...
	 */

	if (!!status) {
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
				condition->get_location(), condition_value, then_bb, else_bb);

		if (!!status) {
			builder.SetInsertPoint(else_bb);

			if (else_ != nullptr) {
				else_->resolve_statement(status, builder,
						scope_if_false ? scope_if_false : scope,
						life, nullptr, &else_block_returns);
				if (!status) {
					user_error(status, else_->get_location(), "while checking else statement");
				}
			}

			if (!!status) {
				if (!else_block_returns) {
					/* keep track of the fact that we have to have a
					 * merged block to land in after the else block */
					insert_merge_bb = true;

					/* go ahead and jump there */
					if (!builder.GetInsertBlock()->getTerminator()) {
						builder.CreateBr(merge_bb);
					}
				}

				/* let's generate code for the "then" block */
				builder.SetInsertPoint(then_bb);
				cond_life->release_vars(status, builder, scope, lf_statement);

				if (!!status) {
					block->resolve_statement(status, builder,
							scope_if_true ? scope_if_true : scope, life, nullptr, &if_block_returns);

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

	assert(!status);
	return;
}

bound_var_t::ref ast::bang_expr_t::resolve_expression(
		status_t &status,
	   	llvm::IRBuilder<> &builder,
	   	scope_t::ref scope,
		life_t::ref life,
		bool as_ref) const
{
	assert(!as_ref);

	auto lhs_value = lhs->resolve_expression(status, builder, scope, life,
			false /*as_ref*/);

	if (!!status) {
		auto type = lhs_value->type->get_type();
		auto maybe_type = dyncast<const types::type_maybe_t>(type);
		if (maybe_type != nullptr) {
			bound_type_t::ref just_bound_type = upsert_bound_type(status,
					builder, scope, maybe_type->just);

			return bound_var_t::create(INTERNAL_LOC(), lhs_value->name,
					just_bound_type,
					lhs_value->get_llvm_value(),
					lhs_value->id);
		} else {
			user_error(status, *this, "bang expression is unnecessary since this is not a 'maybe' type: %s",
					type->str().c_str());
		}
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::var_decl_t::resolve_as_link(
		status_t &status,
		llvm::IRBuilder<> &builder,
		module_scope_t::ref module_scope)
{
	if (initializer != nullptr) {
		user_error(status, get_location(), "linked variables cannot have initializers");
	}

	if (!!status) {
		types::type_t::ref declared_type = get_type()->rebind(module_scope->get_type_variable_bindings());
		bound_type_t::ref var_type = upsert_bound_type(status, builder, module_scope, declared_type);
		bound_type_t::ref ref_var_type = upsert_bound_type(status, builder, module_scope, type_ref(declared_type));
		llvm::Module *llvm_module = module_scope->get_llvm_module();
		auto llvm_global_variable = new llvm::GlobalVariable(*llvm_module,
				var_type->get_llvm_specific_type(),
				false /*is_constant*/, llvm::GlobalValue::ExternalLinkage,
				nullptr, token.text, nullptr,
				llvm::GlobalVariable::NotThreadLocal);
		return bound_var_t::create(
				INTERNAL_LOC(),
				token.text,
				ref_var_type,
				llvm_global_variable,
				make_code_id(token));
	}
	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::var_decl_t::resolve_condition(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		life_t::ref life,
		local_scope_t::ref *scope_if_true,
		local_scope_t::ref *scope_if_false) const
{
    runnable_scope_t::ref runnable_scope = dyncast<runnable_scope_t>(scope);
    assert(runnable_scope);

    /* variable declarations begin new scopes */
    local_scope_t::ref fresh_scope = runnable_scope->new_local_scope(
            string_format("condition-assignment-%s", token.text.c_str()));

    scope = fresh_scope;

    /* check to make sure this var decl is sound */
    bound_var_t::ref var_decl_value = type_check_bound_var_decl(
            status, builder, fresh_scope, *this, life, true /*maybe_unbox*/);

	if (!!status) {
		*scope_if_true = fresh_scope;
		return var_decl_value;
	}

	assert(!status);
	return nullptr;
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

void ast::pass_flow_t::resolve_statement(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
        local_scope_t::ref *new_scope,
		bool *returns) const
{
    return;
}

bound_var_t::ref take_address(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
		ast::expression_t::ref expr)
{
    /* first solve the right hand side */
	bound_var_t::ref rhs_var = expr->resolve_expression(status, builder,
			scope, life, true /*as_ref*/);

	if (!!status) {
		if (auto ref_type = dyncast<const types::type_ref_t>(rhs_var->type->get_type())) {
			bound_type_t::ref bound_ptr_type = upsert_bound_type(status, builder, scope, type_ptr(ref_type->element_type));
			if (!!status) {
				return bound_var_t::create(
						expr->get_location(), string_format("address_of.%s", rhs_var->name.c_str()),
						bound_ptr_type, rhs_var->get_llvm_value(),
						make_code_id(expr->token));
			}
		} else {
			user_error(status, expr->get_location(), "can't take address of %s", expr->str().c_str());
		}
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::prefix_expr_t::resolve_expression(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
		bool as_ref) const
{
	std::string function_name;
	switch (token.tk) {
	case tk_minus:
		function_name = "__negative__";
		break;
	case tk_plus:
		function_name = "__positive__";
		break;
	case tk_ampersand:
		return take_address(status, builder, scope, life, rhs);
	case tk_identifier:
		if (token.is_ident(K(not))) {
			function_name = "__not__";
			break;
		}
	default:
		return null_impl();
	}

    /* first solve the right hand side */
	bound_var_t::ref rhs_var = rhs->resolve_expression(status, builder,
			scope, life, false /*as_ref*/);

    if (!!status) {
		if (function_name == "__not__") {
			bool is_managed;
			rhs_var->type->is_managed_ptr(status, builder, scope, is_managed);
			if (!!status) {
				if (!is_managed) {
					return resolve_null_check(status, builder, scope, life,
							get_location(), rhs, rhs_var, nck_is_null, nullptr, nullptr);
				} else {
					/* TODO: revisit whether managed types must/can override __not__? */
				}
			}
		}
		if (!!status) {
			return call_program_function(status, builder, scope, life,
					function_name, shared_from_this(), {rhs_var});
		}
	}
	assert(!status);
	return nullptr;
}

bound_var_t::ref ast::literal_expr_t::resolve_expression(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
		bool as_ref) const
{
    scope_t::ref program_scope = scope->get_program_scope();

    switch (token.tk) {
	case tk_identifier:
		{
			assert(token.text == "null");
			auto null_type = program_scope->get_bound_type({"null"});
			auto bound_type = bound_type_t::create(
					type_null(),
					token.location,
					null_type->get_llvm_type(),
					null_type->get_llvm_specific_type());
			return bound_var_t::create(
					INTERNAL_LOC(), "null", bound_type,
					llvm::Constant::getNullValue(null_type->get_llvm_specific_type()),
					make_code_id(token));
		}
		break;
	case tk_raw_integer:
        {
			/* create a native integer */
            int64_t value = parse_int_value(status, token);
			if (!!status) {
				bound_type_t::ref native_type = program_scope->get_bound_type({INT_TYPE});
				return bound_var_t::create(
						INTERNAL_LOC(), "raw_int_literal", native_type,
						llvm_create_int(builder, value),
						make_code_id(token));
			}
        }
		break;
    case tk_integer:
        {
			/* create a boxed integer */
            int64_t value = parse_int_value(status, token);
			if (!!status) {
				bound_type_t::ref native_type = program_scope->get_bound_type({INT_TYPE});
				bound_type_t::ref boxed_type = upsert_bound_type(
						status,
						builder,
						scope,
						type_id(make_iid("int")));
				if (!!status) {
					assert(boxed_type != nullptr);
					bound_var_t::ref box_int = get_callable(
							status,
							builder,
							scope,
							{"int"},
							get_location(),
							get_args_type({native_type}),
							nullptr);

					if (!!status) {
						assert(box_int != nullptr);
						return create_callsite(
								status,
								builder,
								scope,
								life,
								box_int,
								{string_format("literal int (%d)", value)},
								get_location(),
								{bound_var_t::create(
										INTERNAL_LOC(), "temp_int_literal", boxed_type,
										llvm_create_int(builder, value),
										make_code_id(token))});
					}
				}
			}
        }
		break;
    case tk_raw_string:
		{
			std::string value = unescape_json_quotes(token.text.substr(0, token.text.size() - 1));
			bound_type_t::ref native_type = program_scope->get_bound_type({MBS_TYPE});
			return bound_var_t::create(
					INTERNAL_LOC(), "raw_str_literal", native_type,
					llvm_create_global_string(builder, value),
					make_code_id(token));
		}
		break;
    case tk_string:
		{
			std::string value = unescape_json_quotes(token.text);
			bound_type_t::ref native_type = program_scope->get_bound_type({MBS_TYPE});
			bound_type_t::ref boxed_type = upsert_bound_type(
					status,
					builder,
					scope,
					type_id(make_iid("str")));

			if (!!status) {
				assert(boxed_type != nullptr);
				bound_var_t::ref box_str = get_callable(
						status,
						builder,
						scope,
						{"__box__"},
						get_location(),
						get_args_type({native_type}),
						nullptr);

				if (!!status) {
					return create_callsite(
							status,
							builder,
							scope,
							life,
							box_str,
							{string_format("literal str (%s)", value.c_str())},
							get_location(),
							{bound_var_t::create(
									INTERNAL_LOC(), "temp_str_literal", boxed_type,
									llvm_create_global_string(builder, value),
									make_code_id(token))});
				}
			}
		}
		break;
	case tk_raw_float:
		{
			double value = atof(token.text.substr(0, token.text.size() - 1).c_str());
			bound_type_t::ref native_type = program_scope->get_bound_type({FLOAT_TYPE});
			return bound_var_t::create(
					INTERNAL_LOC(), "raw_float_literal", native_type,
					llvm_create_double(builder, value),
					make_code_id(token));
		}
		break;
	case tk_float:
		{
			double value = atof(token.text.c_str());
			bound_type_t::ref native_type = program_scope->get_bound_type({FLOAT_TYPE});
			bound_type_t::ref boxed_type = upsert_bound_type(
					status,
					builder,
					scope,
					type_id(make_iid("float")));
			if (!!status) {
				assert(boxed_type != nullptr);
				bound_var_t::ref box_float = get_callable(
						status,
						builder,
						scope,
						{"float"},
						get_location(),
						get_args_type({native_type}),
						nullptr);

				if (!!status) {
					return create_callsite(
							status,
							builder,
							scope,
							life,
							box_float,
							{string_format("literal float (%f)", value)},
							get_location(),
							{bound_var_t::create(
									INTERNAL_LOC(), "temp_float_literal", boxed_type,
									llvm_create_double(builder, value),
									make_code_id(token))});
				}
			}
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
	/* ok, we know we've got some variable here */
	auto bound_var = get_callable(status, builder, scope, token.text,
			get_location(), get_args_type(args), nullptr);
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
		life_t::ref life,
		bool as_ref) const
{
	bound_var_t::ref bound_var = lhs->resolve_expression(status, builder, scope, life, false /*as_ref*/);
	if (!!status) {
		debug_above(6, log("cast expression is casting to %s", type_cast->str().c_str()));

		return cast_bound_var(status, builder, scope, life, get_location(), bound_var, type_cast);
	}

	assert(!status);
	return nullptr;
}

void dump_builder(llvm::IRBuilder<> &builder) {
	std::cerr << llvm_print_function(llvm_get_function(builder)) << std::endl;
}
