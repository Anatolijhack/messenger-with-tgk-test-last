#include "Session.h"
#include <sstream>
#include <string>
#include <iostream>
#include "Logger.h"
#include "Structs.h"
#include "Router.h"
#include "ChatService.h"
#include <cstdint>
#include <limits>

constexpr std::uint64_t MAX_WEBSOCKET_MESSAGE_SIZE =
1024 * 1024;

void Session::enqueue_ws_frame(std::shared_ptr<std::string> frame)
{
    if (websocket_close_sent) {
        return;
    }

    if (write_queue.size() >= MAX_WRITE_QUEUE) {
        LOG_WARNING("WebSocket: клиент не читает, закрываю");
        do_shutdown();
        return;
    }

    write_queue.push_back(std::move(frame));

    if (!writing) {
        do_write();
    }
}

bool Session::is_valid_utf8(
    const std::string& data)
{
    std::size_t i = 0;

    while (i < data.size())
    {
        const unsigned char c =
            static_cast<unsigned char>(
                data[i]
                );

        // -----------------------------
        // 1-byte character
        // U+0000 ... U+007F
        // -----------------------------

        if (c <= 0x7F)
        {
            ++i;
            continue;
        }

        // -----------------------------
        // 2-byte character
        // U+0080 ... U+07FF
        // -----------------------------

        if (c >= 0xC2 &&
            c <= 0xDF)
        {
            if (i + 1 >= data.size())
            {
                return false;
            }

            const unsigned char c1 =
                static_cast<unsigned char>(
                    data[i + 1]
                    );

            if (c1 < 0x80 ||
                c1 > 0xBF)
            {
                return false;
            }

            i += 2;
            continue;
        }

        // -----------------------------
        // 3-byte character
        // -----------------------------

        if (c >= 0xE0 &&
            c <= 0xEF)
        {
            if (i + 2 >= data.size())
            {
                return false;
            }

            const unsigned char c1 =
                static_cast<unsigned char>(
                    data[i + 1]
                    );

            const unsigned char c2 =
                static_cast<unsigned char>(
                    data[i + 2]
                    );

            if (c1 < 0x80 ||
                c1 > 0xBF ||
                c2 < 0x80 ||
                c2 > 0xBF)
            {
                return false;
            }

            // Запрещаем overlong encoding
            // и UTF-16 surrogate range.
            if (c == 0xE0 &&
                c1 < 0xA0)
            {
                return false;
            }

            if (c == 0xED &&
                c1 > 0x9F)
            {
                return false;
            }

            i += 3;
            continue;
        }

        // -----------------------------
        // 4-byte character
        // U+10000 ... U+10FFFF
        // -----------------------------

        if (c >= 0xF0 &&
            c <= 0xF4)
        {
            if (i + 3 >= data.size())
            {
                return false;
            }

            const unsigned char c1 =
                static_cast<unsigned char>(
                    data[i + 1]
                    );

            const unsigned char c2 =
                static_cast<unsigned char>(
                    data[i + 2]
                    );

            const unsigned char c3 =
                static_cast<unsigned char>(
                    data[i + 3]
                    );

            if (c1 < 0x80 ||
                c1 > 0xBF ||
                c2 < 0x80 ||
                c2 > 0xBF ||
                c3 < 0x80 ||
                c3 > 0xBF)
            {
                return false;
            }

            // U+10000 minimum
            if (c == 0xF0 &&
                c1 < 0x90)
            {
                return false;
            }

            // U+10FFFF maximum
            if (c == 0xF4 &&
                c1 > 0x8F)
            {
                return false;
            }

            i += 4;
            continue;
        }

        // -----------------------------
        // Invalid UTF-8 leading byte
        // -----------------------------

        return false;
    }

    return true;
}
bool Session::is_valid_websocket_close_code(
    std::uint16_t code)
{
    // Normal closure
    if (code == 1000)
    {
        return true;
    }

    // Protocol / application-level close codes
    if (code == 1001 ||
        code == 1002 ||
        code == 1003)
    {
        return true;
    }

    // 1004 is reserved
    if (code == 1004)
    {
        return false;
    }

    // 1005: No Status Received
    // Must NOT be sent in a CLOSE frame.
    if (code == 1005)
    {
        return false;
    }

    // 1006: Abnormal Closure
    // Must NOT be sent in a CLOSE frame.
    if (code == 1006)
    {
        return false;
    }

    // 1007 - 1014 are defined WebSocket close codes
    if (code >= 1007 &&
        code <= 1014)
    {
        return true;
    }

    // 1015: TLS failure
    // Must NOT be sent in a CLOSE frame.
    if (code == 1015)
    {
        return false;
    }

    // Application-defined codes
    if (code >= 3000 &&
        code <= 4999)
    {
        return true;
    }

    return false;
}

static std::string websocket_accept(const std::string& key)
{
    static constexpr char GUID[] =
        "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    const std::string input = key + GUID;

    unsigned char hash[SHA_DIGEST_LENGTH];

    SHA1(
        reinterpret_cast<const unsigned char*>(input.data()),
        input.size(),
        hash
    );

    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());

    b64 = BIO_push(b64, mem);

    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);

    BIO_write(
        b64,
        hash,
        SHA_DIGEST_LENGTH
    );

    BIO_flush(b64);

    BUF_MEM* buffer = nullptr;

    BIO_get_mem_ptr(
        b64,
        &buffer
    );

    std::string result(
        buffer->data,
        buffer->length
    );

    BIO_free_all(b64);

    return result;
}

static std::string to_lower(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c)
        {
            return static_cast<char>(std::tolower(c));
        });

    return value;
}

void Session::send_websocket_close()
{
    auto self = shared_from_this();

    boost::asio::post(
        socket.get_executor(),
        [this, self]()
        {
            if (websocket_close_sent)
            {
                return;
            }

            websocket_close_sent = true;

            auto frame =
                std::make_shared<std::string>();

            // FIN=1, opcode=CLOSE
            frame->push_back(
                static_cast<char>(0x88)
            );

            // Empty payload
            frame->push_back(
                static_cast<char>(0x00)
            );

            write_queue.push_back(frame);

            if (!writing)
            {
                do_write();
            }
        }
    );
}


void Session::send_websocket_close(
    std::uint16_t code,
    const std::string& reason)
{
    auto self = shared_from_this();

    boost::asio::post(
        socket.get_executor(),
        [this, self, code, reason]()
        {
            if (websocket_close_sent)
            {
                return;
            }

            websocket_close_sent = true;

            auto frame =
                std::make_shared<std::string>();

            // FIN=1, opcode=CLOSE
            frame->push_back(
                static_cast<char>(0x88)
            );

            // Close payload: 2 bytes for status code
            // followed by the UTF-8 reason.
            const std::size_t reason_size =
                std::min<std::size_t>(
                    reason.size(), 123
                );

            const std::uint16_t close_code = code;

            frame->push_back(
                static_cast<char>(2 + reason_size)
            );

            frame->push_back(
                static_cast<char>(
                    (close_code >> 8) & 0xFF
                    )
            );

            frame->push_back(
                static_cast<char>(
                    close_code & 0xFF
                    )
            );

            frame->append(
                reason.data(), reason_size
            );

            write_queue.push_back(frame);

            if (!writing)
            {
                writing = true;
                do_write();
            }
        }
    );
}

void Session::websocket_protocol_error(
    std::uint16_t code,
    const std::string& reason)
{
    LOG_ERROR("WebSocket protocol error: " + reason);

    send_websocket_close(code, reason);
}

//void Session::do_read()
//{
//    auto self = shared_from_this();
//
//    socket.async_read_some(
//        boost::asio::buffer(temp),
//        [this, self](boost::system::error_code ec, std::size_t length)
//        {
//            if (ec)
//                return;
//
//            buffer.append(temp.data(), length);
//
//            if (buffer.size() > MAX_REQUEST_SIZE)
//            {
//                send_response_safe("Request too large", "text/plain", "413 Payload Too Large");
//                return;
//            }
//
//            if (parse())
//            {
//                process_request();
//                reset_parser();
//            }
//           
//        });
//}
void Session::do_read()
{
    auto self = shared_from_this();

    // Сначала пытаемся разобрать то,
    // что уже накопилось в buffer
    if (!buffer.empty())
    {
        if (buffer.size() > MAX_REQUEST_SIZE)
        {
            send_response_safe(
                "Request too large",
                "text/plain",
                "413 Payload Too Large"
            );
            return;
        }

        ParseResult result = parse();

        if (result == ParseResult::Complete)
        {
            if (is_websocket_request())
            {
                do_websocket_handshake();
                return;
            }
            process_request();
            reset_parser();
            return;
        }

        if (result == ParseResult::Error)
        {
            return;
        }
    }

    // Если полного запроса пока нет —
    // читаем новые данные из сокета
    socket.async_read_some(
        boost::asio::buffer(temp),
        [this, self](boost::system::error_code ec, std::size_t length)
        {
            if (ec)
            {
                if (ec != boost::asio::error::eof) // EOF — это нормальное закрытие клиентом, не ошибка
                {
                    std::cerr << "Read error: " << ec.message() << std::endl;
                }
                return;
            }

            buffer.append(temp.data(), length);

            if (buffer.size() > MAX_REQUEST_SIZE)
            {
                send_response_safe(
                    "Request too large",
                    "text/plain",
                    "413 Payload Too Large"
                );
                return;
            }

            //if (parse())
            //{
            //    process_request();
            //    reset_parser();
            //}
            //else
            //{
            //    // Запрос ещё не полный.
            //    // Продолжаем читать.
            //    do_read();
            //}
            ParseResult result = parse();

            if (result == ParseResult::Complete)
            {
                if (is_websocket_request())
                {
                    do_websocket_handshake();
                    return;
                }
                process_request();
                reset_parser();
                return;
            }

            if (result == ParseResult::Error)
            {
                return;
            }

            // Incomplete
            do_read();
        });
}
void Session::reset_parser()
{
    state = ParseState::RequestLine;
    method.clear();
    path.clear();
    version.clear();
    headers.clear();
    content_length = 0;
    body.clear();
}


Session::ParseResult Session::parse()
{
    while (true)
    {
        if (state == ParseState::RequestLine)
        {
            auto pos = buffer.find("\r\n");

            if (pos == std::string::npos)
                return ParseResult::Incomplete;

            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 2);

            std::istringstream rl(line);

            if (!(rl >> method >> path >> version))
            {
                send_response_safe(
                    "Bad Request",
                    "text/plain",
                    "400 Bad Request"
                );

                return ParseResult::Error;
            }

            state = ParseState::Header;
        }

        else if (state == ParseState::Header)
        {
            auto pos = buffer.find("\r\n");

            if (pos == std::string::npos)
                return ParseResult::Incomplete;

            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 2);

            if (line.empty())
            {
                keep_alive = true;

                auto conn = headers.find("connection");

                if (conn != headers.end())
                {
                    std::string value = to_lower(conn->second);

                    if (value == "close")
                    {
                        keep_alive = false;
                    }
                }

                auto it = headers.find("content-length");

                if (it != headers.end())
                {
                    try
                    {
                        std::size_t pos = 0;

                        long long value =
                            std::stoll(it->second, &pos);

                        if (pos != it->second.size() || value < 0)
                        {
                            send_response_safe(
                                "Bad Request",
                                "text/plain",
                                "400 Bad Request"
                            );

                            return ParseResult::Error;
                        }

                        if (value > static_cast<long long>(MAX_REQUEST_SIZE))
                        {
                            send_response_safe(
                                "Too large",
                                "text/plain",
                                "413 Payload Too Large"
                            );

                            return ParseResult::Error;
                        }

                        content_length =
                            static_cast<int>(value);
                    }
                    catch (...)
                    {
                        send_response_safe(
                            "Bad Request",
                            "text/plain",
                            "400 Bad Request"
                        );

                        return ParseResult::Error;
                    }
                }

                if (content_length > 0)
                {
                    state = ParseState::Body;
                }
                else
                {
                    return ParseResult::Complete;
                }
            }
            else
            {
                auto sep = line.find(":");

                if (sep == std::string::npos)
                {
                    send_response_safe(
                        "Bad Request",
                        "text/plain",
                        "400 Bad Request"
                    );

                    return ParseResult::Error;
                }

                std::string key = line.substr(0, sep);
                key = to_lower(key);

                std::string value = line.substr(sep + 1);

                while (!value.empty() && value.front() == ' ')
                    value.erase(value.begin());

                headers[key] = value;
            }
        }

        else if (state == ParseState::Body)
        {
            if (buffer.size() < content_length)
                return ParseResult::Incomplete;

            body = buffer.substr(0, content_length);
            buffer.erase(0, content_length);

            return ParseResult::Complete;
        }
    }
}


void Session::process_request()
{
    auto self = shared_from_this();

    std::cout << "=== PROCESS REQUEST ===" << std::endl;
    std::cout << "METHOD: [" << method << "]\n";
    std::cout << "PATH: [" << path << "]\n";
    std::cout << "BODY: [" << body << "]\n";
    std::cout << "BODY SIZE: " << body.size() << "\n";


    Request req{ method, path, body, {}, headers };

    pool.submit(0, [this, self, req]() mutable
        {
            std::cout << "INSIDE WORKER" << std::endl;

            Response res = router.route(req);

            std::cout << "ROUTER DONE" << std::endl;
            std::cout << "STATUS: " << res.status << std::endl;
            std::cout << "BODY: " << res.body << std::endl;

            boost::asio::post(socket.get_executor(),
                [this, self, res]()
                {
                    std::cout << "BEFORE RESPONSE" << std::endl;

                    send_response_safe(
                        res.body,
                        res.content_type,
                        res.status
                    );
                });
        });
}

void Session::send_response(const std::string& body,
    const std::string& type,
    const std::string& status)
{
    /*auto self = shared_from_this();

    auto response = std::make_shared<std::string>(
        "HTTP/1.1 " + status + "\r\n" +
        "Content-Length: " + std::to_string(body.size()) + "\r\n" +
        "Content-Type: " + type + "\r\n" +
        "Connection: " + std::string(keep_alive ? "keep-alive" : "close") + "\r\n" +
        "\r\n" +
        body
    );

    write_queue.push_back(response);

    if (!writing)
    {
        do_write();
    }*/
    auto self = shared_from_this();

    auto response = std::make_shared<std::string>(
        "HTTP/1.1 " + status + "\r\n" +
        "Content-Length: " + std::to_string(body.size()) + "\r\n" +
        "Content-Type: " + type + "\r\n" +
        "Connection: " + std::string(keep_alive ? "keep-alive" : "close") + "\r\n" +
        "Access-Control-Allow-Origin: *\r\n" +
        "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n" +
        "Access-Control-Allow-Headers: Content-Type, Authorization\r\n" +
        "\r\n" +
        body
    );

    write_queue.push_back(response);

    if (!writing)
    {
        do_write();
    }
}
void Session::do_write()
{
    if (write_queue.empty())
    {
        writing = false;

        if (!keep_alive)
        {
            do_shutdown();
            return;
        }

        if (websocket_mode)
        {

            if (websocket_close_received)
            {
                do_shutdown();
            }

            do_websocket_read();
            return;
        }

        do_read();
        return;
    }

    writing = true;

    auto self = shared_from_this();
    auto response = write_queue.front();

    boost::asio::async_write(socket,
        boost::asio::buffer(*response),
        [this, self, response](boost::system::error_code ec, std::size_t)
        {
            if (ec)
            {
                LOG_ERROR("Write error: " + ec.message());

                writing = false;
                write_queue.clear(); // очищаем очередь — нет смысла пытаться писать дальше

                do_shutdown();
                return;
            }

            write_queue.pop_front();
            do_write();
        });
}
void Session::send_response_safe(const std::string& body,
    const std::string& type,
    const std::string& status)
{
    auto self = shared_from_this();

    boost::asio::post(socket.get_executor(),
        [this, self, body, type, status]()
        {
            send_response(body, type, status);
        });
}
bool Session::is_websocket_request() const
{
    if (method != "GET" || path != "/ws")
        return false;

    auto get = [&](const char* key) -> std::string {
        auto it = headers.find(key);
        return it == headers.end() ? "" : it->second;
        };

    auto contains_token = [](const std::string& value,
        const std::string& expected) {
            std::istringstream stream(value);
            std::string token;

            while (std::getline(stream, token, ',')) {
                token = to_lower(token);

                auto first = token.find_first_not_of(" \t");
                if (first == std::string::npos)
                    continue;

                auto last = token.find_last_not_of(" \t");
                token = token.substr(first, last - first + 1);

                if (token == expected)
                    return true;
            }

            return false;
        };

    if (to_lower(get("upgrade")) != "websocket")
        return false;

    if (get("sec-websocket-key").empty())
        return false;

    if (get("sec-websocket-version") != "13")
        return false;

    if (!contains_token(get("connection"), "upgrade"))
        return false;

    return true;
}

void Session::do_websocket_handshake()
{
    auto key_it =
        headers.find("sec-websocket-key");

    if (key_it == headers.end())
    {
        do_shutdown();
        return;
    }
    auto origin = headers.find("origin");
    if (chat && origin != headers.end() && !chat->origin_allowed(origin->second))
    {
        send_response_safe("Forbidden", "text/plain", "403 Forbidden");
        return;
    }
    const std::string accept =
        websocket_accept(key_it->second);

    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n"
        "\r\n";

    websocket_mode = true;

    write_queue.push_back(
        std::make_shared<std::string>(
            std::move(response)
        )
    );

    if (!writing)
    {
        writing = true;
        do_write();
    }

    auto auth_timer = std::make_shared<boost::asio::steady_timer>(socket.get_executor());
    auth_timer->expires_after(std::chrono::seconds(10));
    auth_timer->async_wait([this, self = shared_from_this(), auth_timer](boost::system::error_code ec)
        {
            if (!ec && !chat_identity) send_websocket_close(1008, "auth required");
        });
}


void Session::start()
{
    auto self = shared_from_this();

    socket.async_handshake(boost::asio::ssl::stream_base::server,
        [this, self](boost::system::error_code ec)
        {
            if (ec)
            {
                std::cerr << "TLS handshake failed: " << ec.message() << std::endl;
                return;
            }

            do_read();
        });
}
void Session::do_shutdown()
{
    if (shutting_down) return;
    shutting_down = true;
    auto self = shared_from_this();

    socket.async_shutdown(
        [this, self](boost::system::error_code ec)
        {
            // Ошибки shutdown обычно не критичны — клиент мог уже закрыть соединение
            if (ec && ec != boost::asio::error::eof)
            {
                LOG_WARNING("TLS shutdown warning: " + ec.message());
            }

            boost::system::error_code close_ec;
            socket.next_layer().close(close_ec);
        });
}



void Session::do_websocket_read()
{
    auto self = shared_from_this();

    if (buffer.size() >= 2)
    {
        const unsigned char byte1 =
            static_cast<unsigned char>(buffer[0]);

        const unsigned char byte2 =
            static_cast<unsigned char>(buffer[1]);



        const bool fin =
            (byte1 & 0x80) != 0;

        const bool rsv1 =
            (byte1 & 0x40) != 0;

        const bool rsv2 =
            (byte1 & 0x20) != 0;

        const bool rsv3 =
            (byte1 & 0x10) != 0;

        if (rsv1 || rsv2 || rsv3)
        {
            websocket_protocol_error(
                1002,
                "Unexpected RSV bits");
            return;
        }

        const unsigned char opcode =
            byte1 & 0x0F;

        const bool masked =
            (byte2 & 0x80) != 0;

        // Первоначальная длина из первых 7 бит
        std::size_t payload_length =
            byte2 & 0x7F;

        // Размер заголовка:
        //
        // обычный frame:
        // [2 bytes header]
        //
        // extended 126:
        // [2 bytes header][2 bytes length]
        //
        std::size_t header_size = 2;

        // Control frames:
        // CLOSE (0x8), PING (0x9), PONG (0xA)
        // must always have FIN=1 and payload <= 125 bytes.

        const bool is_control_frame =
            opcode == 0x8 ||
            opcode == 0x9 ||
            opcode == 0xA;

        if (is_control_frame)
        {
            if (!fin)
            {
                LOG_ERROR(
                    "WebSocket control frame "
                    "must have FIN=1"
                );

                do_shutdown();
                return;
            }

            if (payload_length > 125)
            {
                LOG_ERROR(
                    "WebSocket control frame "
                    "payload is too large"
                );

                do_shutdown();
                return;
            }
        }

        if (opcode == 0x0)
        {
            if (!websocket_fragmented)
            {
                LOG_ERROR(
                    "Unexpected WebSocket continuation frame"
                );

                do_shutdown();
                return;
            }
        }

        if (opcode == 0x1 ||
            opcode == 0x2)
        {
            if (websocket_fragmented)
            {
                LOG_ERROR(
                    "New WebSocket message while "
                    "fragmentation is active"
                );

                do_shutdown();
                return;
            }
        }


        // -----------------------------
        // Поддерживаем:
        //
        // 0x1 = TEXT
        // 0x9 = PING
        // 0xA = PONG
        // -----------------------------

        if (opcode != 0x0 &&
            opcode != 0x1 &&
            opcode != 0x2 &&
            opcode != 0x8 &&
            opcode != 0x9 &&
            opcode != 0xA)
        {
            websocket_protocol_error(
                1002,
                "Unknown WebSocket opcode");
            return;
        }

        // -----------------------------
        // Client -> Server обязан быть MASKED
        // -----------------------------

        if (!masked)
        {
            websocket_protocol_error(
                1002,
                "Client frame is not masked");
            return;
        }

        // -----------------------------
        // Payload length
        // -----------------------------

        if (payload_length == 126)
        {
            // Нам нужны ещё 2 байта длины
            if (buffer.size() < 4)
            {
                socket.async_read_some(
                    boost::asio::buffer(temp),
                    [this, self](
                        boost::system::error_code ec,
                        std::size_t length)
                    {
                        if (ec)
                        {
                            if (ec != boost::asio::error::eof)
                            {
                                LOG_ERROR(
                                    "WebSocket read error: " +
                                    ec.message()
                                );
                            }

                            return;
                        }

                        buffer.append(
                            temp.data(),
                            length
                        );

                        do_websocket_read();
                    }
                );

                return;
            }

            // Следующие 2 байта содержат реальную длину
            payload_length =
                (static_cast<std::size_t>(
                    static_cast<unsigned char>(buffer[2])
                    ) << 8)
                |
                static_cast<std::size_t>(
                    static_cast<unsigned char>(buffer[3])
                    );

            header_size = 4;

            if (payload_length < 126)
            {
                LOG_ERROR(
                    "Non-canonical WebSocket payload length"
                );

                do_shutdown();
                return;
            }
        }
        else if (payload_length == 127)
        {
            if (buffer.size() < 10)
            {
                socket.async_read_some(
                    boost::asio::buffer(temp),
                    [this, self](
                        boost::system::error_code ec,
                        std::size_t length)
                    {
                        if (ec)
                        {
                            if (ec != boost::asio::error::eof)
                            {
                                LOG_ERROR(
                                    "WebSocket read error: " +
                                    ec.message()
                                );
                            }

                            return;
                        }

                        buffer.append(
                            temp.data(),
                            length
                        );

                        do_websocket_read();
                    }
                );

                return;
            }

            // Старший бит 64-битной длины обязан быть нулём.
            const unsigned char first_length_byte =
                static_cast<unsigned char>(buffer[2]);

            if ((first_length_byte & 0x80) != 0)
            {
                LOG_ERROR(
                    "Invalid WebSocket 64-bit payload length"
                );

                do_shutdown();
                return;
            }

            std::uint64_t length64 = 0;

            for (int i = 0; i < 8; ++i)
            {
                length64 =
                    (length64 << 8) |
                    static_cast<unsigned char>(
                        buffer[2 + i]
                        );
            }

            // Длину меньше 65536 нужно кодировать
            // без 64-битного формата.
            if (length64 < 65536)
            {
                LOG_ERROR(
                    "Non-canonical WebSocket payload length"
                );

                do_shutdown();
                return;
            }

            // Проверяем лимит до преобразования в size_t
            // и до выделения памяти под payload.
            if (length64 > MAX_WEBSOCKET_MESSAGE_SIZE)
            {
                LOG_ERROR(
                    "WebSocket payload exceeds configured limit"
                );

                do_shutdown();
                return;
            }

            payload_length =
                static_cast<std::size_t>(length64);

            header_size = 10;
        }


        // -----------------------------
        // Размер всего frame
        // -----------------------------
        //
        // header_size
        // + 4 bytes mask
        // + payload
        //

        if (payload_length > MAX_WEBSOCKET_MESSAGE_SIZE)
        {
            LOG_ERROR(
                "WebSocket payload exceeds configured limit"
            );

            do_shutdown();
            return;
        }

        const std::size_t frame_size =
            header_size +
            4 +
            payload_length;

        // -----------------------------
        // Ждём весь frame
        // -----------------------------

        if (buffer.size() < frame_size)
        {
            socket.async_read_some(
                boost::asio::buffer(temp),
                [this, self](
                    boost::system::error_code ec,
                    std::size_t length)
                {
                    if (ec)
                    {
                        if (ec != boost::asio::error::eof)
                        {
                            LOG_ERROR(
                                "WebSocket read error: " +
                                ec.message()
                            );
                        }

                        return;
                    }

                    buffer.append(
                        temp.data(),
                        length
                    );

                    do_websocket_read();
                }
            );

            return;
        }

        // -----------------------------
        // Получаем mask
        // -----------------------------

        unsigned char mask[4];

        for (int i = 0; i < 4; ++i)
        {
            mask[i] =
                static_cast<unsigned char>(
                    buffer[header_size + i]
                    );
        }

        // -----------------------------
        // Unmask payload
        // -----------------------------

        std::string payload(
            payload_length,
            '\0'
        );

        for (std::size_t i = 0;
            i < payload_length;
            ++i)
        {
            const unsigned char masked_byte =
                static_cast<unsigned char>(
                    buffer[header_size + 4 + i]
                    );

            payload[i] = static_cast<char>(
                masked_byte ^ mask[i % 4]
                );
        }

        // -----------------------------
        // Удаляем frame из buffer
        // -----------------------------

        buffer.erase(
            0,
            frame_size
        );


        // -----------------------------
        // CLOSE
        // -----------------------------

        if (opcode == 0x8)
        {
            std::cout
                << "WebSocket CLOSE received"
                << std::endl;

            // CLOSE payload:
            //
            // 0 bytes          -> допустимо
            // 2 bytes          -> status code
            // >2 bytes         -> status code + reason
            //
            // 1 byte           -> запрещено

            if (payload.size() == 1)
            {
                LOG_ERROR(
                    "Invalid WebSocket CLOSE payload"
                );

                do_shutdown();
                return;
            }

            // Если есть Status Code
            if (payload.size() >= 2)
            {
                const std::uint16_t close_code =
                    (static_cast<std::uint16_t>(
                        static_cast<unsigned char>(payload[0])
                        ) << 8)
                    |
                    static_cast<std::uint16_t>(
                        static_cast<unsigned char>(payload[1])
                        );

                std::cout
                    << "WebSocket CLOSE code: "
                    << close_code
                    << std::endl;


                if (!is_valid_websocket_close_code(close_code))
                {
                    LOG_ERROR(
                        "Invalid WebSocket CLOSE code"
                    );

                    do_shutdown();
                    return;
                }

                // Reason начинается с третьего байта
                if (payload.size() > 2)
                {
                    const std::string reason =
                        payload.substr(2);

                   
                        if (!is_valid_utf8(reason))
                        {
                            websocket_protocol_error(
                                1007,
                                "Invalid UTF-8 in close reason");
                            return;
                        }

                    std::cout
                        << "WebSocket CLOSE reason: "
                        << reason
                        << std::endl;
                  
                }

            }
            websocket_close_received = true;
            // Отвечаем своим CLOSE
            if (!websocket_close_sent)
            {
                send_websocket_close();
            }
            else
            {
                // Наш Close уже отправлен — завершаем соединение.
                do_shutdown();
            }


            return;
        }



        bool binary_message = false;

        // -----------------------------
        // Начало fragmented TEXT/BINARY
        // -----------------------------

        if ((opcode == 0x1 ||
            opcode == 0x2) &&
            !fin)
        {
            if (payload.size() > MAX_WEBSOCKET_MESSAGE_SIZE)
            {
                LOG_ERROR(
                    "WebSocket fragmented message exceeds limit"
                );

                do_shutdown();
                return;
            }

            websocket_fragment_buffer = payload;
            websocket_fragmented = true;

            // Запоминаем тип сообщения
            websocket_fragment_binary =
                (opcode == 0x2);

            do_websocket_read();
            return;
        }

        // -----------------------------
        // Continuation frame
        // -----------------------------

        if (opcode == 0x0)
        {

            if (websocket_fragment_buffer.size() >
                MAX_WEBSOCKET_MESSAGE_SIZE - payload.size())
            {
                websocket_protocol_error(
                    1009,
                    "WebSocket message too large");
                return;
                
            }

            websocket_fragment_buffer += payload;

            if (!fin)
            {
                // Сообщение ещё не закончено
                do_websocket_read();
                return;
            }

            // -----------------------------
            // Последний fragment
            // -----------------------------

            payload = std::move(
                websocket_fragment_buffer
            );

            websocket_fragment_buffer.clear();

            // Запоминаем тип всего сообщения
            binary_message =
                websocket_fragment_binary;

            websocket_fragmented = false;
            websocket_fragment_binary = false;
        }

        // -----------------------------
        // PING
        // -----------------------------

        if (opcode == 0x9)
        {
            std::cout
                << "WebSocket PING received"
                << std::endl;

            send_websocket_pong(payload);

            do_websocket_read();
            return;
        }

        // -----------------------------
        // PONG
        // -----------------------------

        if (opcode == 0xA)
        {
            std::cout
                << "WebSocket PONG received"
                << std::endl;

            do_websocket_read();
            return;
        }

        // -----------------------------
        // BINARY
        // -----------------------------

        if (opcode == 0x2 ||
            binary_message)
        {
            std::cout
                << "WebSocket BINARY message: "
                << payload.size()
                << " bytes"
                << std::endl;

            if (chat) chat->on_binary(self); else send_websocket_binary(payload);

            do_websocket_read();
            return;
        }

        // -----------------------------
        // TEXT
        // -----------------------------

        if (opcode == 0x1 || opcode == 0x0)
        {
            if (!is_valid_utf8(payload))
            {
                websocket_protocol_error(
                    1007,
                    "Invalid UTF-8 in text message");
                return;
                
            }

            std::cout
                << "WebSocket message: "
                << payload
                << std::endl;

            if (chat) chat->on_text(self, payload); else send_websocket_text(payload);

            do_websocket_read();
            return;
        }
    }

    // -----------------------------
    // Недостаточно данных
    // даже для заголовка
    // -----------------------------

    socket.async_read_some(
        boost::asio::buffer(temp),
        [this, self](
            boost::system::error_code ec,
            std::size_t length)
        {
            if (ec)
            {
                if (ec != boost::asio::error::eof)
                {
                    LOG_ERROR(
                        "WebSocket read error: " +
                        ec.message()
                    );
                }

                return;
            }

            buffer.append(
                temp.data(),
                length
            );

            do_websocket_read();
        }
    );
};



void Session::send_websocket_text(
    const std::string& message)
{
    constexpr std::uint64_t MAX_MESSAGE_SIZE =
        1024 * 1024;

    if (message.size() > MAX_MESSAGE_SIZE)
    {
        LOG_ERROR(
            "WebSocket text message exceeds 1 MiB"
        );

        do_shutdown();
        return;
    }

    auto frame =
        std::make_shared<std::string>();

    // FIN = 1, opcode = 1 (TEXT)
    frame->push_back(
        static_cast<char>(0x81)
    );

    // Payload length
    if (message.size() <= 125)
    {
        frame->push_back(
            static_cast<char>(message.size())
        );
    }
    else if (message.size() <= 65535)
    {
        // Extended payload: 126 + 2 bytes
        frame->push_back(
            static_cast<char>(126)
        );

        const std::uint16_t length =
            static_cast<std::uint16_t>(
                message.size()
                );

        frame->push_back(
            static_cast<char>(
                (length >> 8) & 0xFF
                )
        );

        frame->push_back(
            static_cast<char>(
                length & 0xFF
                )
        );
    }
    else
    {
        // Extended payload: 127 + 8 bytes
        frame->push_back(
            static_cast<char>(127)
        );

        const std::uint64_t length =
            static_cast<std::uint64_t>(
                message.size()
                );

        for (int i = 7; i >= 0; --i)
        {
            frame->push_back(
                static_cast<char>(
                    (length >> (i * 8)) & 0xFF
                    )
            );
        }
    }

    // Server -> Client: NOT MASKED
    frame->append(message);

    auto self = shared_from_this();

    boost::asio::post(
        socket.get_executor(),
        [this, self, frame]()
        {
            if (websocket_close_sent)
            {
                return;
            }

            enqueue_ws_frame(std::move(frame));
        }
    );
}


void Session::send_websocket_binary(
    const std::string& data)
{
    constexpr std::uint64_t MAX_MESSAGE_SIZE =
        1024 * 1024;

    if (data.size() > MAX_MESSAGE_SIZE)
    {
        LOG_ERROR(
            "WebSocket binary message exceeds 1 MiB"
        );

        do_shutdown();
        return;
    }

    auto frame =
        std::make_shared<std::string>();

    // FIN = 1, opcode = 2 (BINARY)
    frame->push_back(
        static_cast<char>(0x82)
    );

    // Payload length
    if (data.size() <= 125)
    {
        frame->push_back(
            static_cast<char>(data.size())
        );
    }
    else if (data.size() <= 65535)
    {
        // Extended payload: 126 + 2 bytes
        frame->push_back(
            static_cast<char>(126)
        );

        const std::uint16_t length =
            static_cast<std::uint16_t>(
                data.size()
                );

        frame->push_back(
            static_cast<char>(
                (length >> 8) & 0xFF
                )
        );

        frame->push_back(
            static_cast<char>(
                length & 0xFF
                )
        );
    }
    else
    {
        // Extended payload: 127 + 8 bytes
        frame->push_back(
            static_cast<char>(127)
        );

        const std::uint64_t length =
            static_cast<std::uint64_t>(
                data.size()
                );

        for (int i = 7; i >= 0; --i)
        {
            frame->push_back(
                static_cast<char>(
                    (length >> (i * 8)) & 0xFF
                    )
            );
        }
    }

    // Server -> Client: NOT MASKED
    frame->append(data);

    auto self = shared_from_this();

    if (websocket_close_received)
    {
        return;
    }
    boost::asio::post(
        socket.get_executor(),
        [this, self, frame]()
        {
            if (websocket_close_sent)
            {
                return;
            }

            enqueue_ws_frame(frame);
        }
    );
}






void Session::send_websocket_pong(
    const std::string& payload)
{
    if (payload.size() > 125)
    {
        LOG_ERROR(
            "Pong payload is too large"
        );

        do_shutdown();
        return;
    }

    auto frame =
        std::make_shared<std::string>();

    frame->reserve(
        2 + payload.size()
    );

    // FIN = 1
    // opcode = 0xA (PONG)
    frame->push_back(
        static_cast<char>(0x8A)
    );

    // Server -> Client:
    // MASK = 0
    // payload length
    frame->push_back(
        static_cast<char>(
            payload.size()
            )
    );

    // Payload должен быть
    // точно таким же, как у Ping
    frame->append(payload);

    auto self =
        shared_from_this();

    boost::asio::post(
        socket.get_executor(),
        [this, self, frame]()
        {
            if (websocket_close_sent)
            {
                return;
            }
            enqueue_ws_frame(std::move(frame));
        }
    );
}
Session::Session(tcp::socket socket, ThreadPool& pool, Router& router,
    boost::asio::ssl::context& ssl_context, ChatService* chat)
    : socket(std::move(socket), ssl_context), pool(pool), router(router), chat(chat) {}
bool Session::allow_chat_message()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - rate_window_start > std::chrono::seconds(10))
    {
        rate_window_start = now;
        rate_count = 0;
    }
    return ++rate_count <= 10;
}

Session::~Session()
{
    if (chat && chat_identity) chat->detach(chat_identity->user_id);
}



