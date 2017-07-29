#include "zion.h"
#include "dbg.h"
#include "atom.h"
#include <iostream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "utils.h"

struct atom_t {
	typedef std::unordered_set<atom_t> set;
	typedef std::vector<atom_t> many;

	template <typename T>
	using map = std::map<atom_t, T>;

	atom_t() : iatom(0) {}
	atom_t(std::string &&str);
	atom_t(const std::string &str);
	atom_t(const char *str);

	atom_t &operator =(std::string &&rhs);
	atom_t &operator =(const std::string &rhs);
	atom_t &operator =(const char *rhs);
	atom_t &operator =(const atom_t &);

	bool operator ==(int) const = delete;
	bool operator ==(const atom_t rhs) const { return iatom == rhs.iatom; }
	bool operator !=(const atom_t rhs) const { return iatom != rhs.iatom; }
	bool operator  <(const atom_t rhs) const { return iatom  < rhs.iatom; }
	bool operator ! () const               { return iatom == 0; }
	atom_t operator + (const atom_t rhs) const;

	const char *c_str() const;
	const std::string str() const;
	size_t size() const;

	int iatom;

private:
	std::string value;
};

namespace std {
	template <>
	struct hash<atom_t> {
		int operator ()(atom_t s) const {
			return 1301081 * s.iatom;
		}
	};
}

inline std::ostream &operator <<(std::ostream &os, atom_t value) {
	return os << value.str();
}

inline std::string operator +(const std::string &lhs, const atom_t rhs) {
	return lhs + rhs.str();
}

bool starts_with(atom_t atom_str, const std::string &search);

atom_t::set to_set(atom_t::many atoms);
void dump_atoms();
static std::unordered_map<std::string, int> atom_str_index = {{"", 0}};
static std::vector<std::string> atoms = {""};

void init_atoms() {
	atoms.reserve(5*1024);
}

int memoize_atom(std::string &&str) {
	auto iter = atom_str_index.find(str);
	if (iter != atom_str_index.end()) {
		return iter->second;
	} else {
		int iatom = atoms.size();
		atom_str_index[str] = iatom;
		atoms.push_back(std::move(str));
		return iatom;
	}
}

int memoize_atom(const std::string &str) {
	auto iter = atom_str_index.find(str);
	if (iter != atom_str_index.end()) {
		return iter->second;
	} else {
		int iatom = atoms.size();
		atom_str_index[str] = iatom;
		atoms.push_back(str);
		return iatom;
	}
}

int memoize_atom(const char *str) {
	auto iter = atom_str_index.find(str);
	if (iter != atom_str_index.end()) {
		return iter->second;
	} else {
		int iatom = atoms.size();
		atom_str_index[str] = iatom;
		atoms.push_back(str);
		return iatom;
	}
}

atom_t::atom_t(std::string &&str) : iatom(memoize_atom(str)) {
	value = str;
}

atom_t::atom_t(const std::string &str) : iatom(memoize_atom(str)) {
	value = str;
}

atom_t::atom_t(const char *str) : iatom(memoize_atom(str)) {
	value = str;
}


atom_t &atom_t::operator =(const atom_t &rhs) {
	iatom = rhs.iatom;
	value = rhs.value;
	return *this;
}

atom_t &atom_t::operator =(std::string &&rhs) {
	iatom = memoize_atom(rhs);
	value = str();
	return *this;
}

atom_t &atom_t::operator =(const std::string &rhs) {
	iatom = memoize_atom(rhs);
	value = str();
	return *this;
}

atom_t &atom_t::operator =(const char *rhs) {
	iatom = memoize_atom(rhs);
	value = str();
	return *this;
}

atom_t atom_t::operator + (const atom_t rhs) const {
	return {str() + rhs.str()};
}

const char *atom_t::c_str() const {
	return atoms[iatom].c_str();
}

const std::string atom_t::str() const {
	return atoms[iatom];
}

size_t atom_t::size() const {
	return atoms[iatom].size();
}

atom_t::set to_set(atom_t::many atoms) {
	atom_t::set set;
	std::for_each(
			atoms.begin(),
			atoms.end(),
			[&] (atom_t x) {
				set.insert(x);
			});
	return set;
}

void dump_atoms() {
	int i = 0;
	for (auto a: atoms) {
		std::cerr << i++ << ": " << a << std::endl;
	}
}

int atomize(std::string val) {
	return atom_t{val}.iatom;
}
