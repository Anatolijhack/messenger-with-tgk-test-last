#pragma once
#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

class Session;

class ChatHub
{
public:
    static constexpr size_t MAX_CONNECTIONS_PER_USER = 5;

    bool attach(int user_id, const std::shared_ptr<Session>& s)
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto& list = by_user[user_id];
        prune(list);
        if (list.size() >= MAX_CONNECTIONS_PER_USER) return false;
        list.push_back(s);
        return true;
    }

    void detach(int user_id)   // убирает умершие соединения
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = by_user.find(user_id);
        if (it == by_user.end()) return;
        prune(it->second);
        if (it->second.empty()) by_user.erase(it);
    }

    std::vector<std::shared_ptr<Session>> sessions_of(int user_id)
    {
        std::vector<std::shared_ptr<Session>> out;
        std::lock_guard<std::mutex> lock(mtx);
        auto it = by_user.find(user_id);
        if (it == by_user.end()) return out;
        prune(it->second);
        for (auto& w : it->second)
            if (auto s = w.lock()) out.push_back(std::move(s));
        return out;
    }

private:
    static void prune(std::vector<std::weak_ptr<Session>>& v)
    {
        v.erase(std::remove_if(v.begin(), v.end(),
            [](const std::weak_ptr<Session>& w) { return w.expired(); }), v.end());
    }

    std::mutex mtx;
    std::unordered_map<int, std::vector<std::weak_ptr<Session>>> by_user;
};