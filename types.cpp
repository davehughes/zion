#include "zion.h"
#include "dbg.h"
#include "types.h"
#include <sstream>
#include "utils.h"
#include "types.h"
#include "parser.h"
#include <iostream>

const char *BUILTIN_NIL_TYPE = "nil";
const char *BUILTIN_LIST_TYPE = "List";
const char *BUILTIN_VOID_TYPE = "void";
const char *BUILTIN_UNREACHABLE_TYPE = "__unreachable";

int next_generic = 1;

void reset_generics() {
	next_generic = 1;
}

namespace types {

	/**********************************************************************/
	/* Types                                                              */
	/**********************************************************************/

	std::string type::str() const {
		return str(map{});
	}

	std::string type::str(const map &bindings) const {
	   	return string_format(c_type("%s"), this->repr(bindings).c_str());
   	}

	atom type::repr(const map &bindings) const {
		std::stringstream ss;
		emit(ss, bindings);
		return ss.str();
	}

	type_id::type_id(identifier::ref id) : id(id) {
	}

	std::ostream &type_id::emit(std::ostream &os, const map &bindings) const {
		return os << id->get_name();
	}

	int type_id::ftv_count() const {
		/* how many free type variables exist in this type? */
		return 0;
	}

    atom::set type_id::get_ftvs() const {
        return {};
    }

	type::ref type_id::rebind(const map &bindings) const {
		return shared_from_this();
	}

	location type_id::get_location() const {
		return id->get_location();
	}

	identifier::ref type_id::get_id() const {
		return id;
	}

	bool type_id::is_void() const {
	   	return id->get_name() == BUILTIN_VOID_TYPE;
   	}

	bool type_id::is_nil() const {
	   	return id->get_name() == BUILTIN_NIL_TYPE;
   	}

	type_variable::type_variable(identifier::ref id) : id(id), location(id->get_location()) {
	}

    identifier::ref gensym() {
        /* generate fresh "any" variables */
        return make_iid({string_format("__%d", next_generic++)});
    }

	type_variable::type_variable(struct location location) : id(gensym()), location(location) {
	}

	std::ostream &type_variable::emit(std::ostream &os, const map &bindings) const {
		auto instance_iter = bindings.find(id->get_name());
		if (instance_iter != bindings.end()) {
			return instance_iter->second->emit(os, bindings);
		} else {
			return os << string_format("(any %s)", id->get_name().c_str());
		}
	}

	/* how many free type variables exist in this type? */
	int type_variable::ftv_count() const {
		return 1;
	}

    atom::set type_variable::get_ftvs() const {
        return {id->get_name()};
    }

	type::ref type_variable::rebind(const map &bindings) const {
		if (bindings.size() == 0) {
			return shared_from_this();
		}

		auto instance_iter = bindings.find(id->get_name());
		if (instance_iter != bindings.end()) {
			return instance_iter->second;
		} else {
			return shared_from_this();
		}
	}

	location type_variable::get_location() const {
		return location;
	}

	identifier::ref type_variable::get_id() const {
		return id;
	}

	type_operator::type_operator(type::ref oper, type::ref operand) :
		oper(oper), operand(operand)
	{
	}

	std::ostream &type_operator::emit(std::ostream &os, const map &bindings) const {
		oper->emit(os, bindings);
		os << "{";
		operand->emit(os, bindings);
		return os << "}";
	}

	int type_operator::ftv_count() const {
		return oper->ftv_count() + operand->ftv_count();
	}

    atom::set type_operator::get_ftvs() const {
        atom::set oper_set = oper->get_ftvs();
        atom::set operand_set = operand->get_ftvs();
        oper_set.insert(operand_set.begin(), operand_set.end());
        return oper_set;
    }

	type::ref type_operator::rebind(const map &bindings) const {
		if (bindings.size() == 0) {
			return shared_from_this();
		}

		return ::type_operator(oper->rebind(bindings), operand->rebind(bindings));
	}

	location type_operator::get_location() const {
		return oper->get_location();
	}

	identifier::ref type_operator::get_id() const {
		return oper->get_id();
	}

	type_struct::type_struct(type::refs dimensions, types::name_index name_index, bool managed) :
		dimensions(dimensions), name_index(name_index), managed(managed)
	{
#ifdef ZION_DEBUG
		for (auto dimension: dimensions) {
			assert(dimension != nullptr);
		}
		assert(name_index.size() == dimensions.size() || name_index.size() == 0);
#endif
	}

	product_kind_t type_struct::get_pk() const {
		return pk_struct;
	}

	type::refs type_struct::get_dimensions() const {
		return dimensions;
	}

	name_index type_struct::get_name_index() const {
		return name_index;
	}

	std::ostream &type_struct::emit(std::ostream &os, const map &bindings) const {
		os << (managed ? "managed " : "native ");
		os << "struct (";
		const char *sep = "";
		for (auto dimension : dimensions) {
			os << sep;
			dimension->emit(os, bindings);
			sep = ", ";
		}
		if (name_index.size() != 0) {
			os << " " << ::str(name_index);
		}
		return os << ")";
	}

	int type_struct::ftv_count() const {
		int ftv_sum = 0;
		for (auto dimension : dimensions) {
			ftv_sum += dimension->ftv_count();
		}
		return ftv_sum;
	}

	atom::set type_struct::get_ftvs() const {
		atom::set set;
		for (auto dimension : dimensions) {
			atom::set dim_set = dimension->get_ftvs();
			set.insert(dim_set.begin(), dim_set.end());
		}
		return set;
    }


	type::ref type_struct::rebind(const map &bindings) const {
		if (bindings.size() == 0) {
			return shared_from_this();
		}

		refs type_dimensions;
		for (auto dimension : dimensions) {
			type_dimensions.push_back(dimension->rebind(bindings));
		}
		return ::type_struct(type_dimensions, name_index, managed);
	}

	location type_struct::get_location() const {
		if (dimensions.size() != 0) {
			return dimensions[0]->get_location();
		} else {
			return INTERNAL_LOC();
		}
	}

	identifier::ref type_struct::get_id() const {
		return nullptr;
	}

	type_args::type_args(type::refs args, types::name_index name_index) :
		args(args), name_index(name_index)
	{
#ifdef ZION_DEBUG
		for (auto arg: args) {
			assert(arg != nullptr);
		}
		assert(name_index.size() == args.size() || name_index.size() == 0);
#endif
	}

	product_kind_t type_args::get_pk() const {
		return pk_args;
	}

	type::refs type_args::get_dimensions() const {
		return args;
	}

	name_index type_args::get_name_index() const {
		return name_index;
	}

	std::ostream &type_args::emit(std::ostream &os, const map &bindings) const {
		os << "(";
		const char *sep = "";
		for (auto arg : args) {
			os << sep;
			arg->emit(os, bindings);
			sep = ", ";
		}
		if (name_index.size() != 0) {
			os << " " << ::str(name_index);
		}
		return os << ")";
	}

	int type_args::ftv_count() const {
		int ftv_sum = 0;
		for (auto arg : args) {
			ftv_sum += arg->ftv_count();
		}
		return ftv_sum;
	}

	atom::set type_args::get_ftvs() const {
		atom::set set;
		for (auto arg : args) {
			atom::set dim_set = arg->get_ftvs();
			set.insert(dim_set.begin(), dim_set.end());
		}
		return set;
    }


	type::ref type_args::rebind(const map &bindings) const {
		if (bindings.size() == 0) {
			return shared_from_this();
		}

		refs type_args;
		for (auto arg : args) {
			type_args.push_back(arg->rebind(bindings));
		}
		return ::type_args(type_args, name_index);
	}

	location type_args::get_location() const {
		if (args.size() != 0) {
			return args[0]->get_location();
		} else {
			return INTERNAL_LOC();
		}
	}

	identifier::ref type_args::get_id() const {
		return nullptr;
	}

	type_ref::type_ref(type::ref element_type) :
		element_type(element_type)
	{
#ifdef ZION_DEBUG
		assert(element_type != nullptr);
#endif
	}

	product_kind_t type_ref::get_pk() const {
		return pk_ref;
	}

	type::refs type_ref::get_dimensions() const {
		return {element_type};
	}

	name_index type_ref::get_name_index() const {
		return {};
	}

	std::ostream &type_ref::emit(std::ostream &os, const map &bindings) const {
		os << "ref ";
		element_type->emit(os, bindings);
		return os;
	}

	int type_ref::ftv_count() const {
		return element_type->ftv_count();
	}

	atom::set type_ref::get_ftvs() const {
		return element_type->get_ftvs();
    }


	type::ref type_ref::rebind(const map &bindings) const {
		if (bindings.size() == 0) {
			return shared_from_this();
		}

		return ::type_ref(element_type->rebind(bindings));
	}

	location type_ref::get_location() const {
		return element_type->get_location();
	}

	identifier::ref type_ref::get_id() const {
		return element_type->get_id();
	}

	type_module::type_module(type::ref module_type) :
		module_type(module_type)
	{
#ifdef ZION_DEBUG
		assert(module_type != nullptr);
#endif
	}

	product_kind_t type_module::get_pk() const {
		return pk_module;
	}

	type::refs type_module::get_dimensions() const {
		return {module_type};
	}

	name_index type_module::get_name_index() const {
		return {};
	}

	std::ostream &type_module::emit(std::ostream &os, const map &bindings) const {
		os << "module ";
		module_type->emit(os, bindings);
		return os;
	}

	int type_module::ftv_count() const {
		return module_type->ftv_count();
	}

	atom::set type_module::get_ftvs() const {
		return module_type->get_ftvs();
    }


	type::ref type_module::rebind(const map &bindings) const {
		if (bindings.size() == 0) {
			return shared_from_this();
		}

		return ::type_module(module_type->rebind(bindings));
	}

	location type_module::get_location() const {
		return module_type->get_location();
	}

	identifier::ref type_module::get_id() const {
		return module_type->get_id();
	}

	type_function::type_function(
			type::ref inbound_context,
		   	type_args::ref args,
			type::ref return_type) :
		inbound_context(inbound_context), args(args), return_type(return_type)
	{
		assert(inbound_context != nullptr);
		assert(args != nullptr);
		assert(return_type != nullptr);
	}

	std::ostream &type_function::emit(std::ostream &os, const map &bindings) const {
		os << "[" << inbound_context->str() << "] def ";
		os << args->str() << " " << return_type->str();
		return os;
	}

	int type_function::ftv_count() const {
		return args->ftv_count() + return_type->ftv_count();
	}

	atom::set type_function::get_ftvs() const {
		atom::set set;
		atom::set args_ftvs = args->get_ftvs();
		set.insert(args_ftvs.begin(), args_ftvs.end());
		atom::set return_type_ftvs = return_type->get_ftvs();
		set.insert(return_type_ftvs.begin(), return_type_ftvs.end());
		return set;
    }


	type::ref type_function::rebind(const map &bindings) const {
		if (bindings.size() == 0) {
			return shared_from_this();
		}

		types::type_args::ref rebound_args = dyncast<const types::type_args>(
			   	args->rebind(bindings));
		assert(args != nullptr);
		return ::type_function(inbound_context,
				rebound_args, return_type->rebind(bindings));
	}

	location type_function::get_location() const {
		return args->get_location();
	}

	identifier::ref type_function::get_id() const {
		return nullptr;
	}

	bool type_function::is_function() const {
	   	return true;
   	}

	type_sum::type_sum(type::refs options) : options(options) {
		for (auto option : options) {
            assert(!dyncast<const type_maybe>(option));
            assert(!option->is_nil());
        }
	}

	std::ostream &type_sum::emit(std::ostream &os, const map &bindings) const {
		os << "(or";
		assert(options.size() != 0);
		for (auto option : options) {
			os << " ";
			option->emit(os, bindings);
		}
		return os << ")";
	}

	int type_sum::ftv_count() const {
		int ftv_sum = 0;
		for (auto option : options) {
			ftv_sum += option->ftv_count();
		}
		return ftv_sum;
	}

    atom::set type_sum::get_ftvs() const {
        atom::set set;
		for (auto option : options) {
            atom::set option_set = option->get_ftvs();
            set.insert(option_set.begin(), option_set.end());
		}
		return set;
	}

	type::ref type_sum::rebind(const map &bindings) const {
		if (bindings.size() == 0) {
			return shared_from_this();
		}

		refs type_options;
		for (auto option : options) {
			type_options.push_back(option->rebind(bindings));
		}
		return ::type_sum(type_options);
	}

	location type_sum::get_location() const {
		if (options.size() != 0) {
			return options[0]->get_location();
		} else {
			return INTERNAL_LOC();
		}
	}

	identifier::ref type_sum::get_id() const {
		return nullptr;
	}

	type_maybe::type_maybe(type::ref just) : just(just) {
        assert(!dyncast<const type_maybe>(just));
        assert(!just->is_nil());
	}

	std::ostream &type_maybe::emit(std::ostream &os, const map &bindings) const {
        just->emit(os, bindings);
		return os << "?";
	}

	int type_maybe::ftv_count() const {
        return just->ftv_count();
	}

    atom::set type_maybe::get_ftvs() const {
        return just->get_ftvs();
	}

	type::ref type_maybe::rebind(const map &bindings) const {
		if (bindings.size() == 0) {
			return shared_from_this();
		}

        return ::type_maybe(just->rebind(bindings));
	}

	location type_maybe::get_location() const {
        return just->get_location();
	}

	identifier::ref type_maybe::get_id() const {
		return nullptr;
	}

	type_lambda::type_lambda(identifier::ref binding, type::ref body) :
		binding(binding), body(body)
	{
	}

	std::ostream &type_lambda::emit(std::ostream &os, const map &bindings_) const {
		os << "(lambda [" << C_ID << binding->get_name().str() << C_RESET "] ";
		map bindings = bindings_;
		auto binding_iter = bindings.find(binding->get_name());
		if (binding_iter != bindings.end()) {
			bindings.erase(binding_iter);
		}
		body->emit(os, bindings);
		return os << ")";
	}

	int type_lambda::ftv_count() const {
		/* pretend this is getting applied */
		assert(!"This should not really get called ....");
		map bindings;
		bindings[binding->get_name()] = type_unreachable();
		return body->rebind(bindings)->ftv_count();
	}

    atom::set type_lambda::get_ftvs() const {
		assert(!"This should not really get called ....");
		map bindings;
		bindings[binding->get_name()] = type_unreachable();
		return body->rebind(bindings)->get_ftvs();
	}

	type::ref type_lambda::rebind(const map &bindings_) const {
		if (bindings_.size() == 0) {
			return shared_from_this();
		}

		map bindings = bindings_;
		auto binding_iter = bindings.find(binding->get_name());
		if (binding_iter != bindings.end()) {
			bindings.erase(binding_iter);
		}
		return ::type_lambda(binding, body->rebind(bindings));
	}

	location type_lambda::get_location() const {
		return binding->get_location();
	}

	identifier::ref type_lambda::get_id() const {
		return nullptr;
	}

	bool is_type_id(type::ref type, atom type_name) {
		if (auto pti = dyncast<const types::type_id>(type)) {
			return pti->id->get_name() == type_name;
		}
		return false;
	}
}

types::type::ref type_id(identifier::ref id) {
	return make_ptr<types::type_id>(id);
}

types::type::ref type_variable(identifier::ref id) {
	return make_ptr<types::type_variable>(id);
}

types::type::ref type_variable(struct location location) {
	return make_ptr<types::type_variable>(location);
}

types::type::ref type_unreachable() {
	return make_ptr<types::type_id>(make_iid(BUILTIN_UNREACHABLE_TYPE));
}

types::type::ref type_nil() {
	static auto nil_type = make_ptr<types::type_id>(make_iid(BUILTIN_NIL_TYPE));
    return nil_type;
}

types::type::ref type_void() {
	return make_ptr<types::type_id>(make_iid(BUILTIN_VOID_TYPE));
}

types::type::ref type_operator(types::type::ref operator_, types::type::ref operand) {
	return make_ptr<types::type_operator>(operator_, operand);
}

types::type_struct::ref type_struct(
	   	types::type::refs dimensions,
	   	types::name_index name_index,
		bool managed)
{
	if (name_index.size() == 0) {
		/* if we omit names for our dimensions, give them names like _0, _1, _2,
		 * etc... so they can be accessed like mytuple._5 if necessary */
		for (int i = 0; i < dimensions.size(); ++i) {
			name_index[string_format("_%d", i)] = i;
		}
	}
	return make_ptr<types::type_struct>(dimensions, name_index, managed);
}

types::type_args::ref type_args(
	   	types::type::refs args,
	   	types::name_index name_index)
{
	return make_ptr<types::type_args>(args, name_index);
}

types::type_module::ref type_module(types::type::ref module_type) {
	return make_ptr<types::type_module>(module_type);
}

types::type_ref::ref type_ref(types::type::ref element_type) {
	return make_ptr<types::type_ref>(element_type);
}

types::type_function::ref type_function(
		types::type::ref inbound_context,
		types::type_args::ref args,
		types::type::ref return_type)
{
	return make_ptr<types::type_function>(inbound_context, args, return_type);
}

types::type::ref type_sum(types::type::refs options) {
	return make_ptr<types::type_sum>(options);
}

types::type::ref type_maybe(types::type::ref just) {
    if (auto maybe = dyncast<const types::type_maybe>(just)) {
		return just;
	}
	return make_ptr<types::type_maybe>(just);
}

types::type::ref type_lambda(identifier::ref binding, types::type::ref body) {
	return make_ptr<types::type_lambda>(binding, body);
}

types::type::ref type_list_type(types::type::ref element) {
	return type_maybe(type_operator(type_id(make_iid(BUILTIN_LIST_TYPE)), element));
}

types::type::ref type_strip_maybe(types::type::ref maybe_maybe) {
    if (auto maybe = dyncast<const types::type_maybe>(maybe_maybe)) {
        return maybe->just;
    } else {
        return maybe_maybe;
    }
}

std::ostream& operator <<(std::ostream &os, const types::type::ref &type) {
	os << type->str();
	return os;
}

types::type::ref get_function_return_type(types::type::ref function_type) {
	debug_above(5, log(log_info, "getting function return type from %s", function_type->str().c_str()));

	auto type_function = dyncast<const types::type_function>(function_type);
	assert(type_function != nullptr);

	return type_function->return_type;
}

std::ostream &operator <<(std::ostream &os, identifier::ref id) {
	return os << id->get_name();
}

types::type::pair make_type_pair(std::string fst, std::string snd, identifier::set generics) {
	debug_above(4, log(log_info, "creating type pair with (%s, %s) and generics [%s]",
				fst.c_str(), snd.c_str(),
			   	join(generics, ", ").c_str()));

	return types::type::pair{parse_type_expr(fst, generics), parse_type_expr(snd, generics)};
}

types::type::ref parse_type_expr(std::string input, identifier::set generics) {
	status_t status;
	std::istringstream iss(input);
	zion_lexer_t lexer("", iss);
	parse_state_t ps(status, "", lexer, nullptr);
	types::type::ref type = parse_maybe_type(ps, {}, {}, generics);
	if (!!status) {
		return type;
	} else {
		panic("bad type");
		return null_impl();
	}
}

types::type::ref operator "" _ty(const char *value, size_t) {
	return parse_type_expr(value, {});
}

bool get_type_variable_name(types::type::ref type, atom &name) {
    if (auto ptv = dyncast<const types::type_variable>(type)) {
		name = ptv->id->get_name();
		return true;
	} else {
		return false;
	}
	return false;
}

std::string str(types::type::refs refs) {
	std::stringstream ss;
	ss << "(";
	const char *sep = "";
	for (auto p : refs) {
		ss << sep << p->str();
		sep = ", ";
	}
	ss << ")";
	return ss.str();
}

std::string str(types::type::map coll) {
	std::stringstream ss;
	ss << "{";
	const char *sep = "";
	for (auto p : coll) {
		ss << sep << C_ID << p.first.c_str() << C_RESET ": ";
		ss << p.second->str().c_str();
		sep = ", ";
	}
	ss << "}";
	return ss.str();
}

const char *pkstr(product_kind_t pk) {
	switch (pk) {
	case pk_module:
		return "module";
	case pk_struct:
		return "struct";
	case pk_ref:
		return "ref";
	case pk_args:
		return "args";
	}
	assert(false);
	return nullptr;
}

types::type::ref eval(types::type::ref type, types::type::map env) {
	/* if there is no expansion of the type passed in, we will return nullptr */

	if (auto id = dyncast<const types::type_id>(type)) {
		return eval_id(id, env);
	} else if (auto operator_ = dyncast<const types::type_operator>(type)) {
		return eval_apply(operator_->oper, operator_->operand, env);
	// } else if (auto product = dyncast<const types::type_product>(type)) {
		/* there is no expansion of product types */
	// 	return nullptr;
	} else {
		return null_impl();
	}
}

types::type::ref eval_id(
		ptr<const types::type_id> ptid,
		types::type::map env)
{
	/* if there is no expansion of the type passed in, we will return nullptr */

	assert(ptid != nullptr);

	/* look in the environment for a declaration of this term */
	auto fn_iter = env.find(ptid->id->get_name());
	if (fn_iter != env.end()) {
		return fn_iter->second;
	} else {
		return nullptr;
	}
}

types::type::ref eval_apply(
		types::type::ref oper,
	   	types::type::ref operand, 
		types::type::map env)
{
	/* if there is no expansion of the type passed in, we will return nullptr */

	assert(oper != nullptr);
	assert(operand != nullptr);
	if (auto ptid = dyncast<const types::type_id>(oper)) {
		/* look in the environment for a declaration of this operator */
		types::type::ref expansion = eval_id(ptid, env);

		debug_above(7, log(log_info, "eval_apply : %s expanded to %s in %s",
					ptid->str().c_str(),
					expansion ? expansion->str().c_str() : c_error("nothing"),
                    str(env).c_str()));
		if (expansion != nullptr) {
			return eval_apply(expansion, operand, env);
		} else {
			return nullptr;
		}
	} else if (auto lambda = dyncast<const types::type_lambda>(oper)) {
		auto var_name = lambda->binding->get_name();
		return lambda->body->rebind({{var_name, operand}});
	} else if (auto pto = dyncast<const types::type_operator>(oper)) {
		auto new_operator = eval_apply(pto->oper, pto->operand, env);
		return eval_apply(new_operator, operand, env);
	} else if (auto ptv = dyncast<const types::type_variable>(oper)) {
		/* type_variables cannot be applied */
		return nullptr;
	} else {
		std::cerr << "what is this? " << oper->str() << std::endl;
		return null_impl();
	}
}

