#pragma once
#include "zion.h"
#include "ast_decls.h"
#include "token.h"
#include <vector>
#include <set>
#include <string>
#include "dbg.h"
#include <memory>
#include "scopes.h"
#include "match.h"
#include "type_checker.h"
#include "callable.h"
#include "life.h"

struct parse_state_t;

enum syntax_kind_t {
	sk_null=0,

#define declare_syntax_kind(x) \
	sk_##x,

#define OP declare_syntax_kind
#include "sk_ops.h"
#undef OP

	sk_expression,
	sk_statement,
};

const char *skstr(syntax_kind_t sk);

namespace ast {
	struct render_state_t;

	struct item_t : std::enable_shared_from_this<item_t> {
		typedef ptr<const item_t> ref;

		virtual ~item_t() throw() = 0;
		std::string str() const;
		virtual void render(render_state_t &rs) const = 0;
		location_t get_location() const { return token.location; }

		syntax_kind_t sk;
		token_t token;
	};

	void log_named_item_create(const char *type, const std::string &name);

	template <typename T>
	ptr<T> create(const token_t &token) {
		auto item = ptr<T>(new T());
		item->sk = T::SK;
		item->token = token;
		debug_ex(log_named_item_create(skstr(T::SK), token.text));
		return item;
	}

	template <typename T, typename... Args>
	ptr<T> create(const token_t &token, Args... args) {
		auto item = ptr<T>(new T(args...));
		item->sk = T::SK;
		item->token = token;
		debug_ex(log_named_item_create(skstr(T::SK), token.text));
		return item;
	}

	struct statement_t : public virtual item_t {
		typedef ptr<const statement_t> ref;

		static const syntax_kind_t SK = sk_statement;
		virtual ~statement_t() {}
		static ptr<ast::statement_t> parse(parse_state_t &ps);
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const = 0;
	};

	struct module_t;
	struct expression_t;
	struct var_decl_t;

	struct param_list_decl_t : public item_t {
		typedef ptr<const param_list_decl_t> ref;

		static const syntax_kind_t SK = sk_param_list_decl;
		static ptr<param_list_decl_t> parse(parse_state_t &ps);
		virtual void render(render_state_t &rs) const;

		std::vector<ptr<var_decl_t>> params;
	};

	struct condition_t : public virtual item_t {
		typedef ptr<const condition_t> ref;
		virtual ~condition_t() {}
		virtual bound_var_t::ref resolve_condition(
				llvm::IRBuilder<> &builder,
				runnable_scope_t::ref block_scope,
				life_t::ref life,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *scope_if_true,
				runnable_scope_t::ref *scope_if_false) const = 0;
	};

	struct predicate_t : public virtual item_t {
		typedef ptr<const predicate_t> ref;
		typedef std::vector<ref> refs;
		virtual ~predicate_t() {}

		virtual std::string repr() const = 0;
		static ref parse(parse_state_t &ps, bool allow_else);
		virtual match::Pattern::ref get_pattern(types::type_t::ref type, env_t::ref env) const = 0;
		virtual bool resolve_match(
				llvm::IRBuilder<> &builder,
				runnable_scope_t::ref scope,
				life_t::ref life,
				location_t value_location,
				bound_var_t::ref input_value,
				llvm::BasicBlock *llvm_match_block,
				llvm::BasicBlock *llvm_no_match_block,
				runnable_scope_t::ref *scope_if_true) const = 0;
	};

	struct irrefutable_predicate_t : public predicate_t {
		typedef ptr<const irrefutable_predicate_t> ref;

		static const syntax_kind_t SK = sk_irrefutable_predicate;
		virtual bool resolve_match(
				llvm::IRBuilder<> &builder,
				runnable_scope_t::ref scope,
				life_t::ref life,
				location_t value_location,
				bound_var_t::ref input_value,
				llvm::BasicBlock *llvm_match_block,
				llvm::BasicBlock *llvm_no_match_block,
				runnable_scope_t::ref *scope_if_true) const;
		virtual std::string repr() const;
		virtual void render(render_state_t &rs) const;
		virtual match::Pattern::ref get_pattern(types::type_t::ref type, env_t::ref env) const;
	};

	struct ctor_predicate_t : public predicate_t {
		typedef ptr<const ctor_predicate_t> ref;
		typedef std::vector<ref> refs;

		static const syntax_kind_t SK = sk_ctor_predicate;
		static ptr<const predicate_t> parse(parse_state_t &ps);
		virtual bool resolve_match(
				llvm::IRBuilder<> &builder,
				runnable_scope_t::ref scope,
				life_t::ref life,
				location_t value_location,
				bound_var_t::ref input_value,
				llvm::BasicBlock *llvm_match_block,
				llvm::BasicBlock *llvm_no_match_block,
				runnable_scope_t::ref *scope_if_true) const;
		virtual std::string repr() const;
		virtual void render(render_state_t &rs) const;
		virtual match::Pattern::ref get_pattern(types::type_t::ref type, env_t::ref env) const;

		std::vector<predicate_t::ref> params;
	};

	struct expression_t : public statement_t, public condition_t {
		typedef ptr<const expression_t> ref;

		static const syntax_kind_t SK = sk_expression;
		virtual ~expression_t() {}
		static ptr<expression_t> parse(parse_state_t &ps);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const = 0;
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const = 0;

		/* when resolve_condition is not overriden, it just proxies through to resolve_expression */
		virtual bound_var_t::ref resolve_condition(
				llvm::IRBuilder<> &builder,
				runnable_scope_t::ref block_scope,
				life_t::ref life,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *scope_if_true,
				runnable_scope_t::ref *scope_if_false) const;
	};

	namespace postfix_expr {
		ptr<expression_t> parse(parse_state_t &ps);
	}

	struct continue_flow_t : public statement_t {
		typedef ptr<const continue_flow_t> ref;

		static const syntax_kind_t SK = sk_continue_flow;
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;
	};

	struct break_flow_t : public statement_t {
		typedef ptr<const break_flow_t> ref;

		static const syntax_kind_t SK = sk_break_flow;
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;
	};

	struct typeid_expr_t : public expression_t {
		typedef ptr<const typeid_expr_t> ref;

		static const syntax_kind_t SK = sk_typeid_expr;
		typeid_expr_t(ptr<expression_t> expr);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const;
		virtual void render(render_state_t &rs) const;
		static ptr<typeid_expr_t> parse(parse_state_t &ps);

		ptr<expression_t> expr;
	};

	struct sizeof_expr_t : public expression_t {
		typedef ptr<const typeid_expr_t> ref;

		static const syntax_kind_t SK = sk_sizeof;
		sizeof_expr_t(types::type_t::ref type);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const;
		virtual void render(render_state_t &rs) const;
		static ptr<sizeof_expr_t> parse(parse_state_t &ps);

		types::type_t::ref type;
	};

	struct callsite_expr_t : public expression_t {
		typedef ptr<const callsite_expr_t> ref;

		static const syntax_kind_t SK = sk_callsite_expr;
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const;
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> function_expr;
		std::vector<ptr<expression_t>> params;
	};

	struct return_statement_t : public statement_t {
		typedef ptr<const return_statement_t> ref;

		static const syntax_kind_t SK = sk_return_statement;
		static ptr<return_statement_t> parse(parse_state_t &ps);
		ptr<expression_t> expr;
		virtual void render(render_state_t &rs) const;

		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
	};

	struct unreachable_t : public statement_t {
		typedef ptr<const unreachable_t> ref;

		static const syntax_kind_t SK = sk_unreachable;
		static ptr<unreachable_t> parse(parse_state_t &ps);
		virtual void render(render_state_t &rs) const;

		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
	};

	struct type_decl_t : public item_t {
		typedef ptr<const type_decl_t> ref;

		type_decl_t(identifier::refs type_variables);
		static const syntax_kind_t SK = sk_type_decl;

		static ref parse(parse_state_t &ps, token_t name_token);
		virtual void render(render_state_t &rs) const;

		identifier::refs type_variables;
	};

	struct cast_expr_t : public expression_t {
		typedef ptr<const cast_expr_t> ref;

		static const syntax_kind_t SK = sk_cast_expr;
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const;
		virtual void render(render_state_t &rs) const;

		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		ptr<expression_t> lhs;
		types::type_t::ref type_cast;
		bool force_cast = false;
	};

	struct dimension_t : public item_t {
		typedef ptr<const dimension_t> ref;
		static const syntax_kind_t SK = sk_dimension;
		dimension_t(std::string name, types::type_t::ref type);
		virtual ~dimension_t() throw() {}
		virtual void render(render_state_t &rs) const;

		static ref parse(parse_state_t &ps, identifier::set generics);

		std::string name;
		types::type_t::ref type;
	};

	struct type_algebra_t : public item_t {
		typedef ptr<const type_algebra_t> ref;

		virtual ~type_algebra_t() throw() {}

		/* register_type is called from within the scope where the type's
		 * ctors should end up living. this function should create the
		 * unchecked ctors with the type. */
		virtual void register_type(
				llvm::IRBuilder<> &builder,
				identifier::ref supertype_id,
				identifier::refs type_variables,
				scope_t::ref scope) const = 0;

		static ref parse(parse_state_t &ps, ast::type_decl_t::ref type_decl);
	};

	struct data_type_t : public type_algebra_t {
		typedef ptr<const data_type_t> ref;

		virtual ~data_type_t() throw() {}
		static const syntax_kind_t SK = sk_data_type;
		static ref parse(parse_state_t &ps, type_decl_t::ref type_decl, identifier::refs type_variables);
		virtual void register_type(
				llvm::IRBuilder<> &builder,
				identifier::ref supertype_id,
				identifier::refs type_variables,
				scope_t::ref scope) const;
		virtual void render(render_state_t &rs) const;

		std::vector<std::pair<token_t, types::type_args_t::ref>> ctor_pairs;
	};

	struct type_product_t : public type_algebra_t {
		typedef ptr<const type_product_t> ref;

		type_product_t(bool native, types::type_t::ref type, identifier::set type_variables);
		virtual ~type_product_t() throw() {}
		static const syntax_kind_t SK = sk_type_product;
		static ref parse(parse_state_t &ps, type_decl_t::ref type_decl, identifier::refs type_variables, bool native);
		virtual void register_type(
				llvm::IRBuilder<> &builder,
				identifier::ref supertype_id,
				identifier::refs type_variables,
				scope_t::ref scope) const;
		virtual void render(render_state_t &rs) const;

		bool native;
		types::type_t::ref type;
		// identifier::set type_variables;
	};

	struct type_alias_t : public type_algebra_t {
		typedef ptr<const type_alias_t> ref;

		virtual ~type_alias_t() throw() {}
		static const syntax_kind_t SK = sk_type_alias;
		static ref parse(parse_state_t &ps, type_decl_t::ref type_decl, identifier::refs type_variables);
		virtual void register_type(
				llvm::IRBuilder<> &builder,
				identifier::ref supertype_id,
				identifier::refs type_variables,
				scope_t::ref scope) const;
		virtual void render(render_state_t &rs) const;

		types::type_t::ref type;
		identifier::set type_variables;
	};

	struct type_link_t : public type_algebra_t {
		typedef ptr<const type_link_t> ref;

		virtual ~type_link_t() throw() {}
		static const syntax_kind_t SK = sk_type_link;
		static ref parse(parse_state_t &ps, type_decl_t::ref type_decl, identifier::refs type_variables);
		virtual void register_type(
				llvm::IRBuilder<> &builder,
				identifier::ref supertype_id,
				identifier::refs type_variables,
				scope_t::ref scope) const;
		virtual void render(render_state_t &rs) const;
	};

	struct type_def_t : public statement_t {
		typedef ptr<const type_def_t> ref;

		static const syntax_kind_t SK = sk_type_def;
		static ptr<type_def_t> parse(parse_state_t &ps);
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		type_decl_t::ref type_decl;
		type_algebra_t::ref type_algebra;
	};

	struct var_decl_t : public virtual statement_t, public condition_t {
		typedef ptr<const var_decl_t> ref;

		static const syntax_kind_t SK = sk_var_decl;
		static ptr<var_decl_t> parse(parse_state_t &ps, bool is_let);
		static ptr<var_decl_t> parse_param(parse_state_t &ps);
		bound_var_t::ref resolve_as_link(
				llvm::IRBuilder<> &builder,
				module_scope_t::ref module_scope);
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual bound_var_t::ref resolve_condition(
				llvm::IRBuilder<> &builder,
				runnable_scope_t::ref block_scope,
				life_t::ref life,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *scope_if_true,
				runnable_scope_t::ref *scope_if_false) const;
		virtual void render(render_state_t &rs) const;

		virtual std::string get_symbol() const;
		virtual types::type_t::ref get_type() const;
		location_t get_location() const;
		virtual bool is_let() const { return is_let_var; }

		/* the inherited ast::item::token member contains the actual identifier
		 * name */

		/* is_let defines whether the name of this variable can be repointed at some other value */
		bool is_let_var = false;

		/* type describes the type of the value this name refers to */
		types::type_t::ref type;

		/* how should this variable be initialized? */
		ptr<expression_t> initializer;

		/* for module variables, extends_module describes the module that this variable should be injected into */
		identifier::ref extends_module;
	};

	struct defer_t : public statement_t {
		typedef ptr<const defer_t> ref;

		static const syntax_kind_t SK = sk_defer;
		static ptr<statement_t> parse(parse_state_t &ps);
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> callable;
	};

	struct assignment_t : public statement_t {
		typedef ptr<const assignment_t> ref;

		static const syntax_kind_t SK = sk_assignment;
		static ptr<statement_t> parse(parse_state_t &ps);
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> lhs, rhs;
	};

	struct plus_assignment_t : public statement_t {
		typedef ptr<const plus_assignment_t> ref;

		static const syntax_kind_t SK = sk_plus_assignment;
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> lhs, rhs;
	};

	struct times_assignment_t : public statement_t {
		typedef ptr<const times_assignment_t> ref;

		static const syntax_kind_t SK = sk_times_assignment;
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> lhs, rhs;
	};

	struct divide_assignment_t : public statement_t {
		typedef ptr<const divide_assignment_t> ref;

		static const syntax_kind_t SK = sk_divide_assignment;
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> lhs, rhs;
	};

	struct minus_assignment_t : public statement_t {
		typedef ptr<const minus_assignment_t> ref;

		static const syntax_kind_t SK = sk_minus_assignment;
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> lhs, rhs;
	};

	struct mod_assignment_t : public statement_t {
		typedef ptr<const mod_assignment_t> ref;

		static const syntax_kind_t SK = sk_mod_assignment;
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> lhs, rhs;
	};

	struct block_t : public expression_t {
		typedef ptr<const block_t> ref;

		static const syntax_kind_t SK = sk_block;

		static ptr<block_t> parse(parse_state_t &ps, bool expression_means_return=false);
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const;
		bound_var_t::ref resolve_block_expr(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				bool *returns,
				types::type_t::ref expected_type) const;
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual void render(render_state_t &rs) const;

		std::vector<ptr<statement_t>> statements;
	};

	struct function_decl_t : public item_t {
		typedef ptr<const function_decl_t> ref;

		static const syntax_kind_t SK = sk_function_decl;
		static ptr<function_decl_t> parse(parse_state_t &ps, bool within_expression, types::type_t::ref default_return_type);

		virtual void render(render_state_t &rs) const;

		types::type_t::ref function_type;
		identifier::ref extends_module;
		token_t link_to_name;

		std::string get_function_name() const;
	};

	struct function_defn_t : public expression_t, public can_reference_overloads_t {
		typedef ptr<const function_defn_t> ref;

		static const syntax_kind_t SK = sk_function_defn;

		static ptr<function_defn_t> parse(parse_state_t &ps, bool within_expression);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const;
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual bound_var_t::ref resolve_function(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_closure,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual bound_var_t::ref resolve_overrides(
				llvm::IRBuilder<> &builder,
				scope_t::ref scope,
				life_t::ref,
				const ptr<const ast::item_t> &obj,
				const bound_type_t::refs &args,
				types::type_t::ref return_type) const;
		virtual types::type_function_t::ref resolve_arg_types_from_overrides(
				scope_t::ref scope,
				location_t location,
				types::type_t::refs args,
				types::type_t::ref return_type) const;
		virtual void render(render_state_t &rs) const;

		ptr<function_decl_t> decl;
		ptr<block_t> block;
	};

	struct if_block_t : public statement_t {
		typedef ptr<const if_block_t> ref;

		static const syntax_kind_t SK = sk_if_block;

		static ptr<if_block_t> parse(parse_state_t &ps);
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<const condition_t> condition;
		ptr<const block_t> block;
		ptr<const statement_t> else_;
	};

	struct while_block_t : public statement_t {
		typedef ptr<const while_block_t> ref;

		static const syntax_kind_t SK = sk_while_block;

		static ptr<while_block_t> parse(parse_state_t &ps);
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<const condition_t> condition;
		ptr<block_t> block;
	};

	struct pattern_block_t : public item_t {
		typedef ptr<const pattern_block_t> ref;
		typedef std::vector<ref> refs;

		static const syntax_kind_t SK = sk_pattern_block;

		static ref parse(parse_state_t &ps);
		virtual void render(render_state_t &rs) const;
		
		predicate_t::ref predicate;
		ptr<block_t> block;
	};

	struct match_expr_t : public expression_t {
		typedef ptr<const match_expr_t> ref;

		static const syntax_kind_t SK = sk_when_block;

		static ptr<match_expr_t> parse(parse_state_t &ps);
		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const;
		bound_var_t::ref resolve_match_expr(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				bool *returns,
				types::type_t::ref expected_type) const;
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> value;
		pattern_block_t::refs pattern_blocks;
	};

	struct semver_t : public item_t {
		typedef ptr<const semver_t> ref;
		virtual void render(render_state_t &rs) const;

		static const syntax_kind_t SK = sk_semver;
		static ptr<semver_t> parse(parse_state_t &ps);
	};

	struct module_decl_t : public item_t {
		typedef ptr<const module_decl_t> ref;

		static const syntax_kind_t SK = sk_module_decl;

		static ptr<module_decl_t> parse(parse_state_t &ps, bool skip_module_token=false);
		virtual void render(render_state_t &rs) const;

		ptr<semver_t> semver;
		std::string get_canonical_name() const;
		token_t get_name() const;

		token_t name;
		bool global = false;
	};

	struct link_module_statement_t : public statement_t {
		typedef ptr<const link_module_statement_t> ref;

		static const syntax_kind_t SK = sk_link_module_statement;

		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		token_t link_as_name;
		ptr<module_decl_t> extern_module;
		identifier::refs symbols;
	};

	struct link_name_t : public statement_t {
		typedef ptr<const link_name_t> ref;

		static const syntax_kind_t SK = sk_link_name;

		virtual void resolve_statement(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				runnable_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		token_t local_name;
		ptr<module_decl_t> extern_module;
		token_t remote_name;
	};

	struct link_function_statement_t : public expression_t {
		typedef ptr<const link_function_statement_t> ref;

		static const syntax_kind_t SK = sk_link_function_statement;

		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const;
		virtual void render(render_state_t &rs) const;

		ptr<function_decl_t> extern_function;
	};

	struct link_var_statement_t : public expression_t {
		typedef ptr<const link_var_statement_t> ref;

		static const syntax_kind_t SK = sk_link_var_statement;

		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const;
		virtual void render(render_state_t &rs) const;

		ptr<var_decl_t> var_decl;
	};

	struct module_t : public std::enable_shared_from_this<module_t>, public item_t {
		typedef ptr<const module_t> ref;

		static const syntax_kind_t SK = sk_module;

		module_t(const std::string filename, bool global=false);
		static ptr<module_t> parse(parse_state_t &ps);
		std::string get_canonical_name() const;
		virtual void render(render_state_t &rs) const;

		bool global;
		std::string filename;
		std::string module_key;

		ptr<module_decl_t> decl;
		std::vector<ptr<var_decl_t>> var_decls;
		std::vector<ptr<type_def_t>> type_defs;
		std::vector<ptr<function_defn_t>> functions;
		std::vector<ptr<link_module_statement_t>> linked_modules;
		std::vector<ptr<link_function_statement_t>> linked_functions;
		std::vector<ptr<link_var_statement_t>> linked_vars;
		std::vector<ptr<const link_name_t>> linked_names;
	};

	struct program_t : public item_t {
		typedef ptr<const program_t> ref;

		static const syntax_kind_t SK = sk_program;
		virtual ~program_t() {}
		virtual void render(render_state_t &rs) const;

		std::vector<ptr<const module_t>> modules;
	};

	struct dot_expr_t : public expression_t, public can_reference_overloads_t {
		typedef ptr<const dot_expr_t> ref;

		static const syntax_kind_t SK = sk_dot_expr;
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const;
        virtual bound_var_t::ref resolve_overrides(
				llvm::IRBuilder<> &builder,
				scope_t::ref scope,
				life_t::ref,
				const ptr<const ast::item_t> &obj,
				const bound_type_t::refs &args,
				types::type_t::ref return_type) const;
		virtual types::type_function_t::ref resolve_arg_types_from_overrides(
				scope_t::ref scope,
				location_t location,
				types::type_t::refs args,
				types::type_t::ref return_type) const;
		virtual void render(render_state_t &rs) const;

		ptr<ast::expression_t> lhs;
		token_t rhs;
	};

	struct tuple_expr_t : public expression_t {
		typedef ptr<const tuple_expr_t> ref;

		static const syntax_kind_t SK = sk_tuple_expr;
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		static ptr<ast::expression_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const;
		virtual void render(render_state_t &rs) const;

		std::vector<ptr<ast::expression_t>> values;
	};

	struct ternary_expr_t : public expression_t {
		typedef ptr<const ternary_expr_t> ref;

		static const syntax_kind_t SK = sk_ternary_expr;
		static ptr<ast::expression_t> parse(parse_state_t &ps);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_condition(
				llvm::IRBuilder<> &builder,
				runnable_scope_t::ref block_scope,
				life_t::ref life,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *scope_if_true,
				runnable_scope_t::ref *scope_if_false) const;
		virtual void render(render_state_t &rs) const;

		ptr<ast::expression_t> condition, when_true, when_false;
	};

	struct or_expr_t : public expression_t {
		typedef ptr<const or_expr_t> ref;

		static const syntax_kind_t SK = sk_or_expr;
		static ptr<ast::expression_t> parse(parse_state_t &ps);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_condition(
				llvm::IRBuilder<> &builder,
				runnable_scope_t::ref block_scope,
				life_t::ref life,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *scope_if_true,
				runnable_scope_t::ref *scope_if_false) const;
		virtual void render(render_state_t &rs) const;

		ptr<ast::expression_t> lhs, rhs;
	};

	struct and_expr_t : public expression_t {
		typedef ptr<const and_expr_t> ref;

		static const syntax_kind_t SK = sk_and_expr;
		static ptr<ast::expression_t> parse(parse_state_t &ps);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_condition(
				llvm::IRBuilder<> &builder,
				runnable_scope_t::ref block_scope,
				life_t::ref life,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *scope_if_true,
				runnable_scope_t::ref *scope_if_false) const;
		virtual void render(render_state_t &rs) const;

		ptr<ast::expression_t> lhs, rhs;
	};

	struct binary_operator_t : public expression_t {
		typedef ptr<const binary_operator_t> ref;

		static const syntax_kind_t SK = sk_binary_operator;
		static ptr<ast::expression_t> parse(parse_state_t &ps);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_condition(
				llvm::IRBuilder<> &builder,
				runnable_scope_t::ref block_scope,
				life_t::ref life,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *scope_if_true,
				runnable_scope_t::ref *scope_if_false) const;
		virtual void render(render_state_t &rs) const;

		std::string function_name;
		ptr<ast::expression_t> lhs, rhs;
	};

	struct prefix_expr_t : public expression_t {
		typedef ptr<const prefix_expr_t> ref;

		static const syntax_kind_t SK = sk_prefix_expr;
		static ptr<ast::expression_t> parse(parse_state_t &ps);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_condition(
				llvm::IRBuilder<> &builder,
				runnable_scope_t::ref block_scope,
				life_t::ref life,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *scope_if_true,
				runnable_scope_t::ref *scope_if_false) const;
		virtual void render(render_state_t &rs) const;

		ptr<ast::expression_t> rhs;

	private:
		virtual bound_var_t::ref resolve_prefix_expr(
				llvm::IRBuilder<> &builder,
				runnable_scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *scope_if_true,
				runnable_scope_t::ref *scope_if_false) const;
	};

	struct typeinfo_expr_t : public expression_t {
		typedef ptr<const typeinfo_expr_t> ref;

		static const syntax_kind_t SK = sk_typeinfo_expr;
		static ptr<typeinfo_expr_t> parse(parse_state_t &ps);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const;
		virtual void render(render_state_t &rs) const;

		types::type_t::ref type;
		types::type_t::ref underlying_type;
		token_t finalize_function;
		token_t mark_function;
	};

	struct reference_expr_t : public expression_t, public can_reference_overloads_t {
		typedef ptr<const reference_expr_t> ref;

		static const syntax_kind_t SK = sk_reference_expr;
		static ptr<expression_t> parse(parse_state_t &ps);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_overrides(
				llvm::IRBuilder<> &builder,
				scope_t::ref scope,
				life_t::ref,
				const ptr<const ast::item_t> &obj,
				const bound_type_t::refs &args,
				types::type_t::ref return_type) const;
		virtual bound_var_t::ref resolve_condition(
				llvm::IRBuilder<> &builder,
				runnable_scope_t::ref block_scope,
				life_t::ref life,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *scope_if_true,
				runnable_scope_t::ref *scope_if_false) const;
		bound_var_t::ref resolve_reference(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type,
				runnable_scope_t::ref *scope_if_true,
				runnable_scope_t::ref *scope_if_false) const;
		virtual types::type_function_t::ref resolve_arg_types_from_overrides(
				scope_t::ref scope,
				location_t location,
				types::type_t::refs args,
				types::type_t::ref return_type) const;
		virtual void render(render_state_t &rs) const;
	};

	struct literal_expr_t : public expression_t, public predicate_t {
		typedef ptr<const literal_expr_t> ref;

		static const syntax_kind_t SK = sk_literal_expr;
		static ptr<expression_t> parse(parse_state_t &ps);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const;
		virtual bool resolve_match(
				llvm::IRBuilder<> &builder,
				runnable_scope_t::ref scope,
				life_t::ref life,
				location_t value_location,
				bound_var_t::ref input_value,
				llvm::BasicBlock *llvm_match_block,
				llvm::BasicBlock *llvm_no_match_block,
				runnable_scope_t::ref *scope_if_true) const;
		virtual std::string repr() const;
		virtual void render(render_state_t &rs) const;

		virtual match::Pattern::ref get_pattern(types::type_t::ref type, env_t::ref env) const;
	};

	struct array_literal_expr_t : public expression_t {
		typedef ptr<const array_literal_expr_t> ref;

		static const syntax_kind_t SK = sk_array_literal_expr;
		static ptr<expression_t> parse(parse_state_t &ps);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const;
		virtual void render(render_state_t &rs) const;

		std::vector<ptr<expression_t>> items;
	};

    struct bang_expr_t : public expression_t {
		typedef ptr<const bang_expr_t> ref;

		static const syntax_kind_t SK = sk_bang_expr;
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> lhs;
    };

	struct array_index_expr_t : public expression_t {
		typedef ptr<const array_index_expr_t> ref;

		static const syntax_kind_t SK = sk_array_index_expr;
		static ptr<expression_t> parse(parse_state_t &ps);
		virtual types::type_t::ref resolve_type(scope_t::ref scope, types::type_t::ref expected_type) const;
		virtual bound_var_t::ref resolve_expression(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				types::type_t::ref expected_type) const;
		bound_var_t::ref resolve_assignment(
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				bool as_ref,
				const ast::expression_t::ref &rhs,
				types::type_t::ref expected_type) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> lhs;
		ptr<expression_t> start, stop;
	};

	namespace base_expr {
		ptr<expression_t> parse(parse_state_t &ps);
	}
}
