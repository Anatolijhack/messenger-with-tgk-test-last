#include "GroupRepository.h"
#include <codecvt>
#include "Utf16.h"
static void print_odbc_error(SQLSMALLINT handle_type, SQLHANDLE handle)
{
    SQLCHAR sqlstate[6];
    SQLCHAR message[SQL_MAX_MESSAGE_LENGTH];
    SQLINTEGER native_error;
    SQLSMALLINT message_len;
    SQLSMALLINT i = 1;

    while (SQLGetDiagRecA(
        handle_type,
        handle,
        i,
        sqlstate,
        &native_error,
        message,
        sizeof(message),
        &message_len
    ) == SQL_SUCCESS)
    {
        std::cerr << "ODBC Error [" << sqlstate << "] " << message << std::endl;
        i++;
    }
}
std::optional<CreatedGroup> GroupRepository::find_by_name(const std::string& name)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();

    SQLHSTMT stmt = nullptr;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

    const char* query =
        "SELECT id, name, kind "
        "FROM ChatGroups "
        "WHERE name = ?";

    SQLPrepareA(stmt, (SQLCHAR*)query, SQL_NTS);

    std::wstring wname = utf8_to_wide(name);
    SQLLEN name_len = SQL_NTS;
    SQLBindParameter(
        stmt, 1, SQL_PARAM_INPUT,
        SQL_C_WCHAR, SQL_WVARCHAR,
        (std::max)((SQLULEN)1, (SQLULEN)wname.size()), 0,
        (SQLPOINTER)wname.c_str(), 0, &name_len
    );

    if (!SQL_SUCCEEDED(SQLExecute(stmt)))
    {
        print_odbc_error(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK);
        return std::nullopt;
    }

    SQLINTEGER out_id = 0;
    SQLWCHAR out_name[256] = {};
    SQLCHAR out_kind[16] = {};

    SQLLEN id_ind = 0, name_ind = 0, kind_ind = 0;

    SQLBindCol(stmt, 1, SQL_C_LONG, &out_id, 0, &id_ind);
    SQLBindCol(stmt, 2, SQL_C_WCHAR, out_name, sizeof(out_name), &name_ind);
    SQLBindCol(stmt, 3, SQL_C_CHAR, out_kind, sizeof(out_kind), &kind_ind);

    std::optional<CreatedGroup> result;

    if (SQL_SUCCEEDED(SQLFetch(stmt)))
    {
        CreatedGroup g;
        g.id = static_cast<int>(out_id);
        g.name = wide_to_utf8((const wchar_t*)out_name, (int)(name_ind / sizeof(wchar_t)));
        g.kind = std::string(reinterpret_cast<char*>(out_kind));
        result = std::move(g);
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
    return result;
}


std::optional<GroupMessageRecord> GroupRepository::save_messages(
    int group_id,
    int sender_id,
    const std::string& body,
    const std::string& attachment_url)
{
    auto leased = pool.acquire();
    SQLHDBC dbc = leased.handle();

    SQLHSTMT stmt = nullptr;
    SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

    const char* query =
        "INSERT INTO GroupMessages (group_id, sender_id, body, attachment_url) "
        "OUTPUT INSERTED.id, "
        "CONVERT(VARCHAR(33), INSERTED.created_at, 126) "
        "VALUES (?, ?, ?, ?)";

    SQLPrepareA(stmt, (SQLCHAR*)query, SQL_NTS);

    SQLINTEGER gid = group_id;
    SQLINTEGER uid = sender_id;

    SQLLEN gid_ind = 0;
    SQLLEN uid_ind = 0;

    SQLBindParameter(
        stmt, 1, SQL_PARAM_INPUT,
        SQL_C_LONG, SQL_INTEGER,
        0, 0, &gid, 0, &gid_ind
    );

    SQLBindParameter(
        stmt, 2, SQL_PARAM_INPUT,
        SQL_C_LONG, SQL_INTEGER,
        0, 0, &uid, 0, &uid_ind
    );

    // текст: UTF-8 -> UTF-16, как в ChatRepository::save
    std::wstring wbody = utf8_to_wide(body);
    SQLLEN body_ind = SQL_NTS;
    SQLBindParameter(
        stmt, 3, SQL_PARAM_INPUT,
        SQL_C_WCHAR, SQL_WVARCHAR,
        (std::max)((SQLULEN)1, (SQLULEN)wbody.size()), 0,
        (SQLPOINTER)wbody.c_str(), 0, &body_ind
    );

    // вложение: пустая строка -> NULL
    std::wstring wurl = utf8_to_wide(attachment_url);
    SQLLEN url_ind = attachment_url.empty() ? SQL_NULL_DATA : SQL_NTS;
    SQLBindParameter(
        stmt, 4, SQL_PARAM_INPUT,
        SQL_C_WCHAR, SQL_WVARCHAR,
        200, 0,
        (SQLPOINTER)wurl.c_str(), 0, &url_ind
    );

    if (!SQL_SUCCEEDED(SQLExecute(stmt)))
    {
        print_odbc_error(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK);
        return std::nullopt;
    }

    SQLBIGINT out_id = 0;
    SQLCHAR out_created_at[40] = {};

    SQLLEN id_ind = 0;
    SQLLEN created_ind = 0;

    SQLBindCol(stmt, 1, SQL_C_SBIGINT, &out_id, 0, &id_ind);
    SQLBindCol(stmt, 2, SQL_C_CHAR, out_created_at, sizeof(out_created_at), &created_ind);

    std::optional<GroupMessageRecord> result;

    if (SQL_SUCCEEDED(SQLFetch(stmt)))
    {
        result = GroupMessageRecord{
            static_cast<long long>(out_id),
            std::string(reinterpret_cast<char*>(out_created_at))
        };
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    SQLEndTran(SQL_HANDLE_DBC, dbc, result ? SQL_COMMIT : SQL_ROLLBACK);
    return result;
}

    std::vector<int> GroupRepository::get_member_ids(int group_id)
    {
        auto leased = pool.acquire();
        SQLHDBC dbc = leased.handle();

        SQLHSTMT stmt = nullptr;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

        const char* query =
            "SELECT user_id "
            "FROM GroupMembers "
            "WHERE group_id = ?";

        SQLPrepareA(stmt, (SQLCHAR*)query, SQL_NTS);

        SQLINTEGER gid = group_id;
        SQLLEN gid_ind = 0;

        SQLBindParameter(
            stmt, 1, SQL_PARAM_INPUT,
            SQL_C_LONG, SQL_INTEGER,
            0, 0, &gid, 0, &gid_ind
        );

        SQLRETURN rc = SQLExecute(stmt);

        if (!SQL_SUCCEEDED(rc))
        {
            print_odbc_error(SQL_HANDLE_STMT, stmt);
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            return {};
        }

        SQLINTEGER user_id = 0;
        SQLLEN user_id_ind = 0;

        SQLBindCol(stmt, 1, SQL_C_LONG,
            &user_id, 0, &user_id_ind);

        std::vector<int> members;

        while (SQL_SUCCEEDED(SQLFetch(stmt)))
        {
            members.push_back(static_cast<int>(user_id));
        }

       
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
        return members;
    }

    std::optional<std::string> GroupRepository::get_member_role(int group_id, int user_id)
    {
        auto leased = pool.acquire();
        SQLHDBC dbc = leased.handle();

        SQLHSTMT stmt = nullptr;
        SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

        const char* query =
            "SELECT role "
            "FROM GroupMembers "
            "WHERE group_id = ? AND user_id = ?";

        SQLPrepareA(stmt, (SQLCHAR*)query, SQL_NTS);

        SQLINTEGER gid = group_id;
        SQLINTEGER uid = user_id;
        SQLLEN gid_ind = 0;
        SQLLEN uid_ind = 0;

        SQLBindParameter(
            stmt, 1, SQL_PARAM_INPUT,
            SQL_C_LONG, SQL_INTEGER,
            0, 0, &gid, 0, &gid_ind
        );

        SQLBindParameter(
            stmt, 2, SQL_PARAM_INPUT,
            SQL_C_LONG, SQL_INTEGER,
            0, 0, &uid, 0, &uid_ind
        );

        if (!SQL_SUCCEEDED(SQLExecute(stmt)))
        {
            print_odbc_error(SQL_HANDLE_STMT, stmt);
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK);
            return std::nullopt;
        }

        SQLCHAR role[32] = {};
        SQLLEN role_ind = 0;

        SQLBindCol(stmt, 1, SQL_C_CHAR, role, sizeof(role), &role_ind);

        std::optional<std::string> result;

        if (SQL_SUCCEEDED(SQLFetch(stmt)))
            result = std::string(reinterpret_cast<char*>(role));

        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT);
        return result;
    }
   