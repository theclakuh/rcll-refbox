
/***************************************************************************
 *  rcll-set-machine-state.cpp - set fake MPS state
 *
 *  Created: Fri Apr 17 22:38:03 2015
 *  Copyright  2013-2015  Tim Niemueller [www.niemueller.de]
 ****************************************************************************/

/*  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the
 *   distribution.
 * - Neither the name of the authors nor the names of its contributors
 *   may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define BOOST_DATE_TIME_POSIX_TIME_STD_CONFIG

#include <protobuf_comm/client.h>
#include <utils/system/argparser.h>

#include <msgs/rc-i-crossover-msgs/OrderInfo.pb.h>

using namespace protobuf_comm;
using namespace fawkes;

boost::asio::io_service io_service_;

static bool quit = false;
ProtobufStreamClient *client_ = NULL;

void
signal_handler(const boost::system::error_code& error, int signum)
{
  if (!error) {
    quit = true;
    io_service_.stop();
  }
}

void
handle_connected()
{
  printf("Connected to refbox\n");
}

void
handle_disconnected(const boost::system::error_code &ec)
{
  quit = true;
  io_service_.stop();
}

void
handle_message(uint16_t component_id, uint16_t msg_type,
	       std::shared_ptr<google::protobuf::Message> msg)
{
  std::shared_ptr<crossover_msgs::Exchange> request;
  if (request = std::dynamic_pointer_cast<crossover_msgs::Exchange>(msg)) {
    if (request->completed() == true) {
      printf("completed is true, something went wrong\n");
      return;
    }
    printf("Transport product to at_work arena\n");
    sleep(1);

    crossover_msgs::Exchange repley;
    repley.set_id( request->id() );
    repley.set_place_from( request->place_from() );
    repley.set_place_to( request->place_to() );
    repley.set_completed( true );

    client_->send( repley );
    printf("DONE\n");
  } else {
    printf("Can't decode msg\n");
  }
}

int
main(int argc, char **argv)
{
  client_ = new ProtobufStreamClient();
  MessageRegister & pcmr = client_->message_register();
  pcmr.add_message_type<crossover_msgs::Exchange>();

  client_->signal_received().connect(handle_message);
  client_->signal_connected().connect(handle_connected);
  client_->signal_disconnected().connect(handle_disconnected);
  client_->async_connect("localhost", 4444);

#if BOOST_ASIO_VERSION >= 100601
  // Construct a signal set registered for process termination.
  boost::asio::signal_set signals(io_service_, SIGINT, SIGTERM);

  // Start an asynchronous wait for one of the signals to occur.
  signals.async_wait(signal_handler);
#endif

  do {
    io_service_.run();
    io_service_.reset();
  } while (! quit);

  delete client_;

  // Delete all global objects allocated by libprotobuf
  google::protobuf::ShutdownProtobufLibrary();
}
