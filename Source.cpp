#include "Server.h"
#include <iostream>

#include "Logger.h"
#include "SessionRepository.h"
#include "Config.h"
#include "AuthController.h"
#include "SessionCleaner.h"
#include "ChatRepository.h"
#include "ChatController.h"
#include "ChatService.h" 


int main()
{

    Logger::instance().init("server.log");
    LOG_INFO("Server starting...");
    Config config = Config::load("config.json");
    Router router;

    ConnectionPool db_pool(
        config.database.pool_size,
        config.database.to_connection_string()
    );
    SessionRepository session_repository(db_pool, std::chrono::minutes(30), std::chrono::hours(12));
    
   

    SessionCleaner session_cleaner(session_repository);

    ThreadPool pool(4);
    ChatService chat(
        db_pool,
        session_repository,
        pool,
        { "https://localhost:8080/" }
    );
    ChatController chat_controller(db_pool, session_repository);
    chat_controller.register_routes(router);   // до запуска сервера
    AuthController auth_controller(db_pool, session_repository,chat);
    auth_controller.register_routes(router);

    try
    {
        boost::asio::io_context io;

        Server server(io, 8765, pool, router,chat);

        std::cout << "Server started on port 8080\n";

        io.run();
        /*pool.shutdown();*/
    }
    catch (std::exception& e)
    {
        std::cout << "Error: " << e.what() << std::endl;
    }
}