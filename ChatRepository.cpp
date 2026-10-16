#include "ChatRepository.h"
#include "Utf16.h"
#include <nlohmann/json.hpp>
#include <iostream>

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

static void print_odbc_error(SQLSMALLINT handle_type, SQLHANDLE handle)
{
    SQLCHAR sqlstate[6];
    SQLCHAR message[SQL_MAX_MESSAGE_LENGTH];
    SQLINTEGER native_error;
    SQLSMALLINT message_len;
    SQLSMALLINT i = 1;

    while (SQLGetDiagRecA(handle_type, handle, i, sqlstate, &native_error,
        message, sizeof(message), &message_len) == SQL_SUCCESS)
    {
        std::cerr << "ODBC Error [" << sqlstate << "] " << message << std::endl;
        i++;
    }
}

std::optional<SavedMessage> ChatRepository::save(int sender_id, int recipient_id,
    const std::string& text, const std::string& attachment_url)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();

    SQLHSTMT stmt = nullptr;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

    const char* query =
        "INSERT INTO Messages (sender_id, recipient_id, body, attachment_url) "
        "OUTPUT INSERTED.id, CONVERT(NVARCHAR(30), INSERTED.created_at, 126) "
        "VALUES (?, ?, ?, ?)";
    SQLPrepareA(stmt, (SQLCHAR*)query, SQL_NTS);

    SQLLEN i1 = 0, i2 = 0;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &sender_id, 0, &i1);
    SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &recipient_id, 0, &i2);

    std::wstring wtext = utf8_to_wide(text);
    SQLLEN body_ind = SQL_NTS;
    SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR,
        (std::max)((SQLULEN)1, (SQLULEN)wtext.size()), 0, (SQLPOINTER)wtext.c_str(), 0, &body_ind);

    // пустой attachment_url -> NULL
    std::wstring wurl = utf8_to_wide(attachment_url);
    SQLLEN url_ind = attachment_url.empty() ? SQL_NULL_DATA : SQL_NTS;
    SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR,
        200, 0, (SQLPOINTER)wurl.c_str(), 0, &url_ind);

    if (!SQL_SUCCEEDED(SQLExecute(stmt)))
    {
        print_odbc_error(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK);
        return std::nullopt;
    }

    SQLBIGINT id = 0;
    SQLCHAR ts[32] = {};
    SQLLEN id_ind = 0, ts_ind = 0;
    SQLBindCol(stmt, 1, SQL_C_SBIGINT, &id, 0, &id_ind);
    SQLBindCol(stmt, 2, SQL_C_CHAR, ts, sizeof(ts), &ts_ind);

    std::optional<SavedMessage> out;
    if (SQL_SUCCEEDED(SQLFetch(stmt)))
        out = SavedMessage{ (long long)id, std::string((char*)ts) };

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    return out;
}

std::string ChatRepository::history(int me, int other, long long before_id, int limit)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();

    SQLHSTMT stmt = nullptr;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

    const char* query =
        "SELECT TOP (?) m.id, u.login, m.body, CONVERT(NVARCHAR(30), m.created_at, 126), m.attachment_url "
        "FROM Messages m JOIN Users u ON u.id = m.sender_id "
        "WHERE ((m.sender_id = ? AND m.recipient_id = ?) OR (m.sender_id = ? AND m.recipient_id = ?)) "
        "AND (? = 0 OR m.id < ?) "
        "ORDER BY m.id DESC";
    SQLPrepareA(stmt, (SQLCHAR*)query, SQL_NTS);

    SQLLEN ind[7] = {};
    auto bind_int = [&](int idx, int& v) {
        SQLBindParameter(stmt, idx, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &v, 0, &ind[idx - 1]);
        };
    bind_int(1, limit);
    bind_int(2, me);  bind_int(3, other);
    bind_int(4, other); bind_int(5, me);
    SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT, 0, 0, &before_id, 0, &ind[5]);
    SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT, 0, 0, &before_id, 0, &ind[6]);

    json items = json::array();

    if (SQL_SUCCEEDED(SQLExecute(stmt)))
    {
        SQLBIGINT id = 0;
        SQLCHAR login[64] = {}, ts[32] = {};
        SQLWCHAR att[201] = {};
        SQLLEN att_ind = 0;
        SQLWCHAR body[2049] = {};
        SQLLEN id_ind = 0, login_ind = 0, body_ind = 0, ts_ind = 0;

        SQLBindCol(stmt, 1, SQL_C_SBIGINT, &id, 0, &id_ind);
        SQLBindCol(stmt, 2, SQL_C_CHAR, login, sizeof(login), &login_ind);
        SQLBindCol(stmt, 3, SQL_C_WCHAR, body, sizeof(body), &body_ind);
        SQLBindCol(stmt, 4, SQL_C_CHAR, ts, sizeof(ts), &ts_ind);
        SQLBindCol(stmt, 5, SQL_C_WCHAR, att, sizeof(att), &att_ind);

        while (SQL_SUCCEEDED(SQLFetch(stmt)))
        {
            json item = {
                {"id", (long long)id},
                {"from", std::string((char*)login)},
                {"text", wide_to_utf8((const wchar_t*)body, (int)(body_ind / sizeof(wchar_t)))},
                {"ts", std::string((char*)ts)}
            };

            if (att_ind != SQL_NULL_DATA && att_ind > 0)
                item["attachment"] = wide_to_utf8((const wchar_t*)att, (int)(att_ind / sizeof(wchar_t)));

            items.push_back(std::move(item));
        }
    }
    else
    {
        print_odbc_error(SQL_HANDLE_STMT, stmt);
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);

    json out = { {"items", items} };
    return out.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string ChatRepository::сreate_group(int owner_id, std::string& group_name, std::vector<std::string>& members, const std::string& kind)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();

    // Единая точка выхода при ошибке: откат всей транзакции + JSON с ошибкой
    auto fail = [&](SQLHSTMT stmt, const char* msg) -> std::string
        {
            if (stmt)
            {
                print_odbc_error(SQL_HANDLE_STMT, stmt);
                SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            }
            SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK);
            json err = { {"error", msg} };
            return err.dump(-1, ' ', false, json::error_handler_t::replace);
        };

    // ---------- 1. Создаём группу ----------
    SQLHSTMT stmt = nullptr;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

    const char* q_group =
        "INSERT INTO ChatGroups (name, owner_id, kind) "
        "OUTPUT INSERTED.id "
        "VALUES (?, ?, ?)";
    SQLPrepareA(stmt, (SQLCHAR*)q_group, SQL_NTS);

    std::wstring wname = utf8_to_wide(group_name);
    std::wstring wkind = utf8_to_wide(kind);
    SQLLEN name_ind = SQL_NTS, owner_ind = 0, kind_ind = SQL_NTS;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR,
        (std::max)((SQLULEN)1, (SQLULEN)wname.size()), 0, (SQLPOINTER)wname.c_str(), 0, &name_ind);
    SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &owner_id, 0, &owner_ind);
    SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR,
        (std::max)((SQLULEN)1, (SQLULEN)wkind.size()), 0, (SQLPOINTER)wkind.c_str(), 0, &kind_ind);

    if (!SQL_SUCCEEDED(SQLExecute(stmt)))
        return fail(stmt, "create group failed");

    SQLINTEGER group_id = 0;
    SQLLEN gid_ind = 0;
    SQLBindCol(stmt, 1, SQL_C_SLONG, &group_id, 0, &gid_ind);
    if (!SQL_SUCCEEDED(SQLFetch(stmt)))
        return fail(stmt, "no group id returned");

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    stmt = nullptr;

    // ---------- 2. Владелец -> GroupMembers с ролью 'owner' ----------
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

    const char* q_owner =
        "INSERT INTO GroupMembers (group_id, user_id, role, joined_at) "
        "VALUES (?, ?, 'owner', SYSUTCDATETIME())";
    SQLPrepareA(stmt, (SQLCHAR*)q_owner, SQL_NTS);

    SQLLEN o1 = 0, o2 = 0;
    SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &group_id, 0, &o1);
    SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &owner_id, 0, &o2);

    if (!SQL_SUCCEEDED(SQLExecute(stmt)))
        return fail(stmt, "add owner failed");

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    stmt = nullptr;

    // ---------- 3. Остальные участники по логину ----------
    json added = json::array();
    json skipped = json::array();

    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

    // Вставляем только если пользователь существует, это не владелец
    // и его ещё нет в группе (защита от дублей в списке)
    const char* q_member =
        "INSERT INTO GroupMembers (group_id, user_id, role, joined_at) "
        "SELECT ?, u.id, 'customer', SYSUTCDATETIME() "
        "FROM Users u "
        "WHERE u.login = ? AND u.id <> ? "
        "AND NOT EXISTS (SELECT 1 FROM GroupMembers gm WHERE gm.group_id = ? AND gm.user_id = u.id)";
    SQLPrepareA(stmt, (SQLCHAR*)q_member, SQL_NTS);

    for (const std::string& login : members)
    {
        std::wstring wlogin = utf8_to_wide(login);

        SQLLEN m_ind[4] = {};
        SQLLEN login_ind = SQL_NTS;
        SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &group_id, 0, &m_ind[0]);
        SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR,
            (std::max)((SQLULEN)1, (SQLULEN)wlogin.size()), 0, (SQLPOINTER)wlogin.c_str(), 0, &login_ind);
        SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &owner_id, 0, &m_ind[2]);
        SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &group_id, 0, &m_ind[3]);

        if (!SQL_SUCCEEDED(SQLExecute(stmt)))
            return fail(stmt, "add member failed");

        SQLLEN rows = 0;
        SQLRowCount(stmt, &rows);
        if (rows > 0) added.push_back(login);
        else          skipped.push_back(login);   // не найден / владелец / дубль

        SQLCloseCursor(stmt);
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);

    json out = {
        {"group_id", (int)group_id},
        {"name", group_name},
        {"kind", kind},
        {"added", added},
        {"skipped", skipped}
    };
    return out.dump(-1, ' ', false, json::error_handler_t::replace);
}



