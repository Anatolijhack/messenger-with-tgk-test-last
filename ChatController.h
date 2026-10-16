#pragma once
#include "ChatRepository.h"
#include "UserRepository.h"
#include "SessionRepository.h"
#include "AuthMiddleware.h"
#include "Router.h"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <random>
#include "MultipartParser.h"
using json = nlohmann::json;
static std::string generate_filename(const std::string& ext)
{
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    std::ostringstream oss;
    oss << std::hex << dist(gen) << ext;
    return oss.str();
}
struct FileType
{
    const char* mime;
    const char* ext;
    size_t      max_size;
    bool        is_image;
};
static const FileType kFileTypes[] = {
    { "image/jpeg",      ".jpg",  10u * 1024 * 1024, true  },
    { "image/png",       ".png",  10u * 1024 * 1024, true  },
    { "image/webp",      ".webp", 10u * 1024 * 1024, true  },
    { "application/pdf", ".pdf",  25u * 1024 * 1024, false },
    { "text/plain",      ".txt",   5u * 1024 * 1024, false },
    { "application/zip", ".zip",  50u * 1024 * 1024, false },
    { "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
                         ".docx", 25u * 1024 * 1024, false },
    { "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
                         ".xlsx", 25u * 1024 * 1024, false },
};

static const FileType* find_type_by_mime(const std::string& mime)
{
    for (const auto& t : kFileTypes)
        if (mime == t.mime) return &t;
    return nullptr;
}

static const FileType* find_type_by_ext(const std::string& ext)
{
    for (const auto& t : kFileTypes)
        if (ext == t.ext) return &t;
    return nullptr;
}

static bool ends_with(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() &&
        s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
static bool is_valid_attachment_url(const std::string& url)
{
    static const std::string prefix = "/uploads/chat/";
    if (url.compare(0, prefix.size(), prefix) != 0)
        return false;

    std::string name = url.substr(prefix.size());
    if (name.size() <= 16) return false;

    for (size_t i = 0; i < 16; ++i)
    {
        char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }

    return find_type_by_ext(name.substr(16)) != nullptr;
}
static bool is_allowed_image_type(const std::string& content_type)
{
    return content_type == "image/jpeg" || content_type == "image/png" || content_type == "image/webp";
}
static std::string normalize_content_type(std::string ct)
{
    auto semi = ct.find(';');
    if (semi != std::string::npos) ct.erase(semi);

    auto b = ct.find_first_not_of(" \t");
    auto e = ct.find_last_not_of(" \t");
    ct = (b == std::string::npos) ? std::string() : ct.substr(b, e - b + 1);

    std::transform(ct.begin(), ct.end(), ct.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return ct;
}

// –асширение только дл€ разрешЄнных типов, иначе nullopt
static std::optional<std::string> extension_for(const std::string& content_type)
{
    if (content_type == "image/jpeg") return ".jpg";
    if (content_type == "image/png")  return ".png";
    if (content_type == "image/webp") return ".webp";
    return std::nullopt;
}

// Content-Type присылает клиент, поэтому провер€ем и реальную сигнатуру файла
static bool matches_signature(const std::string& mime, const char* d, size_t n)
{
    if (mime == "image/jpeg")
        return n >= 3 && (unsigned char)d[0] == 0xFF && (unsigned char)d[1] == 0xD8 && (unsigned char)d[2] == 0xFF;

    if (mime == "image/png")
        return n >= 8 && std::memcmp(d, "\x89PNG\r\n\x1A\n", 8) == 0;

    if (mime == "image/webp")
        return n >= 12 && std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0;

    if (mime == "application/pdf")
        return n >= 5 && std::memcmp(d, "%PDF-", 5) == 0;

    // zip, docx и xlsx Ч это zip-архивы
    if (mime == "application/zip" ||
        mime.find("openxmlformats") != std::string::npos)
        return n >= 4 && std::memcmp(d, "PK\x03\x04", 4) == 0;

    // у txt сигнатуры нет, поэтому провер€ем отсутствие нулевых байтов
    if (mime == "text/plain")
        return std::memchr(d, 0, n) == nullptr;

    return false;
}
class ChatController
{
public:
    ChatController(ConnectionPool& pool, SessionRepository& sessions)
        : chats(pool), users(pool), sessions(sessions) {}

    void register_routes(Router& router)
    {
        router.add("GET", "/chat/history", [this](const Request& req) { return history(req); });
        router.add("POST", "/chat/groups", [this](const Request& req) { return create_group(req); });
        router.add("POST", "/chat/upload", [this](const Request& req) { return upload_attachment(req); });
        router.add("GET", "/uploads/chat/:name", [this](const Request& req) { return get_attachment(req); });
    }

private:
    Response history(const Request& req)
    {
        auto auth = require_auth(req, sessions);
        if (!auth.ok) return auth.error;

        auto param = [&](const char* k) {
            auto it = req.params.find(k);
            return it == req.params.end() ? std::string() : it->second;
            };

        std::string with = param("with");
        if (with.empty())
            return Response{ R"({"error":"with required"})", "application/json", "400 Bad Request" };

        long long before = 0;
        int limit = 50;
        try
        {
            if (!param("before").empty()) before = std::stoll(param("before"));
            if (!param("limit").empty())  limit = std::clamp(std::stoi(param("limit")), 1, 100);
        }
        catch (...)
        {
            return Response{ R"({"error":"Bad before/limit"})", "application/json", "400 Bad Request" };
        }

        auto other = users.find_by_username(with);
        if (!other) return Response{ R"({"items":[]})", "application/json" };   // как Ђнет сообщенийї

        // выдаютс€ только строки, где ты Ч одна из сторон, чужую переписку не получить
        return Response{ chats.history(auth.user_id, other->id, before, limit), "application/json" };
    }

    Response create_group(const Request& req)
    {
        auto auth = require_auth(req, sessions);
        if (!auth.ok) return auth.error;

        auto bad = [](const char* msg) {
            json err = { {"error", msg} };
            return Response{ err.dump(), "application/json", "400 Bad Request" };
            };

        // ---------- разбор тела ----------
        json body = json::parse(req.body, nullptr, /*allow_exceptions*/ false);
        if (body.is_discarded() || !body.is_object())
            return bad("Invalid JSON");

        if (!body.contains("name") || !body["name"].is_string())
            return bad("name required");

        std::string name = body["name"].get<std::string>();

        // обрезаем пробелы по кра€м
        auto b = name.find_first_not_of(" \t\r\n");
        auto e = name.find_last_not_of(" \t\r\n");
        name = (b == std::string::npos) ? std::string() : name.substr(b, e - b + 1);

        if (name.empty())
            return bad("name must not be empty");
        if (name.size() > 100)                       // подгоните под длину колонки ChatGroups.name
            return bad("name too long");

        // ---------- участники ----------
        std::vector<std::string> members;
        if (body.contains("members"))
        {
            if (!body["members"].is_array())
                return bad("members must be an array");

            if (body["members"].size() > 200)
                return bad("too many members");

            for (const auto& m : body["members"])
            {
                if (!m.is_string())
                    return bad("members must be strings");
                std::string login = m.get<std::string>();
                if (!login.empty())
                    members.push_back(std::move(login));
            }
        }
        std::string kind = "group";
        if (body.contains("kind"))
        {
            if (!body["kind"].is_string())
                return bad("kind must be a string");

            kind = body["kind"].get<std::string>();
            if (kind != "group" && kind != "channel")
                return bad("kind must be group or channel");
        }

        // ---------- создание ----------
        // владелец берЄтс€ только из сессии, а не из тела запроса
        std::string result = chats.сreate_group(auth.user_id, name, members, kind);

        // репозиторий при сбое возвращает {"error": "..."}
        json parsed = json::parse(result, nullptr, false);
        if (parsed.is_discarded() || parsed.contains("error"))
            return Response{ result, "application/json", "500 Internal Server Error" };

        return Response{ result, "application/json", "201 Created" };
    }

    
    Response upload_attachment(const Request& req)
    {
        auto auth = require_roles(req, sessions, { "admin", "employee", "customer" });
        if (!auth.ok)
            return auth.error;

        auto error = [](const char* msg, const char* status) {
            json err = { {"error", msg} };
            return Response{ err.dump(), "application/json", status };
            };

        auto content_type_it = req.headers.find("content-type");
        if (content_type_it == req.headers.end() ||
            content_type_it->second.find("multipart/form-data") == std::string::npos)
        {
            return error("Expected multipart/form-data", "400 Bad Request");
        }

        constexpr size_t max_size = 10 * 1024 * 1024;

        // ќтсекаем огромные тела до разбора multipart (запас на заголовки частей)
        if (req.body.size() > max_size + 64 * 1024)
            return error("File too large", "413 Content Too Large");

        auto boundary = extract_boundary(content_type_it->second);
        if (!boundary)
            return error("Missing boundary", "400 Bad Request");

        auto files = parse_multipart(req.body, *boundary);
        if (files.empty())
            return error("No file uploaded", "400 Bad Request");

        const UploadedFile& file = files[0];

        const std::string content_type = normalize_content_type(file.content_type);

        const FileType* type = find_type_by_mime(content_type);
        if (!type)
            return error("Unsupported file type", "400 Bad Request");

        if (file.data.empty())
            return error("Empty file", "400 Bad Request");

        if (file.data.size() > type->max_size)
            return error("File too large", "413 Content Too Large");

        if (!matches_signature(content_type, file.data.data(), file.data.size()))
            return error("File content does not match its type", "400 Bad Request");


        std::error_code ec;
        std::filesystem::create_directories("uploads/chat", ec);
        if (ec)
            return error("Failed to save file", "500 Internal Server Error");

        // »м€ генерируетс€ на сервере, расширение берЄтс€ из белого списка,
        // поэтому им€ из запроса не может привести к path traversal
        std::string saved_name = generate_filename(type->ext);
        std::string saved_path = "uploads/chat/" + saved_name;

        std::ofstream out(saved_path, std::ios::binary);
        if (!out.is_open())
            return error("Failed to save file", "500 Internal Server Error");

        out.write(file.data.data(), static_cast<std::streamsize>(file.data.size()));
        out.close();

        if (!out)
        {
            std::filesystem::remove(saved_path, ec);
            return error("Failed to write file", "500 Internal Server Error");
        }

        json response = {
            {"status", "uploaded"},
            {"file_url", "/uploads/chat/" + saved_name},
            {"content_type", content_type},
            {"file_size", file.data.size()}
        };

        return Response{ response.dump(), "application/json" };
    }

    Response get_attachment(const Request& req)
    {
        auto it = req.params.find("name");
        if (it == req.params.end())
            return Response{ "Not Found", "text/plain", "404 Not Found" };

        const std::string url = "/uploads/chat/" + it->second;
        if (!is_valid_attachment_url(url))
            return Response{ "Not Found", "text/plain", "404 Not Found" };

        std::ifstream in("uploads/chat/" + it->second, std::ios::binary);
        if (!in)
            return Response{ "Not Found", "text/plain", "404 Not Found" };

        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        const std::string& name = it->second;
        const FileType* type = find_type_by_ext(name.substr(16));   // им€ уже проверено is_valid_attachment_url

        const char* ct = (type && type->is_image) ? type->mime : "application/octet-stream";

        return Response{ std::move(data), ct };
    }

   
    ChatRepository chats;
    UserRepository users;
    SessionRepository& sessions;
};