
/***************************************************************************
 *  sps_comm.h - LLSF RefBox SPS Communication
 *
 *  Created: Tue Jan 22 11:00:53 2013
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

#ifndef __LLSF_REFBOX_SPS_COMM_H_
#define __LLSF_REFBOX_SPS_COMM_H_

#include <llsf_sps/machine_comm.h>
#include <utils/llsf/machines.h>

extern "C" {
  typedef struct _modbus modbus_t;
}

#include <string>
#include <stdint.h>
#include <vector>
#include <map>

namespace llsf_sps {
#if 0 /* just to make Emacs auto-indent happy */
}
#endif

#define SPS_NUM_MACHINES 16

class SPSComm : public MachineCommunication
{
 public:
  SPSComm(const char *host, unsigned short port,
	  llsf_utils::MachineAssignment assn = llsf_utils::ASSIGNMENT_2014);
  SPSComm(std::vector<std::string> hosts, unsigned short port,
	  llsf_utils::MachineAssignment assn = llsf_utils::ASSIGNMENT_2014);
  virtual ~SPSComm();

  void try_reconnect();

  void reset_lights();
  void set_light(unsigned int m, Light light, SignalState state);

  void reset_rfids();
  bool read_rfid(unsigned int m, uint32_t &id);
  std::map<std::string, uint32_t> read_rfids();
  void write_rfid(unsigned int m, uint32_t id);

 private:
  unsigned int plc_index(unsigned int &m);

 private:
  llsf_utils::MachineAssignment  assignment_;
  std::vector<modbus_t *>        mbs_;
};

} // end of namespace llsfrb

#endif
