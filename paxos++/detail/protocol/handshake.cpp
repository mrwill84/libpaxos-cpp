#include <boost/ref.hpp>
#include <boost/bind.hpp>
#include <boost/asio/placeholders.hpp>

#include <boost/uuid/uuid_io.hpp>

#include "../../configuration.hpp"
#include "../../quorum.hpp"
#include "../util/debug.hpp"

#include "command.hpp"
#include "protocol.hpp"
#include "handshake.hpp"

namespace paxos { namespace detail { namespace protocol {

handshake::handshake (
   detail::protocol::protocol & protocol)
   : protocol_ (protocol)
{
}

void
handshake::start ()
{
   step1 ();
}

void
handshake::receive_handshake_start (
   tcp_connection::pointer      connection,
   command const &              command)
{
   step3 (connection,
          command);
}

void
handshake::step1 ()
{
   if (protocol_.quorum ().servers ().empty () == true)
   {
      /*!
        This is a bit of an ugly shortcut, but the algorithm below assumes that
        our quorum consists of at least one server, which is not the case when
        we're the leader.
        
        \todo Once again, the role distinction between "leader" and "follower" must
              be made more clear. At the moment, a leader is both a leader and a follower,
              but this is not made clear in the quorum. What we need to do is make the 
              leader connect to itself and make *that* node the "follower inside the leader".
              If that makes sense, somehow.
       */
      protocol_.quorum ().adjust_our_state (remote_server::state_leader);
   }

   for (auto const & i : protocol_.quorum ().servers ())
   {
      boost::asio::ip::tcp::endpoint const & endpoint = i.first;      

      tcp_connection::pointer new_connection = 
         tcp_connection::create (protocol_.io_service ());
      
      new_connection->socket ().async_connect (endpoint,
                                              boost::bind (&handshake::step2,
                                                           this,
                                                           boost::ref (endpoint),
                                                           new_connection,
                                                           boost::asio::placeholders::error));
   }
}

void
handshake::step2 (
   boost::asio::ip::tcp::endpoint const &       endpoint,
   tcp_connection::pointer                      connection,
   boost::system::error_code const &            error)
{
   if (error)
   {
      PAXOS_WARN ("An error occured while establishing a connection, marking host as dead: " << error.message ());
      protocol_.quorum ().lookup (endpoint).set_state (remote_server::state_dead);
      return;
   }

   command command;
   command.set_type (command::type_handshake_start);

   PAXOS_DEBUG ("Connection established!");

   /*!
     Send this command to the other side, which will enter in handshake::receive_handshake_start
     as defined in protocol.cpp's handle_command () function.
    */
   protocol_.write_command (command,
                            connection);

   /*!
     And now we expect a response soon from the other side in response to our handshake request.
    */
   connection->start_timeout (boost::posix_time::milliseconds (configuration::timeout));
   protocol_.read_command (connection,
                           boost::bind (&handshake::step4,
                                        this,
                                        boost::ref (endpoint),
                                        connection,
                                        _1));
}

void
handshake::step3 (
   tcp_connection::pointer      connection,
   command const &              command)
{
   PAXOS_DEBUG ("received handshake request");

   detail::protocol::command ret;
   ret.set_type (command::type_handshake_response);
   ret.set_host_id (protocol_.quorum ().self ().id_);
   ret.set_host_endpoint (protocol_.quorum ().self ().endpoint_);
   ret.set_host_state (protocol_.quorum ().self ().state_);

   protocol_.write_command (ret,
                            connection);
}


void
handshake::step4 (
   boost::asio::ip::tcp::endpoint const &       endpoint,
   tcp_connection::pointer                      connection,
   command const &                              command)
{
   PAXOS_DEBUG ("step4 received command, "
                "host id = " << command.host_id () << ", "
                "endpoint = " << endpoint << ", "
                "state = " << remote_server::to_string (command.host_state ()));

   /*! 
     This validates that the endpoint this host thinks it is, is the same as the endpoint
     we just connected to.
   */
   PAXOS_ASSERT (command.host_endpoint () == endpoint);

   /*!
     Now, update the host id and the state the host thinks it's in.
   */
   protocol_.quorum ().lookup (endpoint).set_state (command.host_state ());
   protocol_.quorum ().lookup (endpoint).set_id (command.host_id ());

   /*!
     And update our connection with the connection, if we do not already have
     one. 
    */
   if (protocol_.quorum ().lookup (endpoint).has_connection () == false)
   {
      protocol_.quorum ().lookup (endpoint).set_connection (connection); 
   }
}

}; }; };
