#include "ChatService.h"
#include "Session.h"
#include "Logger.h"

using json = nlohmann::json;
static bool is_valid_attachment_url(const std::string& url)
{
    static const std::string prefix = "/uploads/chat/";
    if (url.size() != prefix.size() + 16 + 4 && url.size() != prefix.size() + 16 + 5)
        return false;
    if (url.compare(0, prefix.size(), prefix) != 0)
        return false;

    std::string name = url.substr(prefix.size());
    for (size_t i = 0; i < 16; ++i)
        if (!std::isxdigit((unsigned char)name[i]) || std::isupper((unsigned char)name[i]))
            return false;

    std::string ext = name.substr(16);
    return ext == ".jpg" || ext == ".png" || ext == ".webp";
}
std::string ChatService::dump(const json& j)
{
    // replace: битый UTF-8 не бросит исключение, как раньше с 0xEF
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool ChatService::can_message(const std::string& a, const std::string& b)
{
    auto staff = [](const std::string& r) { return r == "admin" || r == "employee" || r == "seller"; };
    return staff(a) || staff(b);
}

bool ChatService::origin_allowed(const std::string& origin) const
{
    if (allowed_origins.empty()) return true;   // dev-режим
    return std::find(allowed_origins.begin(), allowed_origins.end(), origin) != allowed_origins.end();
}

void ChatService::reply_error(const std::shared_ptr<Session>& s, const char* code, const char* text)
{
    s->ws_send(dump({ {"type", "error"}, {"code", code}, {"message", text} }));
}

void ChatService::kick_user(int user_id)
{
    for (auto& s : hub.sessions_of(user_id)) s->ws_close(1008, "session ended");
}

void ChatService::on_binary(const std::shared_ptr<Session>& s)
{
    reply_error(s, "unsupported", "Binary frames are not supported");
}

void ChatService::on_text(const std::shared_ptr<Session>& s, const std::string& raw)
{
    json msg = json::parse(raw, nullptr, false);   // без исключений

    if (msg.is_discarded() || !msg.is_object() || !msg.contains("type") || !msg["type"].is_string())
        return reply_error(s, "bad_request", "Invalid message");

    const std::string type = msg["type"].get<std::string>();

    if (type == "auth") return handle_auth(s, msg);

    if (!s->chat_identity)
        return reply_error(s, "unauthorized", "Send auth first");

    if (type == "message") return handle_message(s, msg);

    reply_error(s, "unknown_type", "Unknown message type");
}

void ChatService::handle_message(const std::shared_ptr<Session>& s, const json& msg)
{
    if (!s->chat_identity)
        return reply_error(s, "unauthorized", "Authenticate first");

    if (!s->allow_chat_message())
        return reply_error(s, "rate_limited", "Too many messages");

    const bool has_to = msg.contains("to") && msg["to"].is_string();
    const bool has_group = msg.contains("group") && msg["group"].is_string();

    if (!has_to && !has_group)
        return reply_error(s, "bad_request", "to or group required");

    if (msg.contains("to") && msg.contains("group"))
        return reply_error(s, "bad_request", "Specify to or group");

    // ---------- текст (необязателен, если есть вложение) ----------
    std::string text;
    if (msg.contains("text"))
    {
        if (!msg["text"].is_string())
            return reply_error(s, "bad_request", "text must be a string");
        text = msg["text"].get<std::string>();
    }

    const char* ws = " \t\r\n";
    size_t b = text.find_first_not_of(ws);
    if (b == std::string::npos) text.clear();
    else text = text.substr(b, text.find_last_not_of(ws) - b + 1);

    // ---------- вложение ----------
    std::string attachment;
    if (msg.contains("attachment"))
    {
        if (!msg["attachment"].is_string())
            return reply_error(s, "bad_request", "attachment must be a string");

        attachment = msg["attachment"].get<std::string>();
        if (!is_valid_attachment_url(attachment))
            return reply_error(s, "bad_request", "Invalid attachment");
    }

    if (text.empty() && attachment.empty())
        return reply_error(s, "empty", "Message is empty");

    if (text.size() > 2000)
        return reply_error(s, "too_long", "Message is too long (max 2000 bytes)");

    Session::ChatIdentity sender = *s->chat_identity;

    // ======================= ГРУППА / КАНАЛ =======================
    if (has_group)
    {
        std::string group_name = msg["group"].get<std::string>();

        bool queued = workers.try_push_task(
            [this, s, sender, group_name, text, attachment]()
            {
                auto group = group_reposs.find_by_name(group_name);

                if (!group)
                {
                    s->ws_post([this, s]() {
                        reply_error(s, "group_not_found", "Group not found");
                        });
                    return;
                }

                // участник ли отправитель и какая у него роль
                auto role = group_reposs.get_member_role(group->id, sender.user_id);

                // не участник: отвечаем так же, как «группа не найдена»
                if (!role)
                {
                    s->ws_post([this, s]() {
                        reply_error(s, "group_not_found", "Group not found");
                        });
                    return;
                }

                // в канале пишут только владелец и админы
                if (group->kind == "channel" && *role != "owner" && *role != "admin")
                {
                    s->ws_post([this, s]() {
                        reply_error(s, "forbidden", "Only admins can post in a channel");
                        });
                    return;
                }

                auto saved = group_reposs.save_messages(
                    group->id, sender.user_id, text, attachment);

                if (!saved)
                {
                    s->ws_post([this, s]() {
                        reply_error(s, "server_error", "Could not save group message");
                        });
                    return;
                }

                json j = {
                    {"type", "group_message"},
                    {"id", saved->id},
                    {"group_id", group->id},
                    {"group", group->name},
                    {"kind", group->kind},
                    {"from", sender.login},
                    {"text", text},
                    {"ts", saved->created_at}
                };
                if (!attachment.empty()) j["attachment"] = attachment;
                std::string out = dump(j);

                auto members = group_reposs.get_member_ids(group->id);

                for (int user_id : members)
                {
                    for (auto& rs : hub.sessions_of(user_id))
                        rs->ws_send(out);
                }

                json ack = {
                    {"type", "group_sent"},
                    {"id", saved->id},
                    {"group_id", group->id},
                    {"group", group->name},
                    {"kind", group->kind},
                    {"ts", saved->created_at}
                };
                if (!attachment.empty()) ack["attachment"] = attachment;
                s->ws_send(dump(ack));
            }, 0);

        if (!queued)
            reply_error(s, "busy", "Server busy, try again");

        return;
    }

    // ===================== ЛИЧНОЕ СООБЩЕНИЕ =====================
    std::string to = msg["to"].get<std::string>();

    bool queued = workers.try_push_task([this, s, sender, to, text, attachment]()
        {
            auto recipient = users.find_by_username(to);

            // «нет такого» и «нельзя писать» отвечаем одинаково — логины не перебрать
            if (!recipient || recipient->id == sender.user_id || !can_message(sender.role, recipient->role))
            {
                s->ws_post([this, s]() { reply_error(s, "recipient_not_found", "Recipient not found"); });
                return;
            }

            auto saved = chats.save(sender.user_id, recipient->id, text, attachment);

            if (!saved)
            {
                s->ws_post([this, s]() { reply_error(s, "server_error", "Could not save message"); });
                return;
            }

            json j = {
                {"type", "message"}, {"id", saved->id}, {"from", sender.login},
                {"text", text}, {"ts", saved->created_at}
            };
            if (!attachment.empty()) j["attachment"] = attachment;
            std::string out = dump(j);

            for (auto& rs : hub.sessions_of(recipient->id)) rs->ws_send(out);

            json ack = {
                {"type", "sent"}, {"id", saved->id},
                {"to", recipient->username}, {"ts", saved->created_at}
            };
            if (!attachment.empty()) ack["attachment"] = attachment;
            s->ws_send(dump(ack));
        }, 0);

    if (!queued) reply_error(s, "busy", "Server busy, try again");
}

void ChatService::handle_auth(const std::shared_ptr<Session>& s, const json& msg)
{
    if (s->chat_identity) return reply_error(s, "already_authenticated", "Already authenticated");

    if (!msg.contains("token") || !msg["token"].is_string())
        return reply_error(s, "bad_request", "token required");

    std::string token = msg["token"].get<std::string>();

    bool queued = workers.try_push_task([this, s, token]()
        {
            auto session = sessions.find(token);          // ÁÄ — â ïóëå, íå â io-ïîòîêå

            s->ws_post([this, s, session]()               // îáðàòíî â ïîòîê ñîêåòà
                {
                    if (s->chat_identity) return;

                    if (!session)
                    {
                        reply_error(s, "unauthorized", "Invalid or expired token");
                        s->ws_close(1008, "unauthorized");
                        return;
                    }

                    if (!hub.attach(session->user_id, s))
                    {
                        reply_error(s, "too_many_connections", "Too many open connections");
                        s->ws_close(1008, "too many connections");
                        return;
                    }

                    s->chat_identity = Session::ChatIdentity{ session->user_id, session->username, session->role };
                    s->ws_send(dump({ {"type", "auth_ok"}, {"login", session->username} }));
                });
        }, 0);

    if (!queued) reply_error(s, "busy", "Server busy, try again");
}
