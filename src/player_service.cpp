#include "player_service.h"
#include "mpv_manager.h"
#include "picojson.h"

#include <log4cplus/loggingmacros.h>
#include <boost/filesystem.hpp>
#include <chrono>

player_service::player_service(const http_config &config):done_polling(false),mpv(new mpv_manager()),wsServer(9090,2),multimedia_folders(config.multimedia_folders),
                                                    logger(log4cplus::Logger::getInstance("player_service"))
{
    using namespace std::placeholders;   
    handlers_map["play"]=std::bind(&player_service::play_command,this,_1);
    handlers_map["stop"]=std::bind(&player_service::stop_command,this,_1);      
}
player_service::~player_service() = default;

void player_service::stop()
{
    wsServer.stop();
    mpv->quit();
    done_polling = true;
    mpv_status_polling_thread.join();
}

void player_service::start()
{
    setup_polling_thread();
    setup_ws_server();
    wsServer.start();
}

void player_service::setup_polling_thread()
{
    mpv_status_polling_thread = std::thread([this](){
        while(!done_polling)
        {
            using namespace std::chrono_literals;
            std::this_thread::sleep_for(2s);
            for(auto con: wsServer.get_connections())
            {
                report_status(con);
            }
        }
    });
}

void player_service::setup_ws_server()
{
    auto& endpoint = wsServer.endpoint["^/player/?$"];    
    endpoint.onmessage = [this](ConnectionPtr connection, MessagePtr message) {
        auto message_str = message->string();            
        LOG4CPLUS_DEBUG(logger, "Server: Message received: \"" << message_str << "\" from " << (size_t)connection.get());  
        handle_message(message_str);            
                
        /*auto send_stream=std::make_shared<WsServer::SendStream>();
        *send_stream << message_str;
        wsServer.send(connection, send_stream, [](const boost::system::error_code& ec){
            if(ec) 
            {
                log4cplus::Logger logger(log4cplus::Logger::getInstance("player_service_send"));
                LOG4CPLUS_DEBUG(logger, "Server: Error sending message. " <<
                //See http://www.boost.org/doc/libs/1_55_0/doc/html/boost_asio/reference.html, Error Codes for error code meanings
                        "Error: " << ec << ", error message: " << ec.message());
            }
        });        */
    };
    
    endpoint.onopen = [this](ConnectionPtr connection){
        LOG4CPLUS_DEBUG(logger, "Opened connection ");                  
        report_status(connection);
    };
    endpoint.onclose = [this](ConnectionPtr /*connection*/,int status, const std::string& reason){
        LOG4CPLUS_DEBUG(logger, "Closed connection "<<status<<", reason: "<<reason);                  
    }; 
}

void player_service::report_status(ConnectionPtr connection)
{
    mpv_status status = mpv->get_mpv_status();
    picojson::object status_obj;
    status_obj.insert({"file_size",picojson::value((int64_t)status.file_size)});
    status_obj.insert({"idle",picojson::value(status.idle)});
    status_obj.insert({"file_name",picojson::value(status.loaded_filename)});
    status_obj.insert({"percent_complete",picojson::value((int64_t)status.percent_complete)});
    status_obj.insert({"time_position",picojson::value(status.time_position)});
    status_obj.insert({"total_duration",picojson::value(status.total_duration)});
    auto send_stream=std::make_shared<WsServer::SendStream>();
    *send_stream << picojson::value(status_obj).serialize();
    wsServer.send(connection, send_stream, [](const boost::system::error_code&){});            
}

void player_service::handle_message(const std::string& msg)
{
    picojson::value msg_val;
    std::string err = picojson::parse(msg_val, msg);
    if(!err.empty())
    {
        LOG4CPLUS_ERROR(logger, "Message not json "<<msg<<". Error:"<<err);                  
        return;
    }
    auto name_obj = msg_val.get("name");
    if(name_obj.is<picojson::null>())
    {
        LOG4CPLUS_ERROR(logger, msg<<" has no name");                  
        return;        
    }
    const std::string& command_name = name_obj.get<std::string>();
    auto it = handlers_map.find(command_name);
    if(it == handlers_map.end())
    {
        LOG4CPLUS_ERROR(logger, "Unknown command "<<command_name);                       
    }
    else
    {
        //execute it
        it->second(msg_val);        
    }
}

void player_service::play_command(const picojson::value& val)
{
    auto link_obj = val.get("link");
    if(link_obj.is<picojson::null>())
    {
        LOG4CPLUS_ERROR(logger, val<<" has no link");                  
        return;        
    }
    const std::string& link = link_obj.get<std::string>();    
    boost::filesystem::path link_path(link);
    auto path_it = link_path.begin();
    boost::filesystem::path set_path = *path_it;
    ++path_it;
    boost::filesystem::path path_to_file;
    for(;path_it != link_path.end();++path_it)
    {
        path_to_file /= *path_it;
    }
    
    if(multimedia_folders.find(set_path.string()) != multimedia_folders.end())
    {        
        boost::filesystem::path file_to_play(multimedia_folders.at(set_path.string()));
        file_to_play /= path_to_file;
        LOG4CPLUS_DEBUG(logger, "Playing "<<file_to_play);
        if(exists(file_to_play))
        {
            mpv->play(file_to_play.string());
        }        
    }
}

void player_service::stop_command(const picojson::value&)
{
    LOG4CPLUS_DEBUG(logger, " Calling STOP");         
    mpv->stop();
}