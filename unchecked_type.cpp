#include "unchecked_type.h"
#include "scopes.h"
#include "ast.h"

std::string unchecked_type_t::str() const {
	return node->str();
}

unchecked_type_t::unchecked_type_t(
		identifier::ref id,
		ptr<const ast::item_t> node,
		ptr<scope_t> const module_scope) :
	id(id), node(node), module_scope(module_scope)
{
	debug_above(5, log(log_info, "creating unchecked type " c_type("%s"),
			   	id->get_name().c_str()));

	assert(this->node != nullptr);
}

unchecked_type_t::ref unchecked_type_t::create(
		identifier::ref id,
		ptr<const ast::item_t> node,
		ptr<scope_t> const module_scope)
{
	return ref(new unchecked_type_t(id, node, module_scope));
}
