#pragma once
#include <string>
#include <optional>
#include "ConnectionPool.h"
struct CreatedGroup
{
	int id = 0;
	std::string name;
	std::string kind = "group";
};
struct GroupMessageRecord
{
	long long id;
	std::string created_at;
};

class GroupRepository
{
public:
	GroupRepository(ConnectionPool& pool) : pool(pool) {}
	std::optional<CreatedGroup> find_by_name(const std::string& group_name);
	std::optional<GroupMessageRecord> save_messages(
		int group_id,
		int sender_id,
		const std::string& body,
		const std::string& attachment_url);
	std::vector<int> get_member_ids(int group_id);
	std::optional<std::string> get_member_role(int group_id, int user_id);
	
private:
	ConnectionPool& pool;
	
};