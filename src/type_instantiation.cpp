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

	/*
	   debug_above(5, log(log_info, "function return_type %s expands to %s",
	   function->return_type->str().c_str(),
	   eval(function->return_type, scope->get_typename_env())->str().c_str()));
     */

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

void instantiate_data_ctor_type(
		status_t &status,
		llvm::IRBuilder<> &builder,
		types::type_t::ref unbound_type,
		scope_t::ref scope,
		ptr<const ast::item_t> node,
		identifier::ref id)
{
	/* create the basic struct type */
	ptr<const types::type_struct_t> struct_ = dyncast<const types::type_struct_t>(unbound_type);
	assert(struct_ != nullptr);
	assert(struct_->dimensions.size() != 0);

	/**********************************************/
	/* Register a data ctor for this struct_ type */
	/**********************************************/

	/* we're declaring a ctor at module scope */
	if (auto module_scope = dyncast<module_scope_t>(scope)) {

		/* create the actual expanded type signature of this type */
		types::type_t::ref type = type_id(id);

		/* we need to register this constructor. all ctors return managed ptrs */
		debug_above(4, log(log_info, "return type for data ctor %s will be %s",
					id->str().c_str(), type->str().c_str()));

		/* the data constructor will take all of the members of this struct, and
		 * return a managed pointer to it */
		types::type_function_t::ref data_ctor_sig = type_function(type_args(struct_->dimensions),
			   	type);

		module_scope->get_program_scope()->put_unchecked_variable(id->get_name(),
				unchecked_data_ctor_t::create(id, node, module_scope, data_ctor_sig));

		/* register the typename in the current environment */
		debug_above(7, log(log_info, "registering type " c_type("%s") " in scope %s",
					id->get_name().c_str(), scope->get_name().c_str()));
		scope->put_typename(status, id->get_name(), type_ptr(type_managed(struct_)));

		// TODO: consider just pushing typename registration to program_scope

		return;
	} else {
		user_error(status, node->get_location(), "local type definitions are not supported");
	}

	assert(!status);
	return;
}

void ast::tag_t::register_type(
		status_t &status,
		llvm::IRBuilder<> &builder,
		scope_t::ref scope) const
{
	std::string tag_name = token.text;

	auto tag_type = type_ptr(type_managed(type_struct({}, {})));

	/* it's a nullary enumeration or "tag", let's create a global value to
	 * represent this tag. */

	bound_type_t::ref bound_tag_type = scope->get_program_scope()->get_bound_type(
			tag_type->get_signature());

	if (bound_tag_type == nullptr) {
		/* only once: make an empty type for all tags */
		bound_tag_type = bound_type_t::create(
				tag_type,
				token.location,
				/* all tags use the var_t* type */
				scope->get_program_scope()->get_bound_type({"__var_ref"})->get_llvm_type());

		scope->get_program_scope()->put_bound_type(status, bound_tag_type);
	}

	if (!!status) {
		bound_var_t::ref tag = llvm_create_global_tag(
				builder, scope, bound_tag_type, tag_name,
				make_code_id(token));

		/* record this tag variable for use later */
		scope->get_program_scope()->put_bound_variable(status, tag_name, tag);
		scope->put_typename(status, tag_name, tag_type);

		if (!!status) {
			debug_above(7, log(log_info, "instantiated nullary data ctor %s",
						tag->str().c_str()));
			return;
		}
	}

	assert(!status);
	return;
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
			 * unchecked data ctor for the type, and registering the type name */
			instantiate_data_ctor_type(status, builder,
					get_type(), scope, shared_from_this(),
					make_code_id(token_t{token.location, tk_identifier, type_name}));
			return;
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
	scope->put_typename(status, get_type_name(), get_type());
}
