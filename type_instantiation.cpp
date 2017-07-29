#include "zion.h"
#include <iostream>
#include <numeric>
#include <list>
#include "type_instantiation.h"
#include "bound_var.h"
#include "scopes.h"
#include "ast.h"
#include "llvm_types.h"
#include "llvm_utils.h"
#include "phase_scope_setup.h"
#include "types.h"
#include "code_id.h"

/* When we encounter the Empty declaration, we have to instantiate something.
 * When we create Empty() with term __obj__{__tuple__}. We don't bother
 * associating anything with the base type. We also create a bound type with
 * term 'Empty' that just maps to the raw __obj__{__tuple__} one.
 *
 * When we encounter Just, we create an unchecked data ctor, which would look
 * like:
 *
 *     def Just(any X) Just{any X}
 *
 * if it needed to have an AST. And, importantly, we do not create a type for
 * Just yet because it's not fully bound.
 * 
 * When we encounter a bound instance of the base type, like:
 * 
 *     var m Maybe{int} = ...
 *
 * we instantiate all the data ctors that are not yet instantiated.
 *
 * In the case of self-references like:
 *
 * type IntList is Node(int, IntList) or Done
 *
 * We notice that the base type is not parameterized. So, we immediately create
 * the base sum type IntList that maps to term __or__{Node{int, IntList},
 * Done} where the LLVM representation of this is just a raw var_t pointer that
 * can later be upcast, based on pattern matching on the type_id.
 */

bound_var_t::ref bind_ctor_to_scope(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope,
		identifier::ref id,
		ast::item_t::ref node,
		types::type_function_t::ref function)
{
	assert(!!status);
	assert(id != nullptr);
	assert(function != nullptr);

	/* create or find an existing ctor function that satisfies the term of
	 * this node */
	debug_above(5, log(log_info, "finding/creating data ctor for " c_type("%s") " with return type %s",
			id->str().c_str(), function->return_type->str().c_str()));

	debug_above(5, log(log_info, "function return_type %s expands to %s",
				function->return_type->str().c_str(),
				eval(function->return_type, scope->get_typename_env())->str().c_str()));

	bound_type_t::refs args = upsert_bound_types(status, builder, scope,
			function->args->args);

	if (!!status) {
		/* now that we know the parameter types, let's see what the term looks
		 * like */
		debug_above(5, log(log_info, "ctor type should be %s",
					function->str().c_str()));

		if (function->return_type != nullptr) {
			/* now we know the type of the ctor we want to create. let's check
			 * whether this ctor already exists. if so, we'll just return it. if
			 * not, we'll generate it. */
			auto tuple_pair = instantiate_tagged_tuple_ctor(status, builder,
					scope, id, node, function->return_type);

			if (!!status) {
				debug_above(5, log(log_info, "created a ctor %s", tuple_pair.first->str().c_str()));
				return tuple_pair.first;
			}
		} else {
			user_error(status, node->get_location(),
				   	"constructor is not returning a product type: %s",
					function->str().c_str());
		}
	}

	assert(!status);
	return nullptr;
}

types::type_t::ref instantiate_data_ctor_type(
		status_t &status,
		llvm::IRBuilder<> &builder,
		types::type_t::ref unbound_type,
		scope_t::ref scope,
		ptr<const ast::item_t> node,
		identifier::ref id)
{
	/* get the name of the ctor */
	std::string tag_name = id->get_name();
	std::string fqn_tag_name = scope->make_fqn(tag_name);
	auto qualified_id = make_iid_impl(fqn_tag_name, id->get_location());

	/* create the tag type */
	auto tag_type = type_id(qualified_id);

	/* create the basic struct type */
	ptr<const types::type_struct_t> struct_ = dyncast<const types::type_struct_t>(unbound_type);
	assert(struct_ != nullptr);

	/* now build the actual typename expansion we'll put in the typename env */
	/**********************************************/
	/* Register a data ctor for this struct_ type */
	/**********************************************/
	assert(!!status);

	if (struct_->dimensions.size() != 0) {
		assert(id->get_name() == tag_name);

		/* we're declaring a ctor at module scope */
		if (auto module_scope = dyncast<module_scope_t>(scope)) {

			/* create the actual expanded type signature of this type */
			types::type_t::ref type = type_managed_ptr(struct_);
			auto ctor_return_type = tag_type;

			/* for now assume all ctors return managed ptrs */
			debug_above(4, log(log_info, "return type for %s will be %s",
						id->str().c_str(), ctor_return_type->str().c_str()));

			/* we need to register this constructor as an override for the name `tag_name` */
			debug_above(2, log(log_info, "adding %s as an unchecked generic data_ctor",
						id->str().c_str()));

			types::type_function_t::ref data_ctor_sig = type_function(
					type_args(struct_->dimensions),
					ctor_return_type);

			module_scope->get_program_scope()->put_unchecked_variable(tag_name,
					unchecked_data_ctor_t::create(id, node, module_scope, data_ctor_sig));
			return type;
		} else {
			user_error(status, node->get_location(), "local type definitions are not yet impl");
		}
	} else {
		// This should not happen...
		not_impl();
	}

	assert(!status);
	return nullptr;
}

void ast::struct_t::register_type(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope) const
{
	debug_above(5, log(log_info, "creating product type for %s", str().c_str()));

	auto type_name = get_type_name();

	if (auto found_type = scope->get_bound_type(type_name)) {
		/* simple check for an already bound monotype */
		user_error(status, get_location(),
			   	"symbol " c_id("%s") " was already defined",
				type_name.c_str());
		user_message(log_warning, status, found_type->get_location(),
				"previous version of %s defined here",
				found_type->str().c_str());
	} else {
		auto env = scope->get_typename_env();
		auto env_iter = env.find(type_name);
		if (env_iter == env.end()) {
			/* instantiate_data_ctor_type has the side-effect of creating an
			 * unchecked data ctor for the type */
			auto data_ctor_type = instantiate_data_ctor_type(status, builder,
					get_type(), scope, shared_from_this(),
					make_code_id(token_t{token.location, tk_identifier, type_name}));

			if (!!status) {
				/* register the typename in the current environment */
				auto qualified_type_name = scope->make_fqn(type_name);
				debug_above(7, log(log_info, "registering type " c_type("%s") " in scope %s",
							qualified_type_name.c_str(), scope->get_name().c_str()));
				scope->put_typename(status, qualified_type_name, data_ctor_type);

				/* success */
				return;
			}
		} else {
			/* simple check for an already bound typename env variable */
			user_error(status, token.location,
					"symbol " c_id("%s") " is already taken in typename env by %s",
					type_name.c_str(),
					env_iter->second->str().c_str());
		}
	}

	assert(!status);
}

void ast::polymorph_t::register_type(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope) const
{
	debug_above(3, log(log_info, "creating subtypes to %s", token.text.c_str()));

	scope->put_typename(status,
		   	scope->make_fqn(get_type_name()),
		   	get_type());
}
