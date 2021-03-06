#pragma once
#include "zion.h"
#include "dbg.h"
#include "user_error.h"
#include "utils.h"
#include <string>
#include <map>
#include "ast_decls.h"
#include "bound_var.h"

struct unchecked_type_t {
	unchecked_type_t() = delete;
	unchecked_type_t(const unchecked_type_t &) = delete;
	unchecked_type_t(
			std::string name,
			ptr<const ast::item_t> node,
			ptr<scope_t> const module_scope);

	std::string const name;
	ptr<const ast::item_t> const node;
	ptr<scope_t> const module_scope;

	std::string str() const;

	typedef ptr<const unchecked_type_t> ref;
	typedef std::vector<ref> refs;
	typedef std::map<std::string, ref> map;

	static ref create(
			std::string name,
			ptr<const ast::item_t> node,
			ptr<scope_t> const module_scope);
};
