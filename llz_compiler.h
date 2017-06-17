#pragma once
struct llzc_t {
	std::string program_name;
	typedef std::vector<std::string> libs;

	ptr<std::vector<std::string>> zion_paths;
	llzc_t() = delete;
	llzc_t(const llzc_t &) = delete;
	llzc_t(atom filename, const libs &zion_paths);
	~llzc_t();
};
