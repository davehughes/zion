#include "zion.h"
#include "dbg.h"
#include "bound_type.h"
#include "scopes.h"
#include "bound_var.h"
#include "ast.h"
#include "type_instantiation.h"
#include "llvm_types.h"
#include "llvm_utils.h"
#include <iostream>

bound_type_t::bound_type_t(
		types::type_t::ref type,
		location_t location,
		llvm::Type *llvm_type,
		llvm::Type *llvm_specific_type) :
	type(type),
	location(location),
	llvm_type(llvm_type),
	llvm_specific_type(llvm_specific_type)
{
	assert(llvm_type != nullptr);

	debug_above(6, log(log_info, "creating type %s with (%s, LLVM TypeID %d, %s)",
			type->str().c_str(),
			llvm_print(llvm_specific_type).c_str(),
			llvm_type ? llvm_type->getTypeID() : -1,
			location.str().c_str()));
}

types::type_t::ref bound_type_t::get_type() const {
	return type;
}

location_t const bound_type_t::get_location() const {
	return location;
}

llvm::Type *bound_type_t::get_llvm_type() const {
	return llvm_type;
}

llvm::Type *bound_type_t::get_llvm_specific_type() const {
	return llvm_specific_type ? llvm_specific_type : llvm_type;
}

bound_type_t::ref bound_type_t::get_pointer() const {
	return create(
			type_ptr(type),
			location,
			llvm_type->getPointerTo(),
			llvm_specific_type ? llvm_specific_type->getPointerTo() : nullptr);
}

bound_type_t::ref bound_type_t::create(
		types::type_t::ref type,
		location_t location,
		llvm::Type *llvm_type,
		llvm::Type *llvm_specific_type)
{
	return make_ptr<bound_type_t>(type, location, llvm_type,
			llvm_specific_type ? llvm_specific_type : llvm_type);
}

std::string str(const bound_type_t::refs &args) {
	std::stringstream ss;
	ss << "(";
	const char *sep = "";
	for (auto &arg : args) {
		ss << sep << arg->get_type()->str();
		sep = ", ";
	}
	ss << ")";
	return ss.str();
}

std::string str(const bound_type_t::named_pairs &named_pairs) {
	std::stringstream ss;
	ss << "(";
	const char *sep = "";
	for (auto &pair : named_pairs) {
		ss << sep << "(" << pair.first << " " << pair.second->str() << ")";
		sep = ", ";
	}
	ss << ")";
	return ss.str();
}

std::string str(const bound_type_t::name_index &name_index) {
	std::stringstream ss;
	ss << "{";
	const char *sep = "";
	for (auto &pair : name_index) {
		ss << sep << pair.first << ": " << pair.second;
		sep = ", ";
	}
	ss << "}";
	return ss.str();
}

std::ostream &operator <<(std::ostream &os, const bound_type_t &type) {
	return os << type.str();
}

std::string bound_type_t::str() const {
	std::stringstream ss;
	ss << get_type();
#ifdef DEBUG_LLVM_TYPES
	ss << " " << llvm_print(get_llvm_specific_type());
#endif
	return ss.str();
}

types::type_args_t::ref get_args_type(bound_type_t::named_pairs args) {
	types::type_t::refs sig_args;
	identifier::refs names;
	for (auto &named_pair : args) {
		names.push_back(make_iid(named_pair.first));
		sig_args.push_back(named_pair.second->get_type());
	}
	return type_args(sig_args, names);
}

types::type_args_t::ref get_args_type(bound_type_t::refs args) {
	types::type_t::refs sig_args;
	for (auto &arg : args) {
		assert(arg != nullptr);
		sig_args.push_back(arg->get_type());
	}
	return type_args(sig_args);
}

types::type_args_t::ref get_args_type(bound_var_t::refs args) {
	types::type_t::refs sig_args;
	for (auto &arg : args) {
		assert(arg != nullptr);
		sig_args.push_back(arg->get_type());
	}
	return type_args(sig_args);
}

types::type_t::refs get_types(const bound_type_t::refs &bound_types) {
	types::type_t::refs types;
	for (auto &bound_type : bound_types) {
		assert(bound_type != nullptr);
		types.push_back(bound_type->get_type());
	}
	return types;
}

types::type_tuple_t::ref get_tuple_type(const bound_type_t::refs &items_types) {
	types::type_t::refs dimensions;
	for (auto &arg : items_types) {
		assert(arg != nullptr);
		dimensions.push_back(arg->get_type());
	}
	return type_tuple(dimensions);
}

bound_type_t::refs bound_type_t::refs_from_vars(const bound_var_t::refs &args) {
	bound_type_t::refs arg_types;
	for (auto &arg : args) {
		assert(arg != nullptr);
		assert(arg->type != nullptr);
		arg_types.push_back(arg->type);
	}
	return arg_types;
}

bool bound_type_t::is_ref(scope_t::ref scope) const {
	return get_type()->eval_predicate(tb_ref, scope);
}

bool bound_type_t::is_int(scope_t::ref scope) const {
	return get_type()->eval_predicate(tb_int, scope);
}

bool bound_type_t::is_function(scope_t::ref scope) const {
	return get_type()->eval_predicate(tb_function, scope);
}

bool bound_type_t::is_void(scope_t::ref scope) const {
	return get_type()->eval_predicate(tb_void, scope);
}

bool bound_type_t::is_unit(scope_t::ref scope) const {
    return get_type()->eval_predicate(tb_unit, scope);
}

bool bound_type_t::is_maybe(scope_t::ref scope) const {
	return get_type()->eval_predicate(tb_maybe, scope);
}

bool bound_type_t::is_module() const {
	return types::is_type_id(get_type(), "module", nullptr);
}

bool bound_type_t::is_ptr(scope_t::ref scope) const {
	bool res = types::is_ptr(type, scope);
	debug_above(7, log("checking whether %s is a ptr of some kind: %s",
				type->str().c_str(),
				res ? c_good("it is") : c_error("it isn't")));

	assert_implies(res, llvm::dyn_cast<llvm::PointerType>(get_llvm_specific_type()));
	return res;
}

void bound_type_t::is_managed_ptr(
	   	llvm::IRBuilder<> &builder,
	   	ptr<scope_t> scope,
		bool &is_managed) const
{
	is_managed = types::is_managed_ptr(type, scope);
}


types::signature bound_type_t::get_signature() const {
	return get_type()->get_signature();
}

types::type_function_t::ref get_function_type(
		types::type_t::ref type_constraints,
		bound_type_t::named_pairs named_args,
		bound_type_t::ref ret)
{
	bound_type_t::refs args;
	for (auto named_arg : named_args) {
		args.push_back(named_arg.second);
	}
	return get_function_type(type_constraints, args, ret);
}

types::type_function_t::ref get_function_type(
		types::type_t::ref type_constraints,
		bound_type_t::refs args,
		bound_type_t::ref return_type)
{
	types::type_t::refs type_args;

	for (auto arg : args) {
		type_args.push_back(arg->get_type());
	}

	return ::type_function(
			INTERNAL_LOC(),
			type_constraints,
			::type_args(type_args),
			return_type->get_type());
}

types::type_function_t::ref get_function_type(
		types::type_t::ref type_constraints,
		bound_type_t::refs args,
		types::type_t::ref return_type)
{
	types::type_t::refs type_args;

	for (auto arg : args) {
		type_args.push_back(arg->get_type());
	}

	return ::type_function(
			INTERNAL_LOC(),
			type_constraints,
			::type_args(type_args),
			return_type);
}
