#pragma once
#include "zion.h"
#include "ast_decls.h"
#include "token.h"
#include <vector>
#include <unordered_set>
#include <string>
#include "dbg.h"
#include <memory>
#include "scopes.h"
#include "type_checker.h"
#include "callable.h"
#include "life.h"

struct parse_state_t;

namespace ast {
	struct render_state_t;
	struct item_t : std::enable_shared_from_this<item_t> {
		typedef ptr<const item_t> ref;

		virtual ~item_t() throw() = 0;
		std::string str() const;
		virtual void render(render_state_t &rs) const = 0;
		location_t get_location() const { return token.location; }

		token_t token;
	};

	void log_named_item_create(const char *type, const std::string &name);

	template <typename T>
	ptr<T> create(const token_t &token) {
		auto item = ptr<T>(new T());
		item->token = token;
		return item;
	}

	template <typename T, typename... Args>
	ptr<T> create(const token_t &token, Args... args) {
		auto item = ptr<T>(new T(args...));
		item->token = token;
		return item;
	}

	struct statement_t : public item_t {
		typedef ptr<const statement_t> ref;

		virtual ~statement_t() {}
		static ptr<ast::statement_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_statement(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const = 0;
	};

	struct module_t;
	struct expression_t;
	struct var_decl_t;

	struct param_list_decl_t : public item_t {
		typedef ptr<const param_list_decl_t> ref;

		static ptr<param_list_decl_t> parse(parse_state_t &ps);
		virtual void render(render_state_t &rs) const;

		std::vector<ptr<var_decl_t>> params;
	};

	struct param_list_t : public item_t {
		typedef ptr<const param_list_t> ref;

		static ptr<param_list_t> parse(parse_state_t &ps);
		virtual void render(render_state_t &rs) const;

		std::vector<ptr<expression_t>> expressions;
	};

	struct rhs_expression_t : public lhs_expression_t {
		typedef ptr<const rhs_expression_t> ref;

		virtual ~rhs_expression_t() {}
		static ptr<rhs_expression_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_expression(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref scope,
				life_t::ref life) const = 0;
	};

	struct continue_flow_t : public statement_t {
		typedef ptr<const continue_flow_t> ref;

		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;
	};

	struct break_flow_t : public statement_t {
		typedef ptr<const break_flow_t> ref;

		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;
	};

	struct pass_flow_t : public statement_t {
		typedef ptr<const pass_flow_t> ref;

		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;
	};

	struct typeid_expr_t : public expression_t {
		typedef ptr<const typeid_expr_t> ref;

		typeid_expr_t(ptr<expression_t> expr);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;
		static ptr<typeid_expr_t> parse(parse_state_t &ps);

		ptr<expression_t> expr;
	};

	struct sizeof_expr_t : public expression_t {
		typedef ptr<const typeid_expr_t> ref;

		sizeof_expr_t(types::type_t::ref type);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;
		static ptr<sizeof_expr_t> parse(parse_state_t &ps);

		types::type_t::ref type;
	};

	struct callsite_expr_t : public expression_t {
		typedef ptr<const callsite_expr_t> ref;

		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> function_expr;
		ptr<param_list_t> params;
	};

	struct return_statement_t : public statement_t {
		typedef ptr<const return_statement_t> ref;

		static ptr<return_statement_t> parse(parse_state_t &ps);
		ptr<expression_t> expr;
		virtual void render(render_state_t &rs) const;

		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
	};

	struct type_decl_t : public item_t {
		typedef ptr<const type_decl_t> ref;

		type_decl_t(identifier::refs type_variables);

		static ref parse(parse_state_t &ps, token_t name_token);
		virtual void render(render_state_t &rs) const;

		identifier::refs type_variables;
	};

	struct cast_expr_t : public expression_t {
		typedef ptr<const cast_expr_t> ref;

		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> lhs;
		types::type_t::ref type_cast;
		bool force_cast = false;
	};

	struct dimension_t : public item_t {
		typedef ptr<const dimension_t> ref;
		dimension_t(atom name, types::type_t::ref type);
		virtual ~dimension_t() throw() {}
		virtual void render(render_state_t &rs) const;

		static ref parse(parse_state_t &ps, identifier::set generics);

		atom name;
		types::type_t::ref type;
	};

	struct type_algebra_t : public item_t {
		typedef ptr<const type_algebra_t> ref;

		virtual ~type_algebra_t() throw() {}

		/* register_type is called from within the scope where the type's
		 * ctors should end up living. this function should create the
		 * unchecked ctors with the type. */
		virtual void register_type(
				status_t &status,
				llvm::IRBuilder<> &builder,
				identifier::ref supertype_id,
				identifier::refs type_variables,
				scope_t::ref scope) const = 0;

		static ref parse(parse_state_t &ps, ast::type_decl_t::ref type_decl);
	};

	struct type_sum_t : public type_algebra_t {
		typedef ptr<const type_sum_t> ref;

		type_sum_t(types::type_t::ref type);
		virtual ~type_sum_t() throw() {}
		static ref parse(parse_state_t &ps, type_decl_t::ref type_decl, identifier::refs type_variables);
		virtual void register_type(
				status_t &status,
				llvm::IRBuilder<> &builder,
				identifier::ref supertype_id,
				identifier::refs type_variables,
				scope_t::ref scope) const;
		virtual void render(render_state_t &rs) const;

		types::type_t::ref type;
	};

	struct type_product_t : public type_algebra_t {
		typedef ptr<const type_product_t> ref;

		type_product_t(types::type_t::ref type, identifier::set type_variables);
		virtual ~type_product_t() throw() {}
		static ref parse(parse_state_t &ps, type_decl_t::ref type_decl, identifier::refs type_variables);
		virtual void register_type(
				status_t &status,
				llvm::IRBuilder<> &builder,
				identifier::ref supertype_id,
				identifier::refs type_variables,
				scope_t::ref scope) const;
		virtual void render(render_state_t &rs) const;

		types::type_t::ref type;
		identifier::set type_variables;
	};

	struct type_alias_t : public type_algebra_t {
		typedef ptr<const type_alias_t> ref;

		virtual ~type_alias_t() throw() {}
		static ref parse(parse_state_t &ps, type_decl_t::ref type_decl, identifier::refs type_variables);
		virtual void register_type(
				status_t &status,
				llvm::IRBuilder<> &builder,
				identifier::ref supertype_id,
				identifier::refs type_variables,
				scope_t::ref scope) const;
		virtual void render(render_state_t &rs) const;

		types::type_t::ref type;
		identifier::set type_variables;
	};

	struct type_def_t : public statement_t {
		typedef ptr<const type_def_t> ref;

		static ptr<type_def_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		type_decl_t::ref type_decl;
		type_algebra_t::ref type_algebra;
	};

	struct tag_t : public statement_t {
		typedef ptr<const tag_t> ref;

		static ptr<tag_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;
		// TODO: track type variables on tags to aid in deserialization and marshalling
	};

	struct var_decl_t : public statement_t {
		typedef ptr<const var_decl_t> ref;

		static ptr<var_decl_t> parse(parse_state_t &ps);
		static ptr<var_decl_t> parse_param(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		bound_var_t::ref resolve_as_condition(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope) const;
		virtual void render(render_state_t &rs) const;

		virtual atom get_symbol() const;
		virtual types::type_t::ref get_type() const;
		location_t get_location() const;
		virtual bool has_initializer() const;
		virtual bound_var_t::ref resolve_initializer(
				status_t &status,
				llvm::IRBuilder<> &builder,
			   	scope_t::ref scope,
			   	life_t::ref life) const;
		/* the inherited ast::item::token member contains the actual identifier
		 * name */
		types::type_t::ref type;
		ptr<expression_t> initializer;
	};

	struct assignment_t : public statement_t {
		typedef ptr<const assignment_t> ref;

		static ptr<expression_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<reference_expr_t> lhs;
		ptr<expression_t> rhs;
	};

	struct plus_assignment_t : public expression_t {
		typedef ptr<const plus_assignment_t> ref;

		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> lhs, rhs;
	};

	struct times_assignment_t : public expression_t {
		typedef ptr<const times_assignment_t> ref;

		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> lhs, rhs;
	};

	struct divide_assignment_t : public expression_t {
		typedef ptr<const divide_assignment_t> ref;

		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> lhs, rhs;
	};

	struct minus_assignment_t : public expression_t {
		typedef ptr<const minus_assignment_t> ref;

		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> lhs, rhs;
	};

	struct mod_assignment_t : public expression_t {
		typedef ptr<const mod_assignment_t> ref;

		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> lhs, rhs;
	};

	struct block_t : public statement_t {
		typedef ptr<const block_t> ref;


		static ptr<block_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		std::vector<ptr<statement_t>> statements;
	};

	struct function_decl_t : public item_t {
		typedef ptr<const function_decl_t> ref;

		static ptr<function_decl_t> parse(parse_state_t &ps);

		virtual void render(render_state_t &rs) const;

		types::type_t::ref return_type;
		ptr<param_list_decl_t> param_list_decl;
		types::type_t::ref inbound_context;
	};

	struct function_defn_t : public expression_t {
		typedef ptr<const function_defn_t> ref;


		static ptr<function_defn_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		bound_var_t::ref instantiate_with_args_and_return_type(
				status_t &status,
			   	llvm::IRBuilder<> &builder,
			   	scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				types::type_t::ref inbound_context,
				bound_type_t::named_pairs args,
				bound_type_t::ref return_type) const;
		virtual void render(render_state_t &rs) const;

		ptr<function_decl_t> decl;
		ptr<block_t> block;
	};

	struct if_block_t : public statement_t {
		typedef ptr<const if_block_t> ref;


		static ptr<if_block_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> condition;
		ptr<block_t> block;
		ptr<statement_t> else_;
	};

	struct while_block_t : public statement_t {
		typedef ptr<const while_block_t> ref;


		static ptr<while_block_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> condition;
		ptr<block_t> block;
	};

	struct for_block_t : public statement_t {
		typedef ptr<const for_block_t> ref;


		static ptr<for_block_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		token_t var_token;
		ptr<expression_t> collection;
		ptr<block_t> block;
	};

	struct pattern_block_t : public item_t {
		typedef ptr<const pattern_block_t> ref;
		typedef std::vector<ref> refs;


		static ref parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_pattern_block(
				status_t &status,
				llvm::IRBuilder<> &builder,
				bound_var_t::ref value,
				identifier::ref value_name,
				runnable_scope_t::ref scope,
				life_t::ref life,
				bool *returns,
				refs::const_iterator next_iter,
				refs::const_iterator end_iter,
				ptr<const block_t> else_block) const;
		virtual void render(render_state_t &rs) const;
		
		types::type_t::ref type;
		ptr<block_t> block;
	};

	struct when_block_t : public statement_t {
		typedef ptr<const when_block_t> ref;


		static ptr<when_block_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> value;
		pattern_block_t::refs pattern_blocks;
		ptr<block_t> else_block;
	};

	struct semver_t : public item_t {
		typedef ptr<const semver_t> ref;
		virtual void render(render_state_t &rs) const;

		static ptr<semver_t> parse(parse_state_t &ps);
	};

	struct module_decl_t : public item_t {
		typedef ptr<const module_decl_t> ref;

		static ptr<module_decl_t> parse(parse_state_t &ps, bool skip_module_token=false);
		virtual void render(render_state_t &rs) const;

		ptr<semver_t> semver;
		std::string get_canonical_name() const;
		token_t get_name() const;

		token_t name;
	};

	struct link_module_statement_t : public statement_t {
		typedef ptr<const link_module_statement_t> ref;

		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<module_decl_t> extern_module;
	};

	struct link_function_statement_t : public statement_t {
		typedef ptr<const link_function_statement_t> ref;


		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		token_t function_name;
		ptr<function_decl_t> extern_function;
	};

	struct module_t : public std::enable_shared_from_this<module_t>, public item_t {
		typedef ptr<const module_t> ref;


		module_t(const atom filename, bool global=false);
		static ptr<module_t> parse(parse_state_t &ps, bool global=false);
		std::string get_canonical_name() const;
		virtual void render(render_state_t &rs) const;

		bool global;
		atom filename;
		atom module_key;

		ptr<module_decl_t> decl;
		std::vector<ptr<type_def_t>> type_defs;
		std::vector<ptr<function_defn_t>> functions;
		std::vector<ptr<link_module_statement_t>> linked_modules;
		std::vector<ptr<link_function_statement_t>> linked_functions;
	};

	struct program_t : public item_t {
		typedef ptr<const program_t> ref;

		virtual ~program_t() {}
		virtual void render(render_state_t &rs) const;

		std::unordered_set<ptr<const module_t>> modules;
	};

	struct dot_expr_t : public expression_t, public can_reference_overloads_t {
		typedef ptr<const dot_expr_t> ref;

		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
        virtual bound_var_t::ref resolve_overrides(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref scope,
				life_t::ref,
				const ptr<const ast::item_t> &obj,
				const bound_type_t::refs &args) const;
		virtual void render(render_state_t &rs) const;

		ptr<ast::expression_t> lhs;
		token_t rhs;
	};

	struct tuple_expr_t : public expression_t {
		typedef ptr<const tuple_expr_t> ref;

		static ptr<ast::expression_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		std::vector<ptr<ast::expression_t>> values;
	};

	struct ternary_expr_t : public expression_t {
		typedef ptr<const ternary_expr_t> ref;

		static ptr<ast::expression_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<ast::expression_t> condition, when_true, when_false;
	};

	struct or_expr_t : public expression_t {
		typedef ptr<const or_expr_t> ref;

		static ptr<ast::expression_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<ast::expression_t> lhs, rhs;
	};

	struct and_expr_t : public expression_t {
		typedef ptr<const and_expr_t> ref;

		static ptr<ast::expression_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<ast::expression_t> lhs, rhs;
	};

	struct eq_expr_t : public expression_t {
		typedef ptr<const eq_expr_t> ref;

		static ptr<ast::expression_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<ast::expression_t> lhs, rhs;
		bool negated = false;
	};

	struct ineq_expr_t : public expression_t {
		typedef ptr<const ineq_expr_t> ref;

		static ptr<ast::expression_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<ast::expression_t> lhs, rhs;
	};

	struct plus_expr_t : public expression_t {
		typedef ptr<const plus_expr_t> ref;

		static ptr<ast::expression_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<ast::expression_t> lhs, rhs;
	};

	struct times_expr_t : public expression_t {
		typedef ptr<const times_expr_t> ref;

		static ptr<ast::expression_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<ast::expression_t> lhs, rhs;
	};

	struct prefix_expr_t : public expression_t {
		typedef ptr<const prefix_expr_t> ref;

		static ptr<ast::expression_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<ast::expression_t> rhs;
	};

	struct reference_expr_t : public expression_t, public can_reference_overloads_t {
		typedef ptr<const reference_expr_t> ref;

		static ptr<expression_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual bound_var_t::ref resolve_overrides(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref scope,
				life_t::ref,
				const ptr<const ast::item_t> &obj,
				const bound_type_t::refs &args) const;
		bound_var_t::ref resolve_as_condition(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope) const;
		virtual void render(render_state_t &rs) const;
	};

	struct literal_expr_t : public expression_t {
		typedef ptr<const literal_expr_t> ref;

		static ptr<expression_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;
	};

	struct array_literal_expr_t : public expression_t {
		typedef ptr<const array_literal_expr_t> ref;

		static ptr<expression_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		std::vector<ptr<expression_t>> items;
	};

    struct bang_expr_t : public expression_t {
		typedef ptr<const bang_expr_t> ref;

		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> lhs;
    };

	struct array_index_expr_t : public expression_t {
		typedef ptr<const array_index_expr_t> ref;

		static ptr<expression_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_instantiation(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual void render(render_state_t &rs) const;

		ptr<expression_t> lhs;
		ptr<expression_t> index;
	};

	namespace base_expr {
		ptr<expression_t> parse(parse_state_t &ps);
	}
}
