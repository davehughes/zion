#include "callable.h"
#include "llvm_utils.h"
#include "type_checker.h"
#include "ast.h"
#include "unification.h"
#include "llvm_types.h"
#include "types.h"
#include "type_instantiation.h"

bound_var_t::ref make_call_value(
		status_t &status,
		llvm::IRBuilder<> &builder,
		location_t location,
		scope_t::ref scope,
		life_t::ref life,
		bound_var_t::ref function,
		bound_var_t::refs arguments)
{
	return create_callsite(
			status, builder, scope, life, function,
			"temp_call_value", INTERNAL_LOC(), arguments);
}

bound_var_t::ref check_func_vs_callsite(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		location_t location,
		var_t::ref fn,
		types::type_t::ref type_fn_context,
		types::type_args_t::ref args)
{
	assert(!!status);
	assert(args->ftv_count() == 0 && "how did you get abstract arguments? are you a wizard?");
	unification_t unification = fn->accepts_callsite(builder, scope, type_fn_context, args);
	if (unification.result) {
		if (auto bound_fn = dyncast<const bound_var_t>(fn)) {
			/* this function has already been bound */
			debug_above(3, log(log_info, "override resolution has chosen %s",
						bound_fn->str().c_str()));
			return bound_fn;
		} else {
			panic("unhandled var type");
		}

		assert(!status);
		return nullptr;
	}

	debug_above(4, log(log_info, "fn %s at %s does not match %s because %s",
				fn->str().c_str(),
				location.str().c_str(), 
				args->str().c_str(),
				unification.str().c_str()));

	/* it's possible to exit without finding that the callable matches the
	 * callsite. this is not an error (unless the status indicates so.) */
	return nullptr;
}

bool function_exists_in(var_t::ref fn, std::list<bound_var_t::ref> callables) {
    for (auto callable : callables) {
        if (callable->get_location() == fn->get_location()) {
            return true;
        }
    }
    return false;
}

bound_var_t::ref maybe_get_callable(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		atom alias,
		location_t location,
		types::type_t::ref type_fn_context,
		types::type_args_t::ref args,
		var_t::refs &fns)
{
	debug_above(3, log(log_info, "maybe_get_callable(..., scope=%s, alias=%s, type_fn_context=%s, args=%s, ...)",
				scope->get_name().c_str(),
				alias.c_str(),
				type_fn_context->str().c_str(),
				args->str().c_str()));

    llvm::IRBuilderBase::InsertPointGuard ipg(builder);
    std::list<bound_var_t::ref> callables;
	if (!!status) {
		/* look through the current scope stack and get a callable that is able
		 * to be invoked with the given args */
		scope->get_callables(alias, fns);
		for (auto &fn : fns) {
            if (function_exists_in(fn, callables)) {
                /* we've already found a matching version of this function,
                 * let's not bind it again */
				debug_above(7, log(log_info,
							"skipping checking %s because we've already got a matched version of that function",
							fn->str().c_str()));
				continue;
            }
			bound_var_t::ref callable = check_func_vs_callsite(status, builder,
					scope, location, fn, type_fn_context, args);

			if (!status) {
				assert(callable == nullptr);
				return nullptr;
			} else if (callable != nullptr) {
				callables.push_front(callable);
			}
		}

        if (!!status) {
            if (callables.size() == 1) {
                return callables.front();
            } else if (callables.size() == 0) {
                return nullptr;
            } else {
				user_error(status, location,
						"multiple matching overloads found for %s",
						alias.c_str());
                for (auto callable :callables) {
                    user_message(log_info, status, callable->get_location(),
						   	"matching overload : %s",
                            callable->type->get_type()->str().c_str());
                }
            }
        }
	}
	return nullptr;
}

bound_var_t::ref get_callable(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		atom alias,
		const ptr<const ast::item_t> &callsite,
		types::type_t::ref outbound_context,
		types::type_args_t::ref args)
{
	var_t::refs fns;
	// TODO: potentially allow fake calling contexts by adding syntax to the
	// callsite
	auto callable = maybe_get_callable(status, builder, scope, alias,
			callsite->get_location(), outbound_context, args, fns);

	if (!!status) {
		if (callable != nullptr) {
			return callable;
		} else {
			if (fns.size() == 0) {
				user_error(status, *callsite, "no function found named " c_id("%s") " for callsite %s with %s in " c_id("%s"),
						alias.c_str(), callsite->str().c_str(),
						args->str().c_str(),
						scope->get_name().c_str());
				debug_above(11, log(log_info, "%s", scope->str().c_str()));
			} else {
				std::stringstream ss;
				ss << "unable to resolve overloads for " << C_ID << alias << C_RESET;
				ss << " at " << callsite->str() << args->str();
				ss << " from context " << outbound_context->str();
				user_error(status, *callsite, "%s", ss.str().c_str());

				if (debug_level() >= 0) {
					/* report on the places we tried to look for a match */
					for (auto &fn : fns) {
						ss.str("");
						ss << fn->get_type(scope)->str() << " did not match";
						user_message(log_info, status, fn->get_location(), "%s", ss.str().c_str());
					}
				}
			}
			return nullptr;
		}
	}

	assert(!status);
	return nullptr;
}

bound_var_t::ref call_program_function(
        status_t &status,
        llvm::IRBuilder<> &builder,
        scope_t::ref scope,
		life_t::ref life,
        atom function_name,
        const ptr<const ast::item_t> &callsite,
        const bound_var_t::refs var_args)
{
    types::type_args_t::ref args = get_args_type(var_args);
	auto program_scope = scope->get_program_scope();
    /* get or instantiate a function we can call on these arguments */
    bound_var_t::ref function = get_callable(
			status, builder, program_scope, function_name, callsite,
			program_scope->get_inbound_context(), args);

    if (!!status) {
		return make_call_value(status, builder, callsite->get_location(), scope,
				life, function, var_args);
    } else {
		user_error(status, callsite->get_location(), "failed to resolve function with args: %s",
				::str(var_args).c_str());

		assert(!status);
		return nullptr;
	}
}

