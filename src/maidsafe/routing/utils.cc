/*******************************************************************************
 *  Copyright 2012 maidsafe.net limited                                        *
 *                                                                             *
 *  The following source code is property of maidsafe.net limited and is not   *
 *  meant for external use.  The use of this code is governed by the licence   *
 *  file licence.txt found in the root of this directory and also on           *
 *  www.maidsafe.net.                                                          *
 *                                                                             *
 *  You are not free to copy, amend or otherwise use this source code without  *
 *  the explicit written permission of the board of directors of maidsafe.net. *
 ******************************************************************************/

#include "maidsafe/routing/utils.h"

#include "maidsafe/common/log.h"
#include "maidsafe/common/utils.h"
#include "maidsafe/rudp/managed_connections.h"
#include "maidsafe/rudp/return_codes.h"

#include "maidsafe/routing/network_utils.h"
#include "maidsafe/routing/non_routing_table.h"
#include "maidsafe/routing/parameters.h"
#include "maidsafe/routing/routing_pb.h"
#include "maidsafe/routing/routing_table.h"
#include "maidsafe/routing/rpcs.h"

namespace maidsafe {

namespace routing {

bool ClosestToMe(protobuf::Message& message, RoutingTable& routing_table) {
  return routing_table.AmIClosestNode(NodeId(message.destination_id()));
}

bool InClosestNodesToMe(protobuf::Message& message, RoutingTable& routing_table) {
  return routing_table.IsMyNodeInRange(NodeId(message.destination_id()),
                                       Parameters::closest_nodes_size);
}

void ValidateThisNode(NetworkUtils& network_,
                      RoutingTable& routing_table,
                      NonRoutingTable& non_routing_table,
                      const NodeId& node_id,
                      const asymm::PublicKey& public_key,
                      const rudp::EndpointPair& their_endpoint,
                      const rudp::EndpointPair& our_endpoint,
                      const bool& client) {
  NodeInfo node_info;
  node_info.node_id = NodeId(node_id);
  node_info.public_key = public_key;
  node_info.endpoint = their_endpoint.external;
  LOG(kVerbose) << "Calling rudp Add on endpoint = " << our_endpoint.external
                << ", their endpoint = " << their_endpoint.external;
  int result = network_.Add(our_endpoint.external, their_endpoint.external, node_id.String());

  if (result != 0) {
      LOG(kWarning) << "rudp add failed " << result;
    return;
  }
  LOG(kVerbose) << "rudp.Add result = " << result;
  bool routing_accepted_node(false);
  if (client) {
    NodeId furthest_close_node_id =
        routing_table.GetNthClosestNode(NodeId(routing_table.kKeys().identity),
                                        Parameters::closest_nodes_size).node_id;

    if (non_routing_table.AddNode(node_info, furthest_close_node_id)) {
      routing_accepted_node = true;
      LOG(kVerbose) << "Added client node to non routing table. node id : "
                    << HexSubstr(node_id.String());
    } else {
      LOG(kVerbose) << "Failed to add client node to non routing table. node id : "
                    << HexSubstr(node_id.String());
    }
  } else {
    if (routing_table.AddNode(node_info)) {
      routing_accepted_node = true;
      LOG(kVerbose) << "Added node to routing table. node id : " << HexSubstr(node_id.String());

      // ProcessSend(rpcs::ProxyConnect(node_id, NodeId(routing_table.kKeys().identity),
       //                              their_endpoint),
       //           rudp,
       //           routing_table,
       //           Endpoint());
    } else {
      LOG(kVerbose) << "failed to Add node to routing table. node id : "
                    << HexSubstr(node_id.String());
    }
  }
  if (!routing_accepted_node) {
    LOG(kVerbose) << "Not adding node to " << (client?"non-": "") << "routing table  node id "
                  << HexSubstr(node_id.String())
                  << " just added rudp connection will be removed now";
    network_.Remove(their_endpoint.external);
  }
}

bool IsRoutingMessage(const protobuf::Message& message) {
  return ((message.type() < 100) && (message.type() > -100));
}

bool IsNodeLevelMessage(const protobuf::Message& message) {
  return !IsRoutingMessage(message);
}

bool IsRequest(const protobuf::Message& message) {
  return (message.type() > 0);
}

bool IsResponse(const protobuf::Message& message) {
  return !IsRequest(message);
}

void SetProtobufEndpoint(const boost::asio::ip::udp::endpoint& endpoint,
                         protobuf::Endpoint* pbendpoint) {
  if (pbendpoint) {
    pbendpoint->set_ip(endpoint.address().to_string().c_str());
    pbendpoint->set_port(endpoint.port());
  }
}

boost::asio::ip::udp::endpoint GetEndpointFromProtobuf(const protobuf::Endpoint& pbendpoint) {
  return boost::asio::ip::udp::endpoint(boost::asio::ip::address::from_string(pbendpoint.ip()),
                                        static_cast<uint16_t>(pbendpoint.port()));
}

}  // namespace routing

}  // namespace maidsafe
