#include "types.h"
#include "parse_state.h"
#include "identifier.h"

types::type_t::ref parse_type(
		parse_state_t &ps,
	   	identifier::ref supertype_id,
	   	identifier::refs type_variables,
	   	identifier::set generics);
