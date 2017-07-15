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
#include "code_id.h"

struct parse_state_t;

namespace ast {
	struct item_t {
		typedef ptr<const item_t> ref;

		virtual ~item_t() throw() = 0;
		location_t get_location() const { return get_token().location; }

		virtual void set_token(const token_t &token) = 0;
		virtual token_t get_token() const = 0;
		virtual std::string str() const { assert(false); return "CODE"; }
	};

	template <typename T=item_t>
	struct item_impl_t : public T {
		token_t token;

		virtual ~item_impl_t() {}

		void set_token(const token_t &token_) { 
			token = token_;
		}

		token_t get_token() const {
			return token;
		}
	};

	void log_named_item_create(const char *type, const std::string &name);

	template <typename T>
	ptr<T> create(const token_t &token) {
		auto item = ptr<T>(new T());
		item->set_token(token);
		return item;
	}

	template <typename T, typename... Args>
	ptr<T> create(const token_t &token, Args... args) {
		auto item = ptr<T>(new T(args...));
		item->set_token(token);
		return item;
	}

	struct statement_t : public virtual item_t {
		typedef ptr<const statement_t> ref;

		virtual ~statement_t() {}
		static ptr<const statement_t> parse(parse_state_t &ps);
		virtual void resolve_statement(
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
	struct reference_expr_t;

	struct param_list_t : public item_impl_t<item_t> {
		typedef ptr<const param_list_t> ref;

		static ptr<const param_list_t> parse(parse_state_t &ps);

		std::vector<ptr<const reference_expr_t>> expressions;
	};

	struct expression_t : public virtual item_t {
		typedef ptr<const expression_t> ref;

		virtual ~expression_t() {}
		static ptr<const expression_t> parse(parse_state_t &ps);
		// virtual identifier::ref get_type() const = 0;
		virtual bound_var_t::ref resolve_expression(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref scope,
				life_t::ref life) const = 0;
	};

	struct continue_flow_t : public item_impl_t<statement_t> {
		typedef ptr<const continue_flow_t> ref;

		virtual void resolve_statement(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
	};

	struct break_flow_t : public item_impl_t<statement_t> {
		typedef ptr<const break_flow_t> ref;

		virtual void resolve_statement(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
	};

	struct typeid_expr_t : public std::enable_shared_from_this<typeid_expr_t>, public item_impl_t<expression_t> {
		typedef ptr<const typeid_expr_t> ref;

		virtual bound_var_t::ref resolve_expression(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life) const;
		static ptr<const typeid_expr_t> parse(parse_state_t &ps);

		ptr<const reference_expr_t> expr;
	};

	struct sizeof_expr_t : public item_impl_t<expression_t> {
		typedef ptr<const typeid_expr_t> ref;

		virtual bound_var_t::ref resolve_expression(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life) const;
		static ptr<const sizeof_expr_t> parse(parse_state_t &ps);

		identifier::ref type_name;
	};

	struct callsite_expr_interface_t : public virtual expression_t, public virtual statement_t {
		virtual ~callsite_expr_interface_t() {}
	};

	struct callsite_expr_t : public std::enable_shared_from_this<callsite_expr_t>, public item_impl_t<callsite_expr_interface_t> {
		typedef ptr<const callsite_expr_t> ref;

		static ref parse(parse_state_t &ps, ptr<const reference_expr_t> ref_expr);
		virtual void resolve_statement(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
		virtual bound_var_t::ref resolve_expression(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life) const;

		ptr<const reference_expr_t> function_expr;
		ptr<const param_list_t> params;
	};

	struct return_statement_t : public std::enable_shared_from_this<return_statement_t>, public item_impl_t<statement_t> {
		typedef ptr<const return_statement_t> ref;

		static ptr<const return_statement_t> parse(parse_state_t &ps);
		ptr<const reference_expr_t> expr;

		virtual void resolve_statement(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;
	};

	struct type_decl_t : public item_impl_t<item_t> {
		typedef ptr<const type_decl_t> ref;

		type_decl_t();

		static ref parse(parse_state_t &ps, token_t name_token);
	};

	struct cast_expr_t : public item_impl_t<expression_t> {
		typedef ptr<const cast_expr_t> ref;

		virtual bound_var_t::ref resolve_expression(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref scope,
				life_t::ref life) const;

		ptr<const reference_expr_t> lhs;
		token_t type_cast;
		bool force_cast = false;
	};

	struct dimension_t : public item_impl_t<item_t> {
		typedef ptr<const dimension_t> ref;
		virtual ~dimension_t() throw() {}

		static ref parse(parse_state_t &ps);

		identifier::ref type_name;
	};

	struct user_defined_type_t : public item_impl_t<item_t> {
		virtual ~user_defined_type_t() throw() {}

		static ptr<const user_defined_type_t> parse(parse_state_t &ps);
		virtual void register_type(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref scope) const = 0;
		virtual identifier::ref get_type_name() const = 0;
		virtual types::type_t::ref get_type() const = 0;
	};

	struct polymorph_t : public item_impl_t<user_defined_type_t> {
		typedef ptr<const polymorph_t> ref;

		virtual ~polymorph_t() throw() {}
		static ref parse(parse_state_t &ps, token_t type_name);
		virtual void register_type(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref scope) const;
		virtual identifier::ref get_type_name() const;
		virtual types::type_t::ref get_type() const;

		identifier::set subtypes;
	};

	struct struct_t : public std::enable_shared_from_this<struct_t>, public item_impl_t<user_defined_type_t> {
		typedef ptr<const struct_t> ref;

		virtual ~struct_t() throw() {}
		static ref parse(parse_state_t &ps, token_t type_name);
		virtual void register_type(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref scope) const;
		virtual identifier::ref get_type_name() const;
		virtual types::type_t::ref get_type() const;

		std::vector<dimension_t::ref> dimensions;
	};

	struct var_decl_t : public item_impl_t<statement_t> {
		typedef ptr<const var_decl_t> ref;

		static ptr<const var_decl_t> parse(parse_state_t &ps);
		virtual void resolve_statement(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;

		virtual atom get_symbol() const;
		location_t get_location() const;
		virtual bound_var_t::ref resolve_initializer(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref scope,
				life_t::ref life) const;
		/* the inherited ast::item::token member contains the actual identifier
		 * name */
		ptr<const expression_t> initializer;
	};

	struct assignment_t : public item_impl_t<statement_t> {
		typedef ptr<const assignment_t> ref;

		static ptr<const statement_t> parse(parse_state_t &ps);
		virtual void resolve_statement(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;

		ptr<const reference_expr_t> lhs;
		ptr<const expression_t> rhs;
	};

	struct block_t : public item_impl_t<statement_t> {
		typedef ptr<const block_t> ref;


		static ptr<const block_t> parse(parse_state_t &ps);
		virtual void resolve_statement(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;

		std::vector<ptr<const statement_t>> statements;
	};

	struct function_decl_t : public item_impl_t<item_t> {
		typedef ptr<const function_decl_t> ref;

		static ptr<const function_decl_t> parse(parse_state_t &ps);

		std::vector<dimension_t::ref> params;
		identifier::ref return_type_name;
	};

	struct function_defn_t : public std::enable_shared_from_this<function_defn_t>, public item_impl_t<item_t> {
		typedef ptr<const function_defn_t> ref;

		static ptr<const function_defn_t> parse(parse_state_t &ps);
		virtual void resolve_function_defn(
				status_t &status,
				llvm::IRBuilder<> &builder,
				module_scope_t::ref scope) const;
		bound_var_t::ref instantiate_with_args_and_return_type(
				status_t &status,
			   	llvm::IRBuilder<> &builder,
			   	scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bound_type_t::named_pairs args,
				bound_type_t::ref return_type) const;

		ptr<const function_decl_t> decl;
		ptr<const block_t> block;
	};

	struct if_block_t : public item_impl_t<statement_t> {
		typedef ptr<const if_block_t> ref;

		static ptr<const if_block_t> parse(parse_state_t &ps);
		virtual void resolve_statement(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;

		ptr<const reference_expr_t> condition;
		ptr<const block_t> block;
		ptr<const statement_t> else_;
	};

	struct loop_block_t : public item_impl_t<statement_t> {
		typedef ptr<const loop_block_t> ref;

		static ptr<const loop_block_t> parse(parse_state_t &ps);
		virtual void resolve_statement(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;

		ptr<const block_t> block;
	};

	struct pattern_block_t : public item_impl_t<item_t> {
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
		
		identifier::ref get_type_name() const { return make_code_id(token); }
		ptr<const block_t> block;
	};

	struct match_block_t : public item_impl_t<statement_t> {
		typedef ptr<const match_block_t> ref;

		static ptr<const match_block_t> parse(parse_state_t &ps);
		virtual void resolve_statement(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life,
				local_scope_t::ref *new_scope,
				bool *returns) const;

		ptr<const reference_expr_t> value;
		pattern_block_t::refs pattern_blocks;
	};

	struct module_decl_t : public item_impl_t<item_t> {
		typedef ptr<const module_decl_t> ref;

		static ptr<const module_decl_t> parse(parse_state_t &ps, bool skip_module_token=false);

		std::string get_canonical_name() const;
		token_t get_name() const;

		token_t name;
	};

	struct link_module_statement_t : public item_impl_t<item_t> {
		typedef ptr<const link_module_statement_t> ref;

		void resolve_linked_module(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref scope) const;

		ptr<const module_decl_t> extern_module;
	};

	struct link_function_statement_t : public item_impl_t<item_t> {
		typedef ptr<const link_function_statement_t> ref;


		bound_var_t::ref resolve_linked_function(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref scope) const;

		token_t function_name;
		ptr<const function_decl_t> extern_function;
	};

	struct module_t : public std::enable_shared_from_this<module_t>, public item_impl_t<item_t> {
		typedef ptr<const module_t> ref;

		module_t(const atom filename);
		static ptr<const module_t> parse(parse_state_t &ps);
		std::string get_canonical_name() const;

		atom filename;
		mutable atom module_key;

		ptr<const module_decl_t> decl;
		std::vector<ptr<const user_defined_type_t>> user_defined_types;
		std::vector<ptr<const function_defn_t>> functions;
		std::vector<ptr<const link_module_statement_t>> linked_modules;
		std::vector<ptr<const link_function_statement_t>> linked_functions;
	};

	struct program_t : public item_impl_t<> {
		typedef ptr<const program_t> ref;

		virtual ~program_t() {}

		std::unordered_set<ptr<const module_t>> modules;
	};

	struct ternary_expr_t : public item_impl_t<expression_t> {
		typedef ptr<const ternary_expr_t> ref;

		virtual bound_var_t::ref resolve_expression(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life) const;

		ptr<const reference_expr_t> condition;
		ptr<const ast::expression_t> when_true, when_false;
	};

	struct ref_expr_interface_t : public expression_t, public can_reference_overloads_t {
		virtual ~ref_expr_interface_t() {}
	};

	struct reference_expr_t : public std::enable_shared_from_this<reference_expr_t>, public item_impl_t<ref_expr_interface_t> {
		typedef ptr<const reference_expr_t> ref;

		static ptr<const reference_expr_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_expression(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life) const;
		virtual bound_var_t::ref resolve_overrides(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref scope,
				life_t::ref,
				const ptr<const ast::item_t> &obj,
				const bound_type_t::refs &args) const;
	};

	struct literal_expr_t : public item_impl_t<expression_t> {
		typedef ptr<const literal_expr_t> ref;

		static ptr<const expression_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_expression(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life) const;
	};

	struct array_literal_expr_t : public item_impl_t<expression_t> {
		typedef ptr<const array_literal_expr_t> ref;

		static ptr<const expression_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_expression(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref scope,
				life_t::ref life) const;

		std::vector<ptr<const expression_t>> items;
	};

	struct array_index_expr_t : public std::enable_shared_from_this<array_index_expr_t>, public item_impl_t<expression_t> {
		typedef ptr<const array_index_expr_t> ref;

		static ptr<const expression_t> parse(parse_state_t &ps);
		virtual bound_var_t::ref resolve_expression(
				status_t &status,
				llvm::IRBuilder<> &builder,
				scope_t::ref block_scope,
				life_t::ref life) const;

		ptr<const reference_expr_t> lhs;
		ptr<const reference_expr_t> index;
	};
}
