
/***************************************************************************
 *  refbox.cpp - LLSF RefBox main program
 *
 *  Created: Thu Feb 07 11:04:17 2013
 *  Copyright  2013  Tim Niemueller [www.niemueller.de]
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

#include "refbox.h"
#include "clips_logger.h"

#include <core/threading/mutex.h>
#include <core/version.h>
#include <config/yaml.h>
#include <protobuf_clips/communicator.h>
#include <protobuf_comm/peer.h>
#include <mps_placing_clips/mps_placing_clips.h>
#include <logging/multi.h>
#include <logging/file.h>
#include <logging/network.h>
#include <logging/console.h>
#include <mps_comm/base_station.h>

#include <boost/bind.hpp>
#include <boost/format.hpp>
#if BOOST_ASIO_VERSION < 100601
#  include <csignal>
#endif
#ifdef HAVE_MONGODB
#  include <mongo/client/dbclient.h>
#  include <mongodb_log/mongodb_log_logger.h>
#  include <mongodb_log/mongodb_log_protobuf.h>
#  ifdef HAVE_MONGODB_VERSION_H
#    include <mongo/version.h>
#  endif
#endif
#ifdef HAVE_AVAHI
#  include <netcomm/dns-sd/avahi_thread.h>
#  include <netcomm/utils/resolver.h>
#endif

#include <string>

using namespace protobuf_comm;
using namespace protobuf_clips;
using namespace llsf_utils;

namespace llsfrb {
#if 0 /* just to make Emacs auto-indent happy */
}
#endif

#if BOOST_ASIO_VERSION < 100601
LLSFRefBox *g_refbox = NULL;
static void handle_signal(int signum)
{
  if (g_refbox) {
    g_refbox->handle_signal(boost::system::errc::make_error_code(boost::system::errc::success),
			    signum);
  }
}
#endif

/** @class LLSFRefBox "refbox.h"
 * LLSF referee box main application.
 * @author Tim Niemueller
 */ 

/** Constructor.
 * @param argc number of arguments passed
 * @param argv array of arguments
 */
LLSFRefBox::LLSFRefBox(int argc, char **argv)
  : clips_mutex_(fawkes::Mutex::RECURSIVE), timer_(io_service_)
{
  pb_comm_ = NULL;
  
  config_ = new YamlConfiguration(CONFDIR);
  config_->load("config.yaml");

  cfg_clips_dir_ = std::string(SHAREDIR) + "/games/rcll/";

  try {
    cfg_timer_interval_ = config_->get_uint("/llsfrb/clips/timer-interval");
  } catch (fawkes::Exception &e) {
    delete config_;
    throw;
  }

  log_level_ = Logger::LL_INFO;
  try {
    std::string ll = config_->get_string("/llsfrb/log/level");
    if (ll == "debug") {
      log_level_ = Logger::LL_DEBUG;
    } else if (ll == "info") {
      log_level_ = Logger::LL_INFO;
    } else if (ll == "warn") {
      log_level_ = Logger::LL_WARN;
    } else if (ll == "error") {
      log_level_ = Logger::LL_ERROR;
    }
  } catch (fawkes::Exception &e) {} // ignored, use default

  MultiLogger *mlogger = new MultiLogger();
  mlogger->add_logger(new ConsoleLogger(log_level_));
  try {
    std::string logfile = config_->get_string("/llsfrb/log/general");
    mlogger->add_logger(new FileLogger(logfile.c_str(), log_level_));
  } catch (fawkes::Exception &e) {} // ignored, use default
  logger_ = mlogger;


  cfg_machine_assignment_ = ASSIGNMENT_2014;
  try {
    std::string m_ass_str = config_->get_string("/llsfrb/game/machine-assignment");
    if (m_ass_str == "2013") {
      cfg_machine_assignment_ = ASSIGNMENT_2013;
    } else if (m_ass_str == "2014") {
      cfg_machine_assignment_ = ASSIGNMENT_2014;
    } else {
      logger_->log_warn("RefBox", "Invalid machine assignment '%s', using 2014",
			m_ass_str.c_str());
      cfg_machine_assignment_ = ASSIGNMENT_2014;
    }
  } catch (fawkes::Exception &e) {} // ignored, use default
  logger_->log_info("RefBox", "Using %s machine assignment",
		    (cfg_machine_assignment_ == ASSIGNMENT_2013) ? "2013" : "2014");

  try {
    mps_ = NULL;
    if (config_->get_bool("/llsfrb/mps/enable")) {
      mps_ = new MPSRefboxInterface("MPSInterface");

      std::string prefix = "/llsfrb/mps/stations/";

      std::set<std::string> mps_configs;
      std::set<std::string> ignored_mps_configs;
      
#if __cplusplus >= 201103L
      std::unique_ptr<Configuration::ValueIterator> i(config_->search(prefix.c_str()));
#else
      std::auto_ptr<Configuration::ValueIterator> i(config_->search(prefix.c_str()));
#endif
      while (i->next()) {

	std::string cfg_name = std::string(i->path()).substr(prefix.length());
	cfg_name = cfg_name.substr(0, cfg_name.find("/"));

	if ( (mps_configs.find(cfg_name) == mps_configs.end()) &&
	     (ignored_mps_configs.find(cfg_name) == ignored_mps_configs.end()) )
	{

	  std::string cfg_prefix = prefix + cfg_name + "/";

	  printf("Config: %s  prefix %s\n", cfg_name.c_str(), cfg_prefix.c_str());

	  bool active = true;
	  try {
	    active = config_->get_bool((cfg_prefix + "active").c_str());
	  } catch (Exception &e) {} // ignored, assume enabled

	  if (active) {
      mps_comm::Machine *mps;
 	    std::string mpstype = config_->get_string((cfg_prefix + "type").c_str());  
	    std::string mpsip = config_->get_string((cfg_prefix + "host").c_str());
	    unsigned int port = config_->get_uint((cfg_prefix + "port").c_str());

      std::string connection_string = "plc";
      try {
        // common setting for all machines
        connection_string = config_->get_string((prefix + "connection").c_str());
			} catch (Exception &e) {
			}
			try {
        // machine-specific setting
				connection_string = config_->get_string((cfg_prefix + "connection").c_str());
			} catch (Exception &e) {
			}

      Machine::ConnectionMode connection_mode;
      if (connection_string == "plc") {
        connection_mode = Machine::PLC;
      } else if (connection_string == "simulation") {
        connection_mode = Machine::SIMULATION;
      } else if (connection_string == "mockup") {
        connection_mode = Machine::MOCKUP;
      } else {
				throw Exception("Unexpected config value for key '%s': '%s'",
							          (cfg_prefix + "connection").c_str(),
							          connection_string.c_str());
			}

			if(mpstype == "BS") {
	      logger_->log_info("RefBox", "Adding BS %s:%u", mpsip.c_str(), port);
        mps = new BaseStation(cfg_name, mpsip, port, connection_mode);
	    }
	    else if(mpstype == "CS") {
	      logger_->log_info("RefBox", "Adding CS %s:%u", mpsip.c_str(), port, cfg_name.c_str());
	      mps = new CapStation(cfg_name, mpsip, port, connection_mode);
	    }
	    else if(mpstype == "RS") {
	      logger_->log_info("RefBox", "Adding RS %s:%u", mpsip.c_str(), port);
	      mps = new RingStation(cfg_name, mpsip, port, connection_mode);
	    }
	    else if(mpstype == "DS") {
	      logger_->log_info("RefBox", "Adding DS %s:%u", mpsip.c_str(), port);
	      mps = new DeliveryStation(cfg_name, mpsip, port, connection_mode);
	    }
	    else {
	      throw fawkes::Exception("this type wont match");
	    }
      mps->connect_PLC();
      mps_->insertMachine(cfg_name, mps);
	    mps_configs.insert(cfg_name);
	  } else {
	    ignored_mps_configs.insert(cfg_name);
	  }
	}
      }
    }
  } catch (Exception &e) {
    throw;
  }

  
  clips_ = new CLIPS::Environment();
  setup_protobuf_comm();
  setup_clips();

  mps_placing_generator_ = std::shared_ptr<mps_placing_clips::MPSPlacingGenerator>(
        new mps_placing_clips::MPSPlacingGenerator(clips_, clips_mutex_)
        );

  mlogger->add_logger(new NetworkLogger(pb_comm_->server(), log_level_));

 #ifdef HAVE_MONGODB
  cfg_mongodb_enabled_ = false;
  try {
    cfg_mongodb_enabled_ = config_->get_bool("/llsfrb/mongodb/enable");
  } catch (fawkes:: Exception &e) {} // ignore, use default

  if (cfg_mongodb_enabled_) {
    cfg_mongodb_hostport_     = config_->get_string("/llsfrb/mongodb/hostport");
    std::string mdb_text_log  = config_->get_string("/llsfrb/mongodb/collections/text-log");
    std::string mdb_clips_log = config_->get_string("/llsfrb/mongodb/collections/clips-log");
    std::string mdb_protobuf  = config_->get_string("/llsfrb/mongodb/collections/protobuf");
    mlogger->add_logger(new MongoDBLogLogger(cfg_mongodb_hostport_, mdb_text_log));

    clips_logger_->add_logger(new MongoDBLogLogger(cfg_mongodb_hostport_, mdb_clips_log));

    mongodb_protobuf_ = new MongoDBLogProtobuf(cfg_mongodb_hostport_, mdb_protobuf);

    
    mongo::DBClientConnection *conn =
      new mongo::DBClientConnection(/* auto reconnect */ true);
    mongodb_ = conn;
    std::string errmsg;
    if (! conn->connect(cfg_mongodb_hostport_, errmsg)) {
      throw fawkes::Exception("Could not connect to MongoDB at %s: %s",
			      cfg_mongodb_hostport_.c_str(), errmsg.c_str());
    }

    setup_clips_mongodb();

    pb_comm_->server()->signal_received()
      .connect(boost::bind(&LLSFRefBox::handle_server_client_msg, this, _1, _2, _3, _4));
    pb_comm_->server()->signal_receive_failed()
      .connect(boost::bind(&LLSFRefBox::handle_server_client_fail, this, _1, _2, _3, _4));

    pb_comm_->signal_server_sent()
      .connect(boost::bind(&LLSFRefBox::handle_server_sent_msg, this, _1, _2));
    pb_comm_->signal_client_sent()
      .connect(boost::bind(&LLSFRefBox::handle_client_sent_msg, this, _1, _2, _3));
    pb_comm_->signal_peer_sent()
      .connect(boost::bind(&LLSFRefBox::handle_peer_sent_msg, this, _2));

  }
#endif

  start_clips();

#ifdef HAVE_MONGODB
  // we can do this only after CLIPS was started as it initiates the private peers
  if (cfg_mongodb_enabled_) {
    const std::map<long int, protobuf_comm::ProtobufBroadcastPeer *> &peers =
      pb_comm_->peers();
    for (auto p : peers) {
      p.second->signal_received()
	.connect(boost::bind(&LLSFRefBox::handle_peer_msg, this, _1, _2, _3, _4));
    }
  }
#endif


#ifdef HAVE_AVAHI
  unsigned int refbox_port = config_->get_uint("/llsfrb/comm/server-port");
  avahi_thread_ = new fawkes::AvahiThread();
  avahi_thread_->start();
  nnresolver_   = new fawkes::NetworkNameResolver(avahi_thread_);
  fawkes::NetworkService *refbox_service =
    new fawkes::NetworkService(nnresolver_, "RefBox on %h", "_refbox._tcp", refbox_port);
  avahi_thread_->publish_service(refbox_service);
  delete refbox_service;
#endif

}

/** Destructor. */
LLSFRefBox::~LLSFRefBox()
{
  timer_.cancel();

#ifdef HAVE_AVAHI
  avahi_thread_->cancel();
  avahi_thread_->join();
  delete avahi_thread_;
  delete nnresolver_;
#endif

  //std::lock_guard<std::recursive_mutex> lock(clips_mutex_);
  {
    fawkes::MutexLocker lock(&clips_mutex_);
    clips_->assert_fact("(finalize)");
    clips_->refresh_agenda();
    clips_->run();

    finalize_clips_logger(clips_->cobj());
  }

  mps_placing_generator_.reset();

  delete pb_comm_;
  delete config_;
  delete clips_;
  delete logger_;
  delete clips_logger_;

  // Delete all global objects allocated by libprotobuf
  google::protobuf::ShutdownProtobufLibrary();
}


void
LLSFRefBox::setup_protobuf_comm()
{
  try {
    std::vector<std::string> proto_dirs;
    try {
      proto_dirs = config_->get_strings("/llsfrb/comm/protobuf-dirs");
      if (proto_dirs.size() > 0) {
	for (size_t i = 0; i < proto_dirs.size(); ++i) {
	  std::string::size_type pos;
	  if ((pos = proto_dirs[i].find("@BASEDIR@")) != std::string::npos) {
	    proto_dirs[i].replace(pos, 9, BASEDIR);
	  }
	  if ((pos = proto_dirs[i].find("@RESDIR@")) != std::string::npos) {
	    proto_dirs[i].replace(pos, 8, RESDIR);
	  }
	  if ((pos = proto_dirs[i].find("@CONFDIR@")) != std::string::npos) {
	    proto_dirs[i].replace(pos, 9, CONFDIR);
	  }
	  if ((pos = proto_dirs[i].find("@SHAREDIR@")) != std::string::npos) {
	    proto_dirs[i].replace(pos, 10, SHAREDIR);
	  }
	
	  if (proto_dirs[i][proto_dirs.size()-1] != '/') {
	    proto_dirs[i] += "/";
	  }
	  //logger_->log_warn("RefBox", "DIR: %s", proto_dirs[i].c_str());
	}
      }
    } catch (fawkes::Exception &e) {} // ignore, use default

    if (proto_dirs.empty()) {
      pb_comm_ = new ClipsProtobufCommunicator(clips_, clips_mutex_);
    } else {
      pb_comm_ = new ClipsProtobufCommunicator(clips_, clips_mutex_, proto_dirs);
    }

    pb_comm_->enable_server(config_->get_uint("/llsfrb/comm/server-port"));

    MessageRegister &mr_server = pb_comm_->message_register();
    if (! mr_server.load_failures().empty()) {
      MessageRegister::LoadFailMap::const_iterator e = mr_server.load_failures().begin();
      std::string errstr = e->first + " (" + e->second + ")";
      for (++e; e != mr_server.load_failures().end(); ++e) {
	errstr += std::string(", ") + e->first + " (" + e->second + ")";
      }
      logger_->log_warn("RefBox", "Failed to load some message types: %s", errstr.c_str());
    }

  } catch (std::runtime_error &e) {
    delete config_;
    delete pb_comm_;
    throw;
  }
}

void
LLSFRefBox::setup_clips()
{
  fawkes::MutexLocker lock(&clips_mutex_);

  logger_->log_info("RefBox", "Creating CLIPS environment");
  MultiLogger *mlogger = new MultiLogger();
  mlogger->add_logger(new ConsoleLogger(log_level_));
  try {
    std::string logfile = config_->get_string("/llsfrb/log/clips");
    mlogger->add_logger(new FileLogger(logfile.c_str(), Logger::LL_DEBUG));
  } catch (fawkes::Exception &e) {} // ignored, use default

  clips_logger_ = mlogger;

  bool simulation = false;
  try {
    simulation = config_->get_bool("/llsfrb/simulation/enabled");
  } catch (Exception &e) {} // ignore, use default

  init_clips_logger(clips_->cobj(), logger_, clips_logger_);

  std::string defglobal_ver =
    boost::str(boost::format("(defglobal\n"
			     "  ?*VERSION-MAJOR* = %u\n"
			     "  ?*VERSION-MINOR* = %u\n"
			     "  ?*VERSION-MICRO* = %u\n"
			     ")")
	       % FAWKES_VERSION_MAJOR
	       % FAWKES_VERSION_MINOR
	       % FAWKES_VERSION_MICRO);

  clips_->build(defglobal_ver);

  clips_->add_function("get-clips-dirs", sigc::slot<CLIPS::Values>(sigc::mem_fun(*this, &LLSFRefBox::clips_get_clips_dirs)));
  clips_->add_function("now", sigc::slot<CLIPS::Values>(sigc::mem_fun(*this, &LLSFRefBox::clips_now)));
  clips_->add_function("load-config", sigc::slot<void, std::string>(sigc::mem_fun(*this, &LLSFRefBox::clips_load_config)));
  clips_->add_function("config-path-exists", sigc::slot<CLIPS::Value, std::string>(sigc::mem_fun(*this, &LLSFRefBox::clips_config_path_exists)));
  clips_->add_function("config-get-bool", sigc::slot<CLIPS::Value, std::string>(sigc::mem_fun(*this, &LLSFRefBox::clips_config_get_bool)));

  if (mps_ && ! simulation) {
		clips_->add_function("mps-move-conveyor",
		                     sigc::slot<void, std::string, std::string, std::string>(
		                       sigc::mem_fun(*this, &LLSFRefBox::clips_mps_move_conveyor)));
		clips_->add_function("mps-cs-retrieve-cap",
		                     sigc::slot<void, std::string>(
		                       sigc::mem_fun(*this, &LLSFRefBox::clips_mps_cs_retrieve_cap)));
		clips_->add_function("mps-cs-mount-cap",
		                     sigc::slot<void, std::string>(
		                       sigc::mem_fun(*this, &LLSFRefBox::clips_mps_cs_mount_cap)));
		clips_->add_function("mps-bs-dispense",
		                     sigc::slot<void, std::string, std::string>(
		                       sigc::mem_fun(*this, &LLSFRefBox::clips_mps_bs_dispense)));

		clips_->add_function("mps-set-light", sigc::slot<void, std::string, std::string, std::string>(sigc::mem_fun(*this, &LLSFRefBox::clips_mps_set_light)));
    clips_->add_function("mps-set-lights", sigc::slot<void, std::string, std::string, std::string, std::string>(sigc::mem_fun(*this, &LLSFRefBox::clips_mps_set_lights)));
    clips_->add_function("mps-reset-lights", sigc::slot<void, std::string>(sigc::mem_fun(*this, &LLSFRefBox::clips_mps_reset_lights)));
    clips_->add_function("mps-ds-process", sigc::slot<void, std::string, int>(sigc::mem_fun(*this, &LLSFRefBox::clips_mps_ds_process)));
    clips_->add_function("mps-rs-mount-ring", sigc::slot<void, std::string, int>(sigc::mem_fun(*this, &LLSFRefBox::clips_mps_rs_mount_ring)));
    clips_->add_function("mps-cs-process", sigc::slot<void, std::string, std::string>(sigc::mem_fun(*this, &LLSFRefBox::clips_mps_cs_process)));
    clips_->add_function("mps-reset", sigc::slot<void, std::string>(sigc::mem_fun(*this, &LLSFRefBox::clips_mps_reset)));
    clips_->add_function("mps-reset-base-counter", sigc::slot<void, std::string>(sigc::mem_fun(*this, &LLSFRefBox::clips_mps_reset_base_counter)));
    clips_->add_function("mps-deliver", sigc::slot<void, std::string>(sigc::mem_fun(*this, &LLSFRefBox::clips_mps_deliver)));

		for (auto &mps : mps_->mpses_) {
			mps.second->addCallback(
			  [this, mps](OpcUtils::ReturnValue *ret) {
				  std::string ready;
				  if (ret->bool_s) {
					  ready = "TRUE";
				  } else {
					  ready = "FALSE";
				  }
				  fawkes::MutexLocker clips_lock(&clips_mutex_);
				  clips_->assert_fact_f("(mps-status-feedback %s READY %s)",
				                        mps.first.c_str(),
				                        ready.c_str());
			  },
			  OpcUtils::MPSRegister::STATUS_READY_IN,
			  nullptr);
			mps.second->addCallback(
			  [this, mps](OpcUtils::ReturnValue *ret) {
				  std::string busy;
				  if (ret->bool_s) {
					  busy = "TRUE";
				  } else {
					  busy = "FALSE";
				  }
				  fawkes::MutexLocker clips_lock(&clips_mutex_);
				  clips_->assert_fact_f("(mps-status-feedback %s BUSY %s)",
				                        mps.first.c_str(),
				                        busy.c_str());
			  },
			  OpcUtils::MPSRegister::STATUS_BUSY_IN,
			  nullptr);
			mps.second->addCallback(
			  [this, mps](OpcUtils::ReturnValue *ret) {
				  fawkes::MutexLocker clips_lock(&clips_mutex_);
				  clips_->assert_fact_f("(mps-status-feedback %s BARCODE %i)",
				                        mps.first.c_str(),
				                        ret->int32_s);
			  },
			  OpcUtils::MPSRegister::BARCODE_IN);
			// TODO proper MPS type check
			if (mps.first == "C-RS1" || mps.first == "C-RS2" || mps.first == "M-RS1"
			    || mps.first == "M-RS2") {
				mps.second->addCallback(
				  [this, mps](OpcUtils::ReturnValue *ret) {
					  fawkes::MutexLocker clips_lock(&clips_mutex_);
					  clips_->assert_fact_f("(mps-status-feedback %s SLIDE-COUNTER %u)",
					                        mps.first.c_str(),
					                        // TODO right type?
					                        ret->uint16_s);
				  },
				  OpcUtils::MPSRegister::SLIDECOUNT_IN);
			}
		}
	}

  clips_->signal_periodic().connect(sigc::mem_fun(*this, &LLSFRefBox::handle_clips_periodic));

}

void
LLSFRefBox::start_clips()
{
  fawkes::MutexLocker lock(&clips_mutex_);

  if (!clips_->batch_evaluate(cfg_clips_dir_ + "init.clp")) {
    logger_->log_warn("RefBox", "Failed to initialize CLIPS environment, batch file failed.");
    throw fawkes::Exception("Failed to initialize CLIPS environment, batch file failed.");
  }  

  clips_->assert_fact("(init)");
  clips_->refresh_agenda();
  clips_->run();
}

void
LLSFRefBox::handle_clips_periodic()
{
  std::queue<int> to_erase;
  std::map<long int, CLIPS::Fact::pointer>::iterator f;

  for (f = clips_msg_facts_.begin(); f != clips_msg_facts_.end(); ++f) {
    if (f->second->refcount() == 1) {
      //logger_->log_info("RefBox", "Fact %li can be erased", f->second->index());
      to_erase.push(f->first);
    }
  }
  while (! to_erase.empty()) {
    long int index = to_erase.front();
    CLIPS::Fact::pointer &f = clips_msg_facts_[index];
    CLIPS::Value v = f->slot_value("ptr")[0];
    void *ptr = v.as_address();
    delete static_cast<std::shared_ptr<google::protobuf::Message> *>(ptr);
    clips_msg_facts_.erase(index);
    to_erase.pop();
  }
}


CLIPS::Values
LLSFRefBox::clips_now()
{
  CLIPS::Values rv;
  struct timeval tv;
  gettimeofday(&tv, 0);
  rv.push_back(tv.tv_sec);
  rv.push_back(tv.tv_usec);
  return rv;
}


CLIPS::Values
LLSFRefBox::clips_get_clips_dirs()
{
  CLIPS::Values rv;
  rv.push_back(cfg_clips_dir_);
  return rv;
}

void
LLSFRefBox::clips_load_config(std::string cfg_prefix)
{
  std::shared_ptr<Configuration::ValueIterator> v(config_->search(cfg_prefix.c_str()));
  while (v->next()) {
    std::string type = "";
    std::string value = v->get_as_string();

    if      (v->is_uint())   type = "UINT";
    else if (v->is_int())    type = "INT";
    else if (v->is_float())  type = "FLOAT";
    else if (v->is_bool())   type = "BOOL";
    else if (v->is_string()) {
      type = "STRING";
      if (! v->is_list()) {
	value = std::string("\"") + value + "\"";
      }
    } else {
      logger_->log_warn("RefBox", "Config value at '%s' of unknown type '%s'",
	     v->path(), v->type());
    }

    if (v->is_list()) {
      //logger_->log_info("RefBox", "(confval (path \"%s\") (type %s) (is-list TRUE) (list-value %s))",
      //       v->path(), type.c_str(), value.c_str());
      clips_->assert_fact_f("(confval (path \"%s\") (type %s) (is-list TRUE) (list-value %s))",
			    v->path(), type.c_str(), value.c_str());
    } else {
      //logger_->log_info("RefBox", "(confval (path \"%s\") (type %s) (value %s))",
      //       v->path(), type.c_str(), value.c_str());
      clips_->assert_fact_f("(confval (path \"%s\") (type %s) (value %s))",
			    v->path(), type.c_str(), value.c_str());
    }
  }
}

CLIPS::Value
LLSFRefBox::clips_config_path_exists(std::string path)
{
  return CLIPS::Value(config_->exists(path.c_str()) ? "TRUE" : "FALSE", CLIPS::TYPE_SYMBOL);
}

CLIPS::Value
LLSFRefBox::clips_config_get_bool(std::string path)
{
  try {
    bool v = config_->get_bool(path.c_str());
    return CLIPS::Value(v ? "TRUE" : "FALSE", CLIPS::TYPE_SYMBOL);
  } catch (Exception &e) {
    return CLIPS::Value("FALSE", CLIPS::TYPE_SYMBOL);
  }
}

bool
LLSFRefBox::mutex_future_ready(const std::string &name)
{
	auto mf_it = mutex_futures_.find(name);
	if (mf_it != mutex_futures_.end()) {
		auto fut_status = mutex_futures_[name].wait_for(std::chrono::milliseconds(0));
		if (fut_status != std::future_status::ready) {
			return false;
		} else {
			mutex_futures_.erase(mf_it);
		}
	}
	return true;
}

void
LLSFRefBox::clips_mps_reset(std::string machine)
{
  logger_->log_info("MPS", "Resetting machine %s", machine.c_str());

  if (! mps_)  return;
  Machine *station;
  station = mps_->get_station(machine, station);
  if (station) {
    if (!mutex_future_ready(machine)) { return; }
		auto fut = std::async(std::launch::async, [station, machine] {
			station->reset();
			return true;
		});

		mutex_futures_[machine] = std::move(fut);
	} else {
    logger_->log_error("MPS", "Invalid station %s", machine.c_str());
    return;
  }
}

void
LLSFRefBox::clips_mps_reset_base_counter(std::string machine)
{
  // TODO implement
  logger_->log_info("MPS", "Resetting machine %s", machine.c_str());
  return;
}


void
LLSFRefBox::clips_mps_deliver(std::string machine)
{
  logger_->log_info("MPS", "Delivering on %s", machine.c_str());

  if (! mps_)  return;
  Machine *station;
  station = mps_->get_station(machine, station);
  if (station) {
		if (!mutex_future_ready(machine)) { return; }
		auto fut = std::async(std::launch::async, [this, station, machine] {
			station->conveyor_move(llsfrb::mps_comm::ConveyorDirection::FORWARD,
			                       llsfrb::mps_comm::MPSSensor::OUTPUT);
			MutexLocker lock(&clips_mutex_);
			clips_->assert_fact_f("(mps-feedback mps-deliver success %s)", machine.c_str());
			return true;
		});

    mutex_futures_[machine] = std::move(fut);
	} else {
    logger_->log_error("MPS", "Invalid station %s", machine.c_str());
    return;
  }
}

void
LLSFRefBox::clips_mps_bs_dispense(std::string machine, std::string color)
{
	logger_->log_info("MPS", "Dispense %s: %s", machine.c_str(), color.c_str());
	if (!mps_) {
    logger_->log_error("MPS", "MPS stations are not initialized");
    return;
  }
	BaseStation *station = mps_->get_station(machine, station);
	if (!station) {
		logger_->log_error("MPS", "Failed to access MPS %s", machine.c_str());
		return;
	}
	llsf_msgs::BaseColor color_id;
	if (color == "BASE_RED") {
		color_id = llsf_msgs::BaseColor::BASE_RED;
	} else if (color == "BASE_SILVER") {
		color_id = llsf_msgs::BaseColor::BASE_SILVER;
	} else if (color == "BASE_BLACK") {
		color_id = llsf_msgs::BaseColor::BASE_BLACK;
	} else {
		logger_->log_error("MPS", "Invalid color %s", color.c_str());
		return;
	}
	station->get_base(color_id);
}


void
LLSFRefBox::clips_mps_ds_process(std::string machine, int slide)
{
  logger_->log_info("MPS", "Processing on %s: slide %d",
		    machine.c_str(), slide);
	if (!mps_) {
    logger_->log_error("MPS", "MPS stations are not initialized");
    return;
  }
	DeliveryStation *station = mps_->get_station(machine, station);
	if (!station) {
		logger_->log_error("MPS", "Failed to access MPS %s", machine.c_str());
		return;
	}
  station->deliver_product(slide);
}

void
LLSFRefBox::clips_mps_rs_mount_ring(std::string machine, int slide)
{
	logger_->log_info("MPS", "Mount ring on %s: slide %d", machine.c_str(), slide);
	if (!mps_) {
		logger_->log_error("MPS", "MPS stations are not initialized");
		return;
	}
	RingStation *station = mps_->get_station(machine, station);
	if (!station) {
		logger_->log_error("MPS", "Failed to access MPS %s", machine.c_str());
		return;
	}
	station->mount_ring(slide);
}

void
LLSFRefBox::clips_mps_move_conveyor(std::string machine,
                                    std::string goal_position,
                                    std::string conveyor_direction /*= "FORWARD"*/)
{
	if (!mps_) {
    logger_->log_error("MPS", "MPS stations are not initialized");
    return;
  }
	Machine *station = mps_->get_station(machine, station);
	if (!station) {
		logger_->log_error("MPS", "Failed to access MPS %s", machine.c_str());
		return;
	}
	MPSSensor goal;
  if (goal_position == "INPUT") {
    goal = INPUT;
	} else if (goal_position == "MIDDLE") {
		goal = MIDDLE;
	} else if (goal_position == "OUTPUT") {
		goal = OUTPUT;
	} else {
    logger_->log_error("MPS", "Unknown conveyor position %s", goal_position.c_str());
    return;
  }
  ConveyorDirection direction;
  if (conveyor_direction == "FORWARD") {
    direction = FORWARD;
  } else if (conveyor_direction == "BACKWARD") {
    direction = BACKWARD;
  } else {
    logger_->log_error("MPS", "Unknown conveyor direction %s", conveyor_direction.c_str());
    return;
  }
  station->conveyor_move(direction, goal);
}

void
LLSFRefBox::clips_mps_cs_retrieve_cap(std::string machine)
{
	if (!mps_) {
    logger_->log_error("MPS", "MPS stations are not initialized");
    return;
  }
	CapStation *station = mps_->get_station(machine, station);
	if (!station) {
		logger_->log_error("MPS", "Failed to access MPS %s", machine.c_str());
		return;
	}
  station->retrieve_cap();
}

void
LLSFRefBox::clips_mps_cs_mount_cap(std::string machine)
{
	if (!mps_) {
		logger_->log_error("MPS", "MPS stations are not initialized");
		return;
	}
	CapStation *station = mps_->get_station(machine, station);
	if (!station) {
		logger_->log_error("MPS", "Failed to access MPS %s", machine.c_str());
		return;
	}
	station->mount_cap();
}


void LLSFRefBox::clips_mps_cs_process(std::string machine, std::string operation)
{
	logger_->log_info("MPS", "%s on %s", operation.c_str(), machine.c_str());
	if (operation != "RETRIEVE_CAP" && operation != "MOUNT_CAP") {
		logger_->log_error("MPS", "Invalid operation '%s' on %s", operation.c_str(), machine.c_str());
    return;
	}
	if (! mps_)  return;
  CapStation *station;
  station = mps_->get_station(machine, station);
  if (station) {
		if (!mutex_future_ready(machine)) { return; }
		auto fut = std::async(std::launch::async, [this, station, machine, operation] {
			MutexLocker lock(&clips_mutex_, false);
			station->band_on_until_mid();
			lock.relock();
			clips_->assert_fact_f("(mps-feedback %s %s AVAILABLE)", machine.c_str(), operation.c_str());
			lock.unlock();
			if (operation == "RETRIEVE_CAP") {
				station->retrieve_cap();
			} else if (operation == "MOUNT_CAP") {
				station->mount_cap();
			}
			station->band_on_until_out();
			lock.relock();
			clips_->assert_fact_f("(mps-feedback %s %s DONE)", machine.c_str(), operation.c_str());
			return true;
		});

		mutex_futures_[machine] = std::move(fut);
	} else {
    logger_->log_error("MPS", "Invalid station %s", machine.c_str());
    return;
  }
}


void
LLSFRefBox::clips_mps_set_light(std::string machine, std::string color, std::string state)
{
  //logger_->log_info("MPS", "Set light %s: %s to %s",
  //		    machine.c_str(), color.c_str(), state.c_str());

  if (! mps_)  return;
  Machine *station;
  station = mps_->get_station(machine, station);
  if (station) {
    llsf_msgs::LightColor color_id;
    if (color == "RED") {
      color_id = llsf_msgs::LightColor::RED;
    } else if (color == "YELLOW") {
      color_id = llsf_msgs::LightColor::YELLOW;
    } else if (color == "GREEN") {
      color_id = llsf_msgs::LightColor::GREEN;
    } else {
      logger_->log_error("MPS", "Invalid color %s", color.c_str());
      return;
    }

    llsf_msgs::LightState state_id;
    if (state == "ON") {
      state_id = llsf_msgs::LightState::ON;
    } else if (state == "BLINK") {
      state_id = llsf_msgs::LightState::BLINK;
    } else if (state == "OFF") {
      state_id = llsf_msgs::LightState::OFF;
    } else {
      logger_->log_error("MPS", "Invalid state %s", state.c_str());
      return;
    }

    //printf("Set light %i %i %i\n", color_id, state_id, blink_id);
    // TODO time?
    int time = 0;
    station->set_light(color_id, state_id, time);

  } else {
    //logger_->log_error("MPS", "Invalid station %s", machine.c_str());
    return;
  }
}

void
LLSFRefBox::clips_mps_set_lights(std::string machine, std::string red_state,
                                 std::string yellow_state, std::string green_state)
{
  clips_mps_set_light(machine, "RED", red_state);
  clips_mps_set_light(machine, "YELLOW", yellow_state);
  clips_mps_set_light(machine, "GREEN", green_state);
}

void
LLSFRefBox::clips_mps_reset_lights(std::string machine)
{
  if (! mps_) return;
	Machine *station = mps_->get_station(machine, station);
  if (station) {
    station->reset_light();
  }
}

#ifdef HAVE_MONGODB

/** Handle message that came from a client.
 * @param client client ID
 * @param component_id component the message was addressed to
 * @param msg_type type of the message
 * @param msg the message
 */
void
LLSFRefBox::handle_server_client_msg(ProtobufStreamServer::ClientID client,
				     uint16_t component_id, uint16_t msg_type,
				     std::shared_ptr<google::protobuf::Message> msg)
{
  mongo::BSONObjBuilder meta;
  meta.append("direction", "inbound");
  meta.append("via", "server");
  meta.append("component_id", component_id);
  meta.append("msg_type", msg_type);
  meta.append("client_id", client);
  mongo::BSONObj meta_obj(meta.obj());
  mongodb_protobuf_->write(*msg, meta_obj);
}

/** Handle message that came from a client.
 * @param client client ID
 * @param component_id component the message was addressed to
 * @param msg_type type of the message
 * @param msg the message
 */
void
LLSFRefBox::handle_peer_msg(boost::asio::ip::udp::endpoint &endpoint,
			    uint16_t component_id, uint16_t msg_type,
			    std::shared_ptr<google::protobuf::Message> msg)
{
  mongo::BSONObjBuilder meta;
  meta.append("direction", "inbound");
  meta.append("via", "peer");
  meta.append("endpoint-host", endpoint.address().to_string());
  meta.append("endpoint-port", endpoint.port());
  meta.append("component_id", component_id);
  meta.append("msg_type", msg_type);
  mongo::BSONObj meta_obj(meta.obj());
  mongodb_protobuf_->write(*msg, meta_obj);
}

/** Handle server reception failure
 * @param client client ID
 * @param component_id component the message was addressed to
 * @param msg_type type of the message
 * @param msg the message string
 */
void
LLSFRefBox::handle_server_client_fail(ProtobufStreamServer::ClientID client,
				      uint16_t component_id, uint16_t msg_type,
				      std::string msg)
{
}


void
LLSFRefBox::add_comp_type(google::protobuf::Message &m, mongo::BSONObjBuilder *b)
{
  const google::protobuf::Descriptor *desc = m.GetDescriptor();
  const google::protobuf::EnumDescriptor *enumdesc = desc->FindEnumTypeByName("CompType");
  if (! enumdesc) return;
  const google::protobuf::EnumValueDescriptor *compdesc =
    enumdesc->FindValueByName("COMP_ID");
  const google::protobuf::EnumValueDescriptor *msgtdesc =
    enumdesc->FindValueByName("MSG_TYPE");
  if (! compdesc || ! msgtdesc)  return;
  int comp_id = compdesc->number();
  int msg_type = msgtdesc->number();
  b->append("component_id", comp_id);
  b->append("msg_type", msg_type);
}

/** Handle message that was sent to a server client.
 * @param client client ID
 * @param msg the message
 */
void
LLSFRefBox::handle_server_sent_msg(ProtobufStreamServer::ClientID client,
				   std::shared_ptr<google::protobuf::Message> msg)
{
  mongo::BSONObjBuilder meta;
  meta.append("direction", "outbound");
  meta.append("via", "server");
  meta.append("client_id", client);
  add_comp_type(*msg, &meta);
  mongo::BSONObj meta_obj(meta.obj());
  mongodb_protobuf_->write(*msg, meta_obj);
}

/** Handle message that was sent with a client.
 * @param host host of the endpoint sent to
 * @param port port of the endpoint sent to
 * @param msg the message
 */
void
LLSFRefBox::handle_client_sent_msg(std::string host, unsigned short int port,
				   std::shared_ptr<google::protobuf::Message> msg)
{
  mongo::BSONObjBuilder meta;
  meta.append("direction", "outbound");
  meta.append("via", "client");
  meta.append("host", host);
  meta.append("port", port);
  add_comp_type(*msg, &meta);
  mongo::BSONObj meta_obj(meta.obj());
  mongodb_protobuf_->write(*msg, meta_obj);
}

/** Setup MongoDB related CLIPS functions. */
void
LLSFRefBox::setup_clips_mongodb()
{
  fawkes::MutexLocker lock(&clips_mutex_);

  clips_->add_function("bson-create", sigc::slot<CLIPS::Value>(sigc::mem_fun(*this, &LLSFRefBox::clips_bson_create)));
  clips_->add_function("bson-parse", sigc::slot<CLIPS::Value, std::string>(sigc::mem_fun(*this, &LLSFRefBox::clips_bson_parse)));
  clips_->add_function("bson-destroy", sigc::slot<void, void *>(sigc::mem_fun(*this, &LLSFRefBox::clips_bson_destroy)));
  clips_->add_function("bson-append", sigc::slot<void, void *, std::string, CLIPS::Value>(sigc::mem_fun(*this, &LLSFRefBox::clips_bson_append)));
  clips_->add_function("bson-append-array", sigc::slot<void, void *, std::string, CLIPS::Values>(sigc::mem_fun(*this, &LLSFRefBox::clips_bson_append_array)));
  clips_->add_function("bson-array-start", sigc::slot<CLIPS::Value, void *, std::string>(sigc::mem_fun(*this, &LLSFRefBox::clips_bson_array_start)));
  clips_->add_function("bson-array-finish", sigc::slot<void, void *>(sigc::mem_fun(*this, &LLSFRefBox::clips_bson_array_finish)));
  clips_->add_function("bson-array-append", sigc::slot<void, void *, CLIPS::Value>(sigc::mem_fun(*this, &LLSFRefBox::clips_bson_array_append)));

  clips_->add_function("bson-append-time", sigc::slot<void, void *, std::string, CLIPS::Values>(sigc::mem_fun(*this, &LLSFRefBox::clips_bson_append_time)));
  clips_->add_function("bson-tostring", sigc::slot<std::string, void *>(sigc::mem_fun(*this, &LLSFRefBox::clips_bson_tostring)));
  clips_->add_function("mongodb-insert", sigc::slot<void, std::string, void *>(sigc::mem_fun(*this, &LLSFRefBox::clips_mongodb_insert)));
  clips_->add_function("mongodb-upsert", sigc::slot<void, std::string, void *, CLIPS::Value>(sigc::mem_fun(*this, &LLSFRefBox::clips_mongodb_upsert)));
  clips_->add_function("mongodb-update", sigc::slot<void, std::string, void *, CLIPS::Value>(sigc::mem_fun(*this, &LLSFRefBox::clips_mongodb_update)));
  clips_->add_function("mongodb-replace", sigc::slot<void, std::string, void *, CLIPS::Value>(sigc::mem_fun(*this, &LLSFRefBox::clips_mongodb_replace)));
  clips_->add_function("mongodb-query", sigc::slot<CLIPS::Value, std::string, void *>(sigc::mem_fun(*this, &LLSFRefBox::clips_mongodb_query)));
  clips_->add_function("mongodb-query-sort", sigc::slot<CLIPS::Value, std::string, void *, void *>(sigc::mem_fun(*this, &LLSFRefBox::clips_mongodb_query_sort)));
  clips_->add_function("mongodb-cursor-destroy", sigc::slot<void, void *>(sigc::mem_fun(*this, &LLSFRefBox::clips_mongodb_cursor_destroy)));
  clips_->add_function("mongodb-cursor-more", sigc::slot<CLIPS::Value, void *>(sigc::mem_fun(*this, &LLSFRefBox::clips_mongodb_cursor_more)));
  clips_->add_function("mongodb-cursor-next", sigc::slot<CLIPS::Value, void *>(sigc::mem_fun(*this, &LLSFRefBox::clips_mongodb_cursor_next)));
  clips_->add_function("bson-field-names", sigc::slot<CLIPS::Values, void *>(sigc::mem_fun(*this, &LLSFRefBox::clips_bson_field_names)));
  clips_->add_function("bson-get", sigc::slot<CLIPS::Value, void *, std::string>(sigc::mem_fun(*this, &LLSFRefBox::clips_bson_get)));
  clips_->add_function("bson-get-array", sigc::slot<CLIPS::Values, void *, std::string>(sigc::mem_fun(*this, &LLSFRefBox::clips_bson_get_array)));
  clips_->add_function("bson-get-time", sigc::slot<CLIPS::Values, void *, std::string>(sigc::mem_fun(*this, &LLSFRefBox::clips_bson_get_time)));

  clips_->build("(deffacts have-feature-mongodb (have-feature MongoDB))");
}

/** Handle message that was sent to a server client.
 * @param client client ID
 * @param msg the message
 */
void
LLSFRefBox::handle_peer_sent_msg(std::shared_ptr<google::protobuf::Message> msg)
{
  mongo::BSONObjBuilder meta;
  meta.append("direction", "outbound");
  meta.append("via", "peer");
  add_comp_type(*msg, &meta);
  mongo::BSONObj meta_obj(meta.obj());
  mongodb_protobuf_->write(*msg, meta_obj);
}


CLIPS::Value
LLSFRefBox::clips_bson_create()
{
  mongo::BSONObjBuilder *b = new mongo::BSONObjBuilder();
  return CLIPS::Value(b);
}

CLIPS::Value
LLSFRefBox::clips_bson_parse(std::string document)
{
  mongo::BSONObjBuilder *b = new mongo::BSONObjBuilder();
  try {
    b->appendElements(mongo::fromjson(document));
#ifdef HAVE_MONGODB_VERSION_H
  } catch (mongo::MsgAssertionException &e) {
#else
  } catch (bson::assertion &e) {
#endif
    logger_->log_error("MongoDB", "Parsing JSON doc failed: %s\n%s",
		       e.what(), document.c_str());
  }
  return CLIPS::Value(b);
}

void
LLSFRefBox::clips_bson_destroy(void *bson)
{
  mongo::BSONObjBuilder *b = static_cast<mongo::BSONObjBuilder *>(bson);
  delete b;
}

std::string
LLSFRefBox::clips_bson_tostring(void *bson)
{
  mongo::BSONObjBuilder *b = static_cast<mongo::BSONObjBuilder *>(bson);
  return b->asTempObj().jsonString(mongo::Strict, true);
}

void
LLSFRefBox::clips_bson_append(void *bson, std::string field_name, CLIPS::Value value)
{
  try {
    mongo::BSONObjBuilder *b = static_cast<mongo::BSONObjBuilder *>(bson);
    switch (value.type()) {
    case CLIPS::TYPE_FLOAT:
      b->append(field_name, value.as_float());
      break;

    case CLIPS::TYPE_INTEGER:
      b->append(field_name, value.as_integer());
      break;

    case CLIPS::TYPE_SYMBOL:
    case CLIPS::TYPE_STRING:
    case CLIPS::TYPE_INSTANCE_NAME:
      b->append(field_name, value.as_string());
      break;

    case CLIPS::TYPE_EXTERNAL_ADDRESS:
      {
	mongo::BSONObjBuilder *subb = static_cast<mongo::BSONObjBuilder *>(value.as_address());
	b->append(field_name, subb->asTempObj());
      }
      break;

    default:
      logger_->log_warn("RefBox", "Tried to add unknown type to BSON field %s",
			field_name.c_str());
      break;
    }
#ifdef HAVE_MONGODB_VERSION_H
  } catch (mongo::MsgAssertionException &e) {
#else
  } catch (bson::assertion &e) {
#endif
    logger_->log_error("MongoDB", "Failed to append array value to field %s: %s",
		       field_name.c_str(), e.what());
  }
}


void
LLSFRefBox::clips_bson_append_array(void *bson,
				    std::string field_name, CLIPS::Values values)
{
  try {
    mongo::BSONObjBuilder *b = static_cast<mongo::BSONObjBuilder *>(bson);
    mongo::BSONArrayBuilder ab(b->subarrayStart(field_name));

    for (auto value : values) {
      switch (value.type()) {
      case CLIPS::TYPE_FLOAT:
	ab.append(value.as_float());
	break;

      case CLIPS::TYPE_INTEGER:
	ab.append(value.as_integer());
	break;
      
      case CLIPS::TYPE_SYMBOL:
      case CLIPS::TYPE_STRING:
      case CLIPS::TYPE_INSTANCE_NAME:
	ab.append(value.as_string());
	break;

      case CLIPS::TYPE_EXTERNAL_ADDRESS:
	{
	  mongo::BSONObjBuilder *subb =
	    static_cast<mongo::BSONObjBuilder *>(value.as_address());
	  ab.append(subb->asTempObj());
	}
	break;
      
      default:
	logger_->log_warn("MongoDB", "Tried to add unknown type to BSON array field %s",
			  field_name.c_str());
	break;
      }
    }
#ifdef HAVE_MONGODB_VERSION_H
  } catch (mongo::MsgAssertionException &e) {
#else
  } catch (bson::assertion &e) {
#endif
    logger_->log_error("MongoDB", "Failed to append array value to field %s: %s",
		       field_name.c_str(), e.what());
  }
}

CLIPS::Value
LLSFRefBox::clips_bson_array_start(void *bson, std::string field_name)
{
  mongo::BSONObjBuilder *b = static_cast<mongo::BSONObjBuilder *>(bson);
  mongo::BufBuilder &bb = b->subarrayStart(field_name);
  mongo::BSONArrayBuilder *arrb = new mongo::BSONArrayBuilder(bb);
  return CLIPS::Value(arrb);
}


void
LLSFRefBox::clips_bson_array_finish(void *barr)
{
  mongo::BSONArrayBuilder *ab = static_cast<mongo::BSONArrayBuilder *>(barr);
  delete ab;
}

void
LLSFRefBox::clips_bson_array_append(void *barr, CLIPS::Value value)
{
  try {
    mongo::BSONArrayBuilder *ab = static_cast<mongo::BSONArrayBuilder *>(barr);
    switch (value.type()) {
    case CLIPS::TYPE_FLOAT:
      ab->append(value.as_float());
      break;

    case CLIPS::TYPE_INTEGER:
      ab->append(value.as_integer());
      break;

    case CLIPS::TYPE_SYMBOL:
    case CLIPS::TYPE_STRING:
    case CLIPS::TYPE_INSTANCE_NAME:
      ab->append(value.as_string());
      break;

    case CLIPS::TYPE_EXTERNAL_ADDRESS:
      {
	mongo::BSONObjBuilder *subb = static_cast<mongo::BSONObjBuilder *>(value.as_address());
	ab->append(subb->asTempObj());
      }
      break;

    default:
      logger_->log_warn("RefBox", "Tried to add unknown type to BSON array");
      break;
    }
#ifdef HAVE_MONGODB_VERSION_H
  } catch (mongo::MsgAssertionException &e) {
#else
  } catch (bson::assertion &e) {
#endif
    logger_->log_error("MongoDB", "Failed to append to array: %s", e.what());
  }
}


void
LLSFRefBox::clips_bson_append_time(void *bson, std::string field_name, CLIPS::Values time)
{
  if (time.size() != 2) {
    logger_->log_warn("MongoDB", "Invalid time, %zu instead of 2 entries", time.size());
    return;
  }
  if (time[0].type() != CLIPS::TYPE_INTEGER || time[1].type() != CLIPS::TYPE_INTEGER) {
    logger_->log_warn("MongoDB", "Invalid time, type mismatch");
    return;
  }

  try {
    mongo::BSONObjBuilder *b = static_cast<mongo::BSONObjBuilder *>(bson);
    struct timeval now = { time[0].as_integer(), time[1].as_integer()};
    mongo::Date_t nowd = now.tv_sec * 1000 + now.tv_usec / 1000;
    b->appendDate(field_name, nowd);
#ifdef HAVE_MONGODB_VERSION_H
  } catch (mongo::MsgAssertionException &e) {
#else
  } catch (bson::assertion &e) {
#endif
    logger_->log_error("MongoDB", "Failed to append time value to field %s: %s",
		       field_name.c_str(), e.what());
  }
}

void
LLSFRefBox::clips_mongodb_insert(std::string collection, void *bson)
{
  if (! cfg_mongodb_enabled_) {
    logger_->log_warn("MongoDB", "Insert requested while MongoDB disabled");
    return;
  }

  mongo::BSONObjBuilder *b = static_cast<mongo::BSONObjBuilder *>(bson);

  try {
    mongodb_->insert(collection, b->asTempObj());
  } catch (mongo::DBException &e) {
    logger_->log_warn("MongoDB", "Insert failed: %s", e.what());
  }
}


void
LLSFRefBox::mongodb_update(std::string &collection, mongo::BSONObj obj,
			   CLIPS::Value &query, bool upsert)
{
  if (! cfg_mongodb_enabled_) {
    logger_->log_warn("MongoDB", "Update requested while MongoDB disabled");
    return;
  }

  try {
    mongo::BSONObj query_obj;
    if (query.type() == CLIPS::TYPE_STRING) {
      query_obj = mongo::fromjson(query.as_string());
    } else if (query.type() == CLIPS::TYPE_EXTERNAL_ADDRESS) {
      mongo::BSONObjBuilder *qb = static_cast<mongo::BSONObjBuilder *>(query.as_address());
      query_obj = qb->asTempObj();
    } else {
      logger_->log_warn("MongoDB", "Invalid query, must be string or BSON document");
      return;
    }

    mongodb_->update(collection, query_obj, obj, upsert);
#ifdef HAVE_MONGODB_VERSION_H
  } catch (mongo::MsgAssertionException &e) {
#else
  } catch (bson::assertion &e) {
#endif
    logger_->log_warn("MongoDB", "Compiling query failed: %s", e.what());
  } catch (mongo::DBException &e) {
    logger_->log_warn("MongoDB", "Insert failed: %s", e.what());
  }
}


void
LLSFRefBox::clips_mongodb_upsert(std::string collection, void *bson, CLIPS::Value query)
{
  mongo::BSONObjBuilder *b = static_cast<mongo::BSONObjBuilder *>(bson);
  if (! b) {
	  logger_->log_warn("MongoDB", "Invalid BSON Obj Builder passed");
	  return;
  }
  mongodb_update(collection, b->asTempObj(), query, true);
}

void
LLSFRefBox::clips_mongodb_update(std::string collection, void *bson, CLIPS::Value query)
{
  mongo::BSONObjBuilder *b = static_cast<mongo::BSONObjBuilder *>(bson);
  if (! b) {
	  logger_->log_warn("MongoDB", "Invalid BSON Obj Builder passed");
	  return;
  }

  mongo::BSONObjBuilder update_doc;
  update_doc.append("$set", b->asTempObj());

  mongodb_update(collection, update_doc.obj(), query, false);
}

void
LLSFRefBox::clips_mongodb_replace(std::string collection, void *bson, CLIPS::Value query)
{
  mongo::BSONObjBuilder *b = static_cast<mongo::BSONObjBuilder *>(bson);
  if (! b) logger_->log_warn("MongoDB", "Invalid BSON Obj Builder passed");
  mongodb_update(collection, b->asTempObj(), query, false);
}

CLIPS::Value
LLSFRefBox::clips_mongodb_query_sort(std::string collection, void *bson, void *bson_sort)
{
  if (! cfg_mongodb_enabled_) {
    logger_->log_warn("MongoDB", "Query requested while MongoDB disabled");
    return CLIPS::Value("FALSE", CLIPS::TYPE_SYMBOL);
  }

  mongo::BSONObjBuilder *b = static_cast<mongo::BSONObjBuilder *>(bson);

  try {
	  mongo::Query q(b->asTempObj());
	  if (bson_sort) {
		  mongo::BSONObjBuilder *bs = static_cast<mongo::BSONObjBuilder *>(bson_sort);
		  q.sort(bs->asTempObj());
	  }

#if __cplusplus >= 201103L
 	  std::unique_ptr<mongo::DBClientCursor> c = mongodb_->query(collection, q);

	  return CLIPS::Value(new std::unique_ptr<mongo::DBClientCursor>(std::move(c)),
	                      CLIPS::TYPE_EXTERNAL_ADDRESS);
#else
 	  std::auto_ptr<mongo::DBClientCursor> c = mongodb_->query(collection, q);

	  return CLIPS::Value(new std::auto_ptr<mongo::DBClientCursor>(c),
	                      CLIPS::TYPE_EXTERNAL_ADDRESS);
#endif

  } catch (mongo::DBException &e) {
    logger_->log_warn("MongoDB", "Query failed: %s", e.what());
    return CLIPS::Value("FALSE", CLIPS::TYPE_SYMBOL);
  }
}

CLIPS::Value
LLSFRefBox::clips_mongodb_query(std::string collection, void *bson)
{
	return clips_mongodb_query_sort(collection, bson, NULL);
}

void
LLSFRefBox::clips_mongodb_cursor_destroy(void *cursor)
{
#if __cplusplus >= 201103L
	std::unique_ptr<mongo::DBClientCursor> *c =
		static_cast<std::unique_ptr<mongo::DBClientCursor> *>(cursor);
#else
	std::auto_ptr<mongo::DBClientCursor> *c =
		static_cast<std::auto_ptr<mongo::DBClientCursor> *>(cursor);
#endif

	if (! c || ! c->get()) {
		logger_->log_error("MongoDB", "mongodb-cursor-destroy: got invalid cursor");
		return;
	}

	delete c;
}

CLIPS::Value
LLSFRefBox::clips_mongodb_cursor_more(void *cursor)
{
#if __cplusplus >= 201103L
	std::unique_ptr<mongo::DBClientCursor> *c =
		static_cast<std::unique_ptr<mongo::DBClientCursor> *>(cursor);
#else
	std::auto_ptr<mongo::DBClientCursor> *c =
		static_cast<std::auto_ptr<mongo::DBClientCursor> *>(cursor);
#endif

	if (! c || ! c->get()) {
		logger_->log_error("MongoDB", "mongodb-cursor-more: got invalid cursor");
		return CLIPS::Value("FALSE", CLIPS::TYPE_SYMBOL);
	}

	return CLIPS::Value((*c)->more() ? "TRUE" : "FALSE", CLIPS::TYPE_SYMBOL);
}

CLIPS::Value
LLSFRefBox::clips_mongodb_cursor_next(void *cursor)
{
#if __cplusplus >= 201103L
	std::unique_ptr<mongo::DBClientCursor> *c =
		static_cast<std::unique_ptr<mongo::DBClientCursor> *>(cursor);
#else
	std::auto_ptr<mongo::DBClientCursor> *c =
		static_cast<std::auto_ptr<mongo::DBClientCursor> *>(cursor);
#endif

	if (! c || ! c->get()) {
		logger_->log_error("MongoDB", "mongodb-cursor-next: got invalid cursor");
		return CLIPS::Value("FALSE", CLIPS::TYPE_SYMBOL);
	}

  mongo::BSONObjBuilder *b = new mongo::BSONObjBuilder();
  b->appendElements((*c)->next());
  return CLIPS::Value(b);
}


CLIPS::Values
LLSFRefBox::clips_bson_field_names(void *bson)
{
  mongo::BSONObjBuilder *b = static_cast<mongo::BSONObjBuilder *>(bson);

	if (! b) {
		logger_->log_error("MongoDB", "mongodb-bson-field-names: invalid object");
		CLIPS::Values rv;
		rv.push_back(CLIPS::Value("FALSE", CLIPS::TYPE_SYMBOL));
		return rv;
	}

	std::set<std::string> field_names;
	b->asTempObj().getFieldNames(field_names);

	CLIPS::Values rv;
	for (const std::string &n : field_names) {
		rv.push_back(CLIPS::Value(n));
	}
	return rv;
}


CLIPS::Value
LLSFRefBox::clips_bson_get(void *bson, std::string field_name)
{
	mongo::BSONObjBuilder *b = static_cast<mongo::BSONObjBuilder *>(bson);

	if (! b) {
		logger_->log_error("MongoDB", "mongodb-bson-get: invalid object");
		return CLIPS::Value("FALSE", CLIPS::TYPE_SYMBOL);
	}

	mongo::BSONObj o(b->asTempObj());

	if (! o.hasField(field_name)) {
		logger_->log_error("MongoDB", "mongodb-bson-get: has no field %s",
		                   field_name.c_str());
		return CLIPS::Value("FALSE", CLIPS::TYPE_SYMBOL);
	}

	mongo::BSONElement el = o.getField(field_name);

	switch (el.type()) {
	case mongo::NumberDouble:
		return CLIPS::Value(el.Double());
	case mongo::String:
		return CLIPS::Value(el.String());
	case mongo::Bool:
		return CLIPS::Value(el.Bool() ? "TRUE" : "FALSE", CLIPS::TYPE_SYMBOL);
	case mongo::NumberInt:
		return CLIPS::Value(el.Int());
	case mongo::NumberLong:
		return CLIPS::Value(el.Long());
	case mongo::Object:
		{
			mongo::BSONObjBuilder *b = new mongo::BSONObjBuilder();
			b->appendElements(el.Obj());
			return CLIPS::Value(b);
		}
	default:
		return CLIPS::Value("INVALID_VALUE_TYPE", CLIPS::TYPE_SYMBOL);
	}
}


CLIPS::Values
LLSFRefBox::clips_bson_get_array(void *bson, std::string field_name)
{
	mongo::BSONObjBuilder *b = static_cast<mongo::BSONObjBuilder *>(bson);

	CLIPS::Values rv;

	if (! b) {
		logger_->log_error("MongoDB", "mongodb-bson-get-array: invalid object");
		rv.push_back(CLIPS::Value("FALSE", CLIPS::TYPE_SYMBOL));
		return rv;
	}

	mongo::BSONObj o(b->asTempObj());

	if (! o.hasField(field_name)) {
		logger_->log_error("MongoDB", "mongodb-bson-get-array: has no field %s",
		                   field_name.c_str());
		rv.push_back(CLIPS::Value("FALSE", CLIPS::TYPE_SYMBOL));
		return rv;
	}

	mongo::BSONElement el = o.getField(field_name);

	if (el.type() != mongo::Array) {
		logger_->log_error("MongoDB", "mongodb-bson-get-array: field %s is not an array",
		                   field_name.c_str());
		rv.push_back(CLIPS::Value("FALSE", CLIPS::TYPE_SYMBOL));
		return rv;
	}

	std::vector<mongo::BSONElement> elements(el.Array());

	for (const mongo::BSONElement &e : elements) {
		switch (e.type()) {
		case mongo::NumberDouble:
			rv.push_back(CLIPS::Value(e.Double())); break;
		case mongo::String:
			rv.push_back(CLIPS::Value(e.String())); break;
		case mongo::Bool:
			rv.push_back(CLIPS::Value(e.Bool() ? "TRUE" : "FALSE", CLIPS::TYPE_SYMBOL));
			break;
		case mongo::NumberInt:
			rv.push_back(CLIPS::Value(e.Int())); break;
		case mongo::NumberLong:
			rv.push_back(CLIPS::Value(e.Long())); break;
		case mongo::Object:
			{
				mongo::BSONObjBuilder *b = new mongo::BSONObjBuilder();
				b->appendElements(e.Obj());
				rv.push_back(CLIPS::Value(b));
			}
			break;
		default:
			rv.clear();
			rv.push_back(CLIPS::Value("FALSE", CLIPS::TYPE_SYMBOL));
			return rv;
		}
	}
	return rv;
	
}


CLIPS::Values
LLSFRefBox::clips_bson_get_time(void *bson, std::string field_name)
{
	mongo::BSONObjBuilder *b = static_cast<mongo::BSONObjBuilder *>(bson);

	CLIPS::Values rv;

	if (! b) {
		logger_->log_error("MongoDB", "mongodb-bson-get-time: invalid object");
		rv.push_back(CLIPS::Value("FALSE", CLIPS::TYPE_SYMBOL));
		return rv;
	}

	mongo::BSONObj o(b->asTempObj());

	if (! o.hasField(field_name)) {
		logger_->log_error("MongoDB", "mongodb-bson-get-time: has no field %s",
		                   field_name.c_str());
		rv.push_back(CLIPS::Value("FALSE", CLIPS::TYPE_SYMBOL));
		return rv;
	}

	mongo::BSONElement el = o.getField(field_name);

	int64_t ts = 0;
	if (el.type() == mongo::Date) {
		mongo::Date_t d = el.Date();
		ts = d.asInt64();
	} else if (el.type() == mongo::Timestamp) {
#ifdef HAVE_MONGODB_VERSION_H
		mongo::Timestamp_t t = el.Timestamp();
		ts = t.seconds();
#else
		mongo::Date_t d = el.timestampTime();
		ts = d.asInt64();
#endif
	} else {
		logger_->log_error("MongoDB", "mongodb-bson-get-time: field %s is not a time",
		                   field_name.c_str());
		rv.push_back(CLIPS::Value("FALSE", CLIPS::TYPE_SYMBOL));
		return rv;
	}


	rv.resize(2);
	rv[0] = CLIPS::Value((long long int)(ts / 1000));
	rv[1] = CLIPS::Value((ts - (rv[0].as_integer() * 1000)) * 1000);
	return rv;
}

#endif


/** Start the timer for another run. */
void
LLSFRefBox::start_timer()
{
  timer_last_ = boost::posix_time::microsec_clock::local_time();
  timer_.expires_from_now(boost::posix_time::milliseconds(cfg_timer_interval_));
  timer_.async_wait(boost::bind(&LLSFRefBox::handle_timer, this,
				boost::asio::placeholders::error));
}

/** Handle timer event.
 * @param error error code
 */
void
LLSFRefBox::handle_timer(const boost::system::error_code& error)
{
  if (! error) {
    /*
    boost::posix_time::ptime now = boost::posix_time::microsec_clock::local_time();
    long ms = (now - timer_last_).total_milliseconds();
    timer_last_ = now;
    */

    //sps_read_rfids();
    if (mps_)  mps_->process();

    {
      //std::lock_guard<std::recursive_mutex> lock(clips_mutex_);
      fawkes::MutexLocker lock(&clips_mutex_);

      if (mps_) {
	std::map<std::string, std::string> machine_states = mps_->get_states();
	for (const auto &ms : machine_states) {
	  //printf("Asserting (machine-mps-state (name %s) (state %s) (num-bases %u))\n",
	  //       ms.first.c_str(), ms.second.c_str(), 0);
          std::string type = ms.first.substr(2, 2);
          unsigned int num_bases = 0;
          // TODO get RS base count
          //if (type == "RS") {
          //  RingStation *station;
          //  station = mps_->get_station(ms.first, station);
          //  if (station)  num_bases = station->getCountSlide();
          //}
	  clips_->assert_fact_f("(machine-mps-state (name %s) (state %s) (num-bases %u))",
				ms.first.c_str(), ms.second.c_str(), num_bases);
	}
      }

      clips_->assert_fact("(time (now))");
      clips_->refresh_agenda();
      clips_->run();
    }

    timer_.expires_at(timer_.expires_at()
		      + boost::posix_time::milliseconds(cfg_timer_interval_));
    timer_.async_wait(boost::bind(&LLSFRefBox::handle_timer, this,
				  boost::asio::placeholders::error));
  }
}


/** Handle operating system signal.
 * @param error error code
 * @param signum signal number
 */
void
LLSFRefBox::handle_signal(const boost::system::error_code& error, int signum)
{
  timer_.cancel();
  io_service_.stop();
}



/** Run the application.
 * @return return code, 0 if no error, error code otherwise
 */
int
LLSFRefBox::run()
{
#if BOOST_ASIO_VERSION >= 100601
  // Construct a signal set registered for process termination.
  boost::asio::signal_set signals(io_service_, SIGINT, SIGTERM);

  // Start an asynchronous wait for one of the signals to occur.
  signals.async_wait(boost::bind(&LLSFRefBox::handle_signal, this,
				 boost::asio::placeholders::error,
				 boost::asio::placeholders::signal_number));
#else
  g_refbox = this;
  signal(SIGINT, llsfrb::handle_signal);
#endif

  start_timer();
  io_service_.run();
  return 0;
}

} // end of namespace llsfrb
