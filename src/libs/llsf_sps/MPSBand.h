/*!
* \file MPSBand.h
* \brief Definitions for communication between Refbox and Band
* \author David Masternak
* \version 1.0
*/

#ifndef MPSBAND_H
#define MPSBAND_H

#include <IMPSBand.h>
#include <MPSRefboxInterface.h>

#include <modbus/modbus.h>

/*!
* \class MPSBand
* \brief Communication between Refbox and MPS. What kind of MPS we want use.
*/
class MPSBand : public IMPSBand {
 public:
  /*!
   * \fn MPSBand(MPSRefboxInterface* cli, int addr)
   * \brief Constructor
   * \param cli reference of Refbox Interface
   * \param addr address of destination MPS
   */
  MPSBand(MPSRefboxInterface *cli, int addr);

  /*!
   * \fn run()
   * \brief sending the command to run the band
   */
  void runBand();

  /*!
   * \fn isReady()
   * \brief receive isReady command
   * \param ready received data
   * \return true if workpiece is ready and false if not
   */
  bool isReady(bool ready);

  void receiveData();

 private:
  modbus_t* ctx;
  int addr;
};

#endif // MPSBAND_H
