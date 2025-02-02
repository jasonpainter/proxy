//
// tcpproxy_server.cpp
// ~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2007 Arash Partow (http://www.partow.net)
// URL: http://www.partow.net/programming/tcpproxy/index.html
//
// Distributed under the Boost Software License, Version 1.0.
//
//
// Description
// ~~~~~~~~~~~
// The  objective of  the TCP  proxy server  is to  act  as  an
// intermediary  in order  to 'forward'  TCP based  connections
// from external clients onto a singular remote server.
//
// The communication flow in  the direction from the  client to
// the proxy to the server is called the upstream flow, and the
// communication flow in the  direction from the server  to the
// proxy  to  the  client   is  called  the  downstream   flow.
// Furthermore  the   up  and   down  stream   connections  are
// consolidated into a single concept known as a bridge.
//
// In the event  either the downstream  or upstream end  points
// disconnect, the proxy server will proceed to disconnect  the
// other  end  point  and  eventually  destroy  the  associated
// bridge.
//
// The following is a flow and structural diagram depicting the
// various elements  (proxy, server  and client)  and how  they
// connect and interact with each other.

//
//                                    ---> upstream --->           +---------------+
//                                                     +---->------>               |
//                               +-----------+         |           | Remote Server |
//                     +--------->          [x]--->----+  +---<---[x]              |
//                     |         | TCP Proxy |            |        +---------------+
// +-----------+       |  +--<--[x] Server   <-----<------+
// |          [x]--->--+  |      +-----------+
// |  Client   |          |
// |           <-----<----+
// +-----------+
//                <--- downstream <---
//
//


#include <cstdlib>
#include <cstddef>
#include <iostream>
#include <string>
#include <memory>
#include <mutex>
#include <thread>

#include <asio.hpp>

namespace tcp_proxy
{
   namespace ip = asio::ip;

   class bridge : public std::enable_shared_from_this<bridge>
   {
   public:

      typedef ip::tcp::socket socket_type;
      typedef std::shared_ptr<bridge> ptr_type;

      bridge(asio::io_context& ios)
      : downstream_socket_(ios),
        upstream_socket_  (ios)
      {}

      socket_type& downstream_socket()
      {
         // Client socket
         return downstream_socket_;
      }

      socket_type& upstream_socket()
      {
         // Remote server socket
         return upstream_socket_;
      }

      void start(const std::string& upstream_host, unsigned short upstream_port)
      {
         // Attempt connection to remote server (upstream side)
         upstream_socket_.async_connect(
              ip::tcp::endpoint(
                   asio::ip::make_address(upstream_host),
                   upstream_port),
               std::bind(&bridge::handle_upstream_connect,
                    shared_from_this(),
                    std::placeholders::_1));
      }

      void handle_upstream_connect(const std::error_code& error)
      {
         if (!error)
         {
            // Setup async read from remote server (upstream)
            upstream_socket_.async_read_some(
                 asio::buffer(upstream_data_,max_data_length),
                 std::bind(&bridge::handle_upstream_read,
                      shared_from_this(),
                      std::placeholders::_1,
                      std::placeholders::_2));

            // Setup async read from client (downstream)
            downstream_socket_.async_read_some(
                 asio::buffer(downstream_data_,max_data_length),
                 std::bind(&bridge::handle_downstream_read,
                      shared_from_this(),
                      std::placeholders::_1,
                      std::placeholders::_2));
         }
         else
            close();
      }

   private:

      /*
         Section A: Remote Server --> Proxy --> Client
         Process data recieved from remote sever then send to client.
      */

      // Read from remote server complete, now send data to client
      void handle_upstream_read(const std::error_code& error,
                                const size_t& bytes_transferred)
      {
         if (!error)
         {
            async_write(downstream_socket_,
                 asio::buffer(upstream_data_,bytes_transferred),
                 std::bind(&bridge::handle_downstream_write,
                      shared_from_this(),
                      std::placeholders::_1));
         }
         else
            close();
      }

      // Write to client complete, Async read from remote server
      void handle_downstream_write(const std::error_code& error)
      {
         if (!error)
         {
            upstream_socket_.async_read_some(
                 asio::buffer(upstream_data_,max_data_length),
                 std::bind(&bridge::handle_upstream_read,
                      shared_from_this(),
                      std::placeholders::_1,
                      std::placeholders::_2));
         }
         else
            close();
      }
      // *** End Of Section A ***


      /*
         Section B: Client --> Proxy --> Remove Server
         Process data recieved from client then write to remove server.
      */

      // Read from client complete, now send data to remote server
      void handle_downstream_read(const std::error_code& error,
                                  const size_t& bytes_transferred)
      {
         if (!error)
         {
            async_write(upstream_socket_,
                  asio::buffer(downstream_data_,bytes_transferred),
                  std::bind(&bridge::handle_upstream_write,
                        shared_from_this(),
                        std::placeholders::_1));
         }
         else
            close();
      }

      // Write to remote server complete, Async read from client
      void handle_upstream_write(const std::error_code& error)
      {
         if (!error)
         {
            downstream_socket_.async_read_some(
                 asio::buffer(downstream_data_,max_data_length),
                 std::bind(&bridge::handle_downstream_read,
                      shared_from_this(),
                      std::placeholders::_1,
                      std::placeholders::_2));
         }
         else
            close();
      }
      // *** End Of Section B ***

      void close()
      {
         std::scoped_lock lock(mutex_);

         if (downstream_socket_.is_open())
         {
            downstream_socket_.close();
         }

         if (upstream_socket_.is_open())
         {
            upstream_socket_.close();
         }
      }

      socket_type downstream_socket_;
      socket_type upstream_socket_;

      enum { max_data_length = 8192 }; //8KB
      unsigned char downstream_data_[max_data_length];
      unsigned char upstream_data_  [max_data_length];

      std::mutex mutex_;

   public:

      class acceptor
      {
      public:

         acceptor(asio::io_context& io_service,
                  const std::string& local_host, unsigned short local_port,
                  const std::string& upstream_host, unsigned short upstream_port)
         : io_service_(io_service),
           localhost_address(asio::ip::make_address_v4(local_host)),
           acceptor_(io_service_,ip::tcp::endpoint(localhost_address,local_port)),
           upstream_port_(upstream_port),
           upstream_host_(upstream_host)
         {}

         bool accept_connections()
         {
            try
            {
               session_ = std::shared_ptr<bridge>(new bridge(io_service_));

               acceptor_.async_accept(session_->downstream_socket(),
                    std::bind(&acceptor::handle_accept,
                         this,
                         std::placeholders::_1));
            }
            catch(std::exception& e)
            {
               std::cerr << "acceptor exception: " << e.what() << std::endl;
               return false;
            }

            return true;
         }

      private:

         void handle_accept(const std::error_code& error)
         {
            if (!error)
            {
               session_->start(upstream_host_,upstream_port_);

               if (!accept_connections())
               {
                  std::cerr << "Failure during call to accept." << std::endl;
               }
            }
            else
            {
               std::cerr << "Error: " << error.message() << std::endl;
            }
         }

         asio::io_context& io_service_;
         ip::address_v4 localhost_address;
         ip::tcp::acceptor acceptor_;
         ptr_type session_;
         unsigned short upstream_port_;
         std::string upstream_host_;
      };

   };
}

int main(int argc, char* argv[])
{
   if (argc != 5)
   {
      std::cerr << "usage: tcpproxy_server <local host ip> <local port> <forward host ip> <forward port>" << std::endl;
      return 1;
   }

   const unsigned short local_port   = static_cast<unsigned short>(::atoi(argv[2]));
   const unsigned short forward_port = static_cast<unsigned short>(::atoi(argv[4]));
   const std::string local_host      = argv[1];
   const std::string forward_host    = argv[3];

   asio::io_context ios;

   try
   {
      tcp_proxy::bridge::acceptor acceptor(ios,
                                           local_host, local_port,
                                           forward_host, forward_port);

      acceptor.accept_connections();

      ios.run();
   }
   catch(std::exception& e)
   {
      std::cerr << "Error: " << e.what() << std::endl;
      return 1;
   }

   return 0;
}

/*
 * [Note] On posix systems the tcp proxy server build command is as follows:
 * c++ -pedantic -ansi -Wall -Werror -O3 -o tcpproxy_server tcpproxy_server.cpp -L/usr/lib -lstdc++ -lpthread -lboost_thread -lboost_system
 */
