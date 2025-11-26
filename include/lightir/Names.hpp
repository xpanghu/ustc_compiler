#ifndef NAMES_HPP
#define NAMES_HPP
#include <string>
#include <unordered_map>

class Names
{
public:
	explicit Names(bool use_underline, std::string prefix, std::string append)
		: use_underline_(use_underline), default_prefix_used_count_(0), default_prefix_(std::move(prefix)),
		  appended_prefix_(std::move(append))
	{
	}

	// 获得一个 Names 内唯一的名称，会保持唯一的同时尽可能的与 names 类似
	std::string get_name(std::string name);
	// 获得一个 Names 内唯一的名称
	std::string get_name();

private:
	bool use_underline_;
	int default_prefix_used_count_;
	std::string default_prefix_;
	std::string appended_prefix_;
	std::unordered_map<std::string, int> allocated_;
};
#endif // !NAMES_HPP
