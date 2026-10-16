#pragma once
#include "СhatHub.h"
#include "ChatRepository.h"
#include "UserRepository.h"
#include "SessionRepository.h"
#include "ThreadPool.h"
#include <memory>
#include <nlohmann/json.hpp>
#include "GroupRepository.h"


class Session;

class ChatService
{
public:
    ChatService(ConnectionPool& pool, SessionRepository& sessions, ThreadPool& workers,
        std::vector<std::string> allowed_origins = {})
        : chats(pool), users(pool),group_reposs(pool), sessions(sessions), workers(workers),
        allowed_origins(std::move(allowed_origins)) {}

    void on_text(const std::shared_ptr<Session>& s, const std::string& raw);   // io-поток
    void on_binary(const std::shared_ptr<Session>& s);
    void detach(int user_id) { hub.detach(user_id); }
    void kick_user(int user_id);                                               // закрыть все соединения
    bool origin_allowed(const std::string& origin) const;

private:
    using json = nlohmann::json;

    void handle_auth(const std::shared_ptr<Session>& s, const json& msg);
    void handle_message(const std::shared_ptr<Session>& s, const json& msg);
    void reply_error(const std::shared_ptr<Session>& s, const char* code, const char* text);
    static bool can_message(const std::string& a, const std::string& b);
    static std::string dump(const json& j);

    ChatHub hub;
    ChatRepository chats;
    UserRepository users;
    GroupRepository group_reposs;
    SessionRepository& sessions;
    ThreadPool& workers;

    std::vector<std::string> allowed_origins;
};