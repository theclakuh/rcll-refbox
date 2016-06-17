
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
#include <msgs/OrderInfo.pb.h>

using namespace protobuf_comm;
using namespace fawkes;

boost::asio::io_service io_service_;

static bool quit = false;
ProtobufStreamClient *client_ = NULL;

unsigned int id_;
crossover_msgs::CapColor cap_color_;
unsigned int quantity_requested_;

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
  printf("Sending Order\n");
  crossover_msgs::Order order;
  order.set_id(id_);
  order.set_cap_color(cap_color_);
  order.set_quantity_requested(quantity_requested_);
  order.set_quantity_delivered(0);

  client_->send(order);

//  usleep(200000);
//  quit = true;
//  io_service_.stop();
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
  std::shared_ptr<crossover_msgs::Order> order;
  if (order = std::dynamic_pointer_cast<crossover_msgs::Order>(msg)) {
    printf("requested %u\tdelivert %u\n", order->quantity_requested(), order->quantity_delivered());
    if (order->quantity_requested() <= order->quantity_delivered()) {
      printf("ORDER IS DONE for RCLL\n");
    }
  } else {
    printf("Can't decode msg\n");
  }
}



void
usage(const char *progname)
{
  printf("Usage: %s <order-id> <cap-color>\n", progname);
}


int
main(int argc, char **argv)
{
  client_ = new ProtobufStreamClient();
  MessageRegister & pcmr = client_->message_register();
  pcmr.add_message_type<crossover_msgs::Order>();

  ArgumentParser argp(argc, argv, "");

  if (argp.num_items() < 2) {
    usage(argv[0]);
    exit(1);
  }


  id_ = (unsigned int)argp.parse_item_int(0);
//  id_ = (unsigned int)(argp.items()[0]);
  quantity_requested_ = 1;

  if ( ! crossover_msgs::CapColor_Parse(std::string(argp.items()[1]), &cap_color_) ) {
    printf("cap color\n");
    exit(-1);
  }

  //MessageRegister & message_register = client_->message_register();

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
