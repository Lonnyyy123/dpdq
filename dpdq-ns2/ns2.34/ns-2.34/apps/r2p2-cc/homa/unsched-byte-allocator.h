//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/.
//

/*
 * UnschedByteAllocator.h
 *
 *  Created on: Oct 15, 2015
 *      Author: behnamm
 */

/**
 * @brief This file is taken almost as-is from https://github.com/PlatformLab/HomaSimulation/tree/omnet_simulations/RpcTransportDesign/OMNeT%2B%2BSimulation
 */
#include <map>
#include <unordered_map>
#include "homa-hdr.h"

#ifndef UNSCHEDBYTEALLOCATOR_H_
#define UNSCHEDBYTEALLOCATOR_H_

class HomaConfigDepot;

class UnschedByteAllocator
{
public:
  explicit UnschedByteAllocator(HomaConfigDepot *homaConfig);
  ~UnschedByteAllocator();
  std::vector<uint32_t> getReqUnschedDataPkts(int32_t rxAddr, uint32_t msgSize);
  void updateReqDataBytes(hdr_homa *grantPkt);
  void updateUnschedBytes(hdr_homa *grantPkt);

private:
  void initReqBytes(int32_t rxAddr);
  void initUnschedBytes(int32_t rxAddr);
  uint32_t getReqDataBytes(int32_t rxAddr, uint32_t msgSize);
  uint32_t getUnschedBytes(int32_t rxAddr, uint32_t msgSize);

private:
  std::unordered_map<int32_t, std::map<uint32_t, uint32_t>>
      rxAddrUnschedbyteMap;
  std::unordered_map<int32_t, std::map<uint32_t, uint32_t>>
      rxAddrReqbyteMap;
  HomaConfigDepot *homaConfig;
  uint32_t maxReqPktDataBytes;
  uint32_t maxUnschedPktDataBytes;
};

template <typename Small, typename Large>
Small downCast(const Large &large)
{
  Small small = static_cast<Small>(large);
  // The following comparison (rather than "large==small") allows
  // this method to convert between signed and unsigned values.
  assert(large - small == 0);
  return small;
}

#endif /* UNSCHEDBYTEALLOCATOR_H_ */
