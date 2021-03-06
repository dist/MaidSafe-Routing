/*  Copyright 2012 MaidSafe.net limited

    This MaidSafe Software is licensed to you under (1) the MaidSafe.net Commercial License,
    version 1.0 or later, or (2) The General Public License (GPL), version 3, depending on which
    licence you accepted on initial access to the Software (the "Licences").

    By contributing code to the MaidSafe Software, or to this project generally, you agree to be
    bound by the terms of the MaidSafe Contributor Agreement, version 1.0, found in the root
    directory of this project at LICENSE, COPYING and CONTRIBUTOR respectively and also
    available at: http://www.maidsafe.net/licenses

    Unless required by applicable law or agreed to in writing, the MaidSafe Software distributed
    under the GPL Licence is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS
    OF ANY KIND, either express or implied.

    See the Licences for the specific language governing permissions and limitations relating to
    use of the MaidSafe Software.                                                                 */

#include "maidsafe/routing/message_handler.h"

#include <vector>

#include "maidsafe/common/log.h"
#include "maidsafe/common/node_id.h"

#include "maidsafe/routing/client_routing_table.h"
#include "maidsafe/routing/group_change_handler.h"
#include "maidsafe/routing/message.h"
#include "maidsafe/routing/network_utils.h"
#include "maidsafe/routing/routing.pb.h"
#include "maidsafe/routing/routing_table.h"
#include "maidsafe/routing/service.h"
#include "maidsafe/routing/remove_furthest_node.h"
#include "maidsafe/routing/utils.h"

namespace maidsafe {

namespace routing {

namespace {

SingleToSingleMessage CreateSingleToSingleMessage(const protobuf::Message& proto_message) {
  return SingleToSingleMessage(proto_message.data(0),
                               SingleSource(NodeId(proto_message.source_id())),
                               SingleId(NodeId(proto_message.destination_id())),
                               static_cast<Cacheable>(proto_message.cacheable()));
}

SingleToGroupMessage CreateSingleToGroupMessage(const protobuf::Message& proto_message) {
  return SingleToGroupMessage(proto_message.data(0),
                              SingleSource(NodeId(proto_message.source_id())),
                              GroupId(NodeId(proto_message.group_destination())),
                              static_cast<Cacheable>(proto_message.cacheable()));
}

GroupToSingleMessage CreateGroupToSingleMessage(const protobuf::Message& proto_message) {
  return GroupToSingleMessage(proto_message.data(0),
                              GroupSource(GroupId(NodeId(proto_message.group_source())),
                                          SingleId(NodeId(proto_message.source_id()))),
                              SingleId(NodeId(proto_message.destination_id())),
                              static_cast<Cacheable>(proto_message.cacheable()));
}

GroupToGroupMessage CreateGroupToGroupMessage(const protobuf::Message& proto_message) {
  return GroupToGroupMessage(proto_message.data(0),
                             GroupSource(GroupId(NodeId(proto_message.group_source())),
                                         SingleId(NodeId(proto_message.source_id()))),
                             GroupId(NodeId(proto_message.group_destination())),
                             static_cast<Cacheable>(proto_message.cacheable()));
}

SingleToGroupRelayMessage CreateSingleToGroupRelayMessage(const protobuf::Message& proto_message) {
  SingleSource single_src(NodeId(proto_message.relay_id()));
  NodeId connection_id(proto_message.relay_connection_id());
  SingleSource single_src_relay_node(NodeId(proto_message.source_id()));
  SingleRelaySource single_relay_src(single_src,  // original sender
                                     connection_id,
                                     single_src_relay_node);

  return SingleToGroupRelayMessage(proto_message.data(0),
      single_relay_src,  // relay node
          GroupId(NodeId(proto_message.group_destination())),
              static_cast<Cacheable>(proto_message.cacheable()));

//  return SingleToGroupRelayMessage(proto_message.data(0),
//      SingleSourceRelay(SingleSource(NodeId(proto_message.relay_id())), // original sender
//                        NodeId(proto_message.relay_connection_id()),
//                        SingleSource(NodeId(proto_message.source_id()))),  // relay node
//          GroupId(NodeId(proto_message.group_destination())),
//              static_cast<Cacheable>(proto_message.cacheable()));
}

}  //  unnamed namespace

MessageHandler::MessageHandler(RoutingTable& routing_table,
                               ClientRoutingTable& client_routing_table, NetworkUtils& network,
                               Timer<std::string>& timer, RemoveFurthestNode& remove_furthest_node,
                               GroupChangeHandler& group_change_handler,
                               NetworkStatistics& network_statistics)
    : routing_table_(routing_table),
      client_routing_table_(client_routing_table),
      network_statistics_(network_statistics),
      network_(network),
      remove_furthest_node_(remove_furthest_node),
      group_change_handler_(group_change_handler),
      cache_manager_(routing_table_.client_mode()
                         ? nullptr
                         : (new CacheManager(routing_table_.kNodeId(), network_))),
      timer_(timer),
      response_handler_(new ResponseHandler(routing_table, client_routing_table, network_,
                                            group_change_handler)),
      service_(new Service(routing_table, client_routing_table, network_)),
      message_received_functor_(),
      typed_message_received_functors_() {}

void MessageHandler::HandleRoutingMessage(protobuf::Message& message) {
  bool request(message.request());
  switch (static_cast<MessageType>(message.type())) {
    case MessageType::kPing:
      message.request() ? service_->Ping(message) : response_handler_->Ping(message);
      break;
    case MessageType::kConnect:
      message.request() ? service_->Connect(message) : response_handler_->Connect(message);
      break;
    case MessageType::kFindNodes:
      message.request() ? service_->FindNodes(message) : response_handler_->FindNodes(message);
      break;
    case MessageType::kConnectSuccess:
      service_->ConnectSuccess(message);
      break;
    case MessageType::kConnectSuccessAcknowledgement:
      response_handler_->ConnectSuccessAcknowledgement(message);
      break;
    case MessageType::kRemove:
      message.request() ? remove_furthest_node_.RemoveRequest(message)
                        : remove_furthest_node_.RemoveResponse(message);
      break;
    case MessageType::kClosestNodesUpdate:
      {
        assert(message.request());
        auto matrix_update(group_change_handler_.ClosestNodesUpdate(message));
        if (matrix_update.first != NodeId())
          response_handler_->AddMatrixUpdateFromUnvalidatedPeer(matrix_update.first,
                                                                matrix_update.second);
      }
      if (routing_table_.client_mode())
        response_handler_->CloseNodeUpdateForClient(message);
      break;
    case MessageType::kGetGroup:
      message.request() ? service_->GetGroup(message)
                        : response_handler_->GetGroup(timer_, message);
      break;
    default:  // unknown (silent drop)
      return;
  }

  if (!request || !message.IsInitialized())
    return;

  if (routing_table_.size() == 0)  // This node can only send to bootstrap_endpoint
    network_.SendToDirect(message, network_.bootstrap_connection_id(),
                          network_.bootstrap_connection_id());
  else
    network_.SendToClosestNode(message);
}

void MessageHandler::HandleNodeLevelMessageForThisNode(protobuf::Message& message) {
  if (IsRequest(message) &&
      !IsClientToClientMessageWithDifferentNodeIds(message, routing_table_.client_mode())) {
    LOG(kSuccess) << " [" << DebugId(routing_table_.kNodeId())
                  << "] rcvd : " << MessageTypeString(message) << " from "
                  << HexSubstr(message.source_id()) << "   (id: " << message.id()
                  << ")  --NodeLevel--";
    ReplyFunctor response_functor = [=](const std::string & reply_message) {
      if (reply_message.empty()) {
        LOG(kInfo) << "Empty response for message id :" << message.id();
        return;
      }
      LOG(kSuccess) << " [" << DebugId(routing_table_.kNodeId())
                    << "] repl : " << MessageTypeString(message) << " from "
                    << HexSubstr(message.source_id()) << "   (id: " << message.id()
                    << ")  --NodeLevel Replied--";
      protobuf::Message message_out;
      message_out.set_request(false);
      message_out.set_hops_to_live(Parameters::hops_to_live);
      message_out.set_destination_id(message.source_id());
      message_out.set_type(message.type());
      message_out.set_direct(true);
      message_out.clear_data();
      message_out.set_client_node(message.client_node());
      message_out.set_routing_message(message.routing_message());
      message_out.add_data(reply_message);
      message_out.set_last_id(routing_table_.kNodeId().string());
      message_out.set_source_id(routing_table_.kNodeId().string());
      if (message.has_id())
        message_out.set_id(message.id());
      else
        LOG(kInfo) << "Message to be sent back had no ID.";

      if (message.has_relay_id())
        message_out.set_relay_id(message.relay_id());

      if (message.has_relay_connection_id()) {
        message_out.set_relay_connection_id(message.relay_connection_id());
      }
      if (routing_table_.client_mode() &&
          routing_table_.kNodeId().string() == message_out.destination_id()) {
        network_.SendToClosestNode(message_out);
        return;
      }
      if (routing_table_.kNodeId().string() != message_out.destination_id()) {
        network_.SendToClosestNode(message_out);
      } else {
        LOG(kInfo) << "Sending response to self."
                   << " id: " << message.id();
        HandleMessage(message_out);
      }
    };
    if (message_received_functor_)
      message_received_functor_(message.data(0), false, response_functor);
    else
      InvokeTypedMessageReceivedFunctor(message);  // typed message received
  } else if (IsResponse(message)) {                // response
    LOG(kInfo) << "[" << DebugId(routing_table_.kNodeId())
               << "] rcvd : " << MessageTypeString(message) << " from "
               << HexSubstr(message.source_id()) << "   (id: " << message.id()
               << ")  --NodeLevel--";
    try {
      if (!message.has_id() || message.data_size() != 1)
        BOOST_THROW_EXCEPTION(MakeError(CommonErrors::parsing_error));
      timer_.AddResponse(message.id(), message.data(0));
    }
    catch (const maidsafe_error& e) {
      LOG(kError) << e.what();
      return;
    }
    if (message.has_average_distace())
      network_statistics_.UpdateNetworkAverageDistance(NodeId(message.average_distace()));
  } else {
    LOG(kWarning) << "This node [" << DebugId(routing_table_.kNodeId())
                  << " Dropping message as client to client message not allowed."
                  << PrintMessage(message);
    message.Clear();
  }
}

void MessageHandler::HandleMessageForThisNode(protobuf::Message& message) {
  if (RelayDirectMessageIfNeeded(message))
    return;

  LOG(kVerbose) << "Message for this node."
                << " id: " << message.id();
  if (IsRoutingMessage(message))
    HandleRoutingMessage(message);
  else
    HandleNodeLevelMessageForThisNode(message);
}

void MessageHandler::HandleMessageAsClosestNode(protobuf::Message& message) {
  LOG(kVerbose) << "This node is in closest proximity to this message destination ID [ "
                << HexSubstr(message.destination_id()) << " ]."
                << " id: " << message.id();
  if (IsDirect(message)) {
    return HandleDirectMessageAsClosestNode(message);
  } else {
    return HandleGroupMessageAsClosestNode(message);
  }
}

void MessageHandler::HandleDirectMessageAsClosestNode(protobuf::Message& message) {
  assert(message.direct());
  // Dropping direct messages if this node is closest and destination node is not in routing_table_
  // or client_routing_table_.
  NodeId destination_node_id(message.destination_id());
  if (routing_table_.IsThisNodeClosestToIncludingMatrix(destination_node_id)) {
    if (routing_table_.Contains(destination_node_id) ||
        client_routing_table_.Contains(destination_node_id)) {
      return network_.SendToClosestNode(message);
    } else if (!message.has_visited() || !message.visited()) {
      message.set_visited(true);
      return network_.SendToClosestNode(message);
    } else {
      LOG(kWarning) << "Dropping message. This node [" << DebugId(routing_table_.kNodeId())
                    << "] is the closest but is not connected to destination node ["
                    << HexSubstr(message.destination_id())
                    << "], Src ID: " << HexSubstr(message.source_id())
                    << ", Relay ID: " << HexSubstr(message.relay_id()) << " id: " << message.id()
                    << PrintMessage(message);
      return;
    }
  } else {
    // if (IsCacheableRequest(message))
    //   return HandleCacheLookup(message);  // forwarding message is done by cache manager
    // else if (IsCacheableResponse(message))
    //   StoreCacheCopy(message);  //  Upper layer should take this on seperate thread

    return network_.SendToClosestNode(message);
  }
}

void MessageHandler::HandleGroupMessageAsClosestNode(protobuf::Message& message) {
  assert(!message.direct());
  bool have_node_with_group_id(routing_table_.Contains(NodeId(message.destination_id())));
  // This node is not closest to the destination node for non-direct message.
  if (!routing_table_.IsThisNodeClosestTo(NodeId(message.destination_id()), !IsDirect(message)) &&
      !have_node_with_group_id) {
    LOG(kInfo) << "This node is not closest, passing it on."
               << " id: " << message.id();
    // if (IsCacheableRequest(message))
    //   return HandleCacheLookup(message);  // forwarding message is done by cache manager
    // else if (IsCacheableResponse(message))
    //   StoreCacheCopy(message);  // Upper layer should take this on seperate thread
    return network_.SendToClosestNode(message);
  }

  if (message.has_visited() && !message.visited() &&
      (routing_table_.size() > Parameters::closest_nodes_size) &&
      (!routing_table_.IsThisNodeInRange(NodeId(message.destination_id()),
                                         Parameters::closest_nodes_size))) {
    message.set_visited(true);
    return network_.SendToClosestNode(message);
  }

  std::vector<std::string> route_history;
  if (message.route_history().size() > 1)
    route_history = std::vector<std::string>(message.route_history().begin(),
                                             message.route_history().end() - 1);
  else if ((message.route_history().size() == 1) &&
           (message.route_history(0) != routing_table_.kNodeId().string()))
    route_history.push_back(message.route_history(0));

  // Confirming from group matrix. If this node is closest to the target id or else passing on to
  // the connected peer which has the closer node.
  NodeInfo closest_to_group_leader_node;
  if (!routing_table_.IsThisNodeGroupLeader(NodeId(message.destination_id()),
                                            closest_to_group_leader_node, route_history)) {
    assert(NodeId(message.destination_id()) != closest_to_group_leader_node.node_id);
    return network_.SendToDirectAdjustedRoute(message, closest_to_group_leader_node.node_id,
                                              closest_to_group_leader_node.connection_id);
  }

  // This node is closest so will send to all replicant nodes
  uint16_t replication(static_cast<uint16_t>(message.replication()));
  if ((replication < 1) || (replication > Parameters::group_size)) {
    LOG(kError) << "Dropping invalid non-direct message."
                << " id: " << message.id();
    return;
  }

  --replication;  // Will send to self as well
  message.set_direct(true);
  message.clear_route_history();
  NodeId destination_id(message.destination_id());
  NodeId own_node_id(routing_table_.kNodeId());
  auto close_from_matrix(routing_table_.GetClosestMatrixNodes(destination_id, replication + 2));
  close_from_matrix.erase(std::remove_if(close_from_matrix.begin(), close_from_matrix.end(),
                                         [&destination_id](const NodeInfo & node_info) {
                            return node_info.node_id == destination_id;
                          }),
                          close_from_matrix.end());
  close_from_matrix.erase(std::remove_if(close_from_matrix.begin(), close_from_matrix.end(),
                                         [&own_node_id](const NodeInfo & node_info) {
                            return node_info.node_id == own_node_id;
                          }),
                          close_from_matrix.end());
  while (close_from_matrix.size() > replication)
    close_from_matrix.pop_back();

  std::string group_id(message.destination_id());
  std::string group_members("[" + DebugId(routing_table_.kNodeId()) + "]");

  for (const auto& i : close_from_matrix)
    group_members += std::string("[" + DebugId(i.node_id) + "]");
  LOG(kInfo) << "Group nodes for group_id " << HexSubstr(group_id) << " : " << group_members;

  for (const auto& i : close_from_matrix) {
    LOG(kInfo) << "[" << DebugId(own_node_id) << "] - "
               << "Replicating message to : " << HexSubstr(i.node_id.string())
               << " [ group_id : " << HexSubstr(group_id) << "]"
               << " id: " << message.id();
    message.set_destination_id(i.node_id.string());
    NodeInfo node;
    if (routing_table_.GetNodeInfo(i.node_id, node)) {
      network_.SendToDirect(message, node.node_id, node.connection_id);
    } else {
      network_.SendToClosestNode(message);
    }
  }

  message.set_destination_id(routing_table_.kNodeId().string());

  if (IsRoutingMessage(message)) {
    LOG(kVerbose) << "HandleGroupMessageAsClosestNode if, msg id: " << message.id();
    HandleRoutingMessage(message);
  } else {
    LOG(kVerbose) << "HandleGroupMessageAsClosestNode else, msg id: " << message.id();
    HandleNodeLevelMessageForThisNode(message);
  }
}

void MessageHandler::HandleMessageAsFarNode(protobuf::Message& message) {
  if (message.has_visited() &&
      routing_table_.IsThisNodeClosestTo(NodeId(message.destination_id()), !message.direct()) &&
      !message.direct() && !message.visited())
    message.set_visited(true);
  LOG(kVerbose) << "[" << DebugId(routing_table_.kNodeId())
                << "] is not in closest proximity to this message destination ID [ "
                << HexSubstr(message.destination_id()) << " ]; sending on."
                << " id: " << message.id();
  network_.SendToClosestNode(message);
}

void MessageHandler::HandleMessage(protobuf::Message& message) {
  LOG(kVerbose) << "[" << DebugId(routing_table_.kNodeId()) << "]"
                << " MessageHandler::HandleMessage handle message with id: " << message.id();
  if (!ValidateMessage(message)) {
    LOG(kWarning) << "Validate message failed， id: " << message.id();
    assert((message.hops_to_live() > 0) && "Message has traversed maximum number of hops allowed");
    return;
  }

  // Decrement hops_to_live
  message.set_hops_to_live(message.hops_to_live() - 1);

  if (IsValidCacheableGet(message)) {
    LOG(kInfo) << "MessageHandler::HandleMessage " << message.id() << " with cache manager";
    return HandleCacheLookup(message);  // forwarding message is done by cache manager
  }
  if (IsValidCacheablePut(message)) {
    LOG(kInfo) << "MessageHandler::HandleMessage " << message.id() << " StoreCacheCopy";
    StoreCacheCopy(message);  //  Upper layer should take this on separate thread
  }

  // If group message request to self id
  if (IsGroupMessageRequestToSelfId(message)) {
    LOG(kInfo) << "MessageHandler::HandleMessage " << message.id() << " HandleGroupMessageToSelfId";
    return HandleGroupMessageToSelfId(message);
  }

  // If this node is a client
  if (routing_table_.client_mode()) {
    LOG(kInfo) << "MessageHandler::HandleMessage " << message.id() << " HandleClientMessage";
    return HandleClientMessage(message);
  }

  // Relay mode message
  if (message.source_id().empty()) {
    LOG(kInfo) << "MessageHandler::HandleMessage " << message.id() << " HandleRelayRequest";
    return HandleRelayRequest(message);
  }

  // Invalid source id, unknown message
  if (NodeId(message.source_id()).IsZero()) {
    LOG(kWarning) << "Stray message dropped, need valid source ID for processing."
                  << " id: " << message.id();
    return;
  }

  // Direct message
  if (message.destination_id() == routing_table_.kNodeId().string()) {
    LOG(kInfo) << "MessageHandler::HandleMessage " << message.id() << " HandleMessageForThisNode";
    return HandleMessageForThisNode(message);
  }

  if (IsRelayResponseForThisNode(message)) {
    LOG(kInfo) << "MessageHandler::HandleMessage " << message.id() << " HandleRoutingMessage";
    return HandleRoutingMessage(message);
  }

  if (client_routing_table_.Contains(NodeId(message.destination_id())) && IsDirect(message)) {
    LOG(kInfo) << "MessageHandler::HandleMessage " << message.id()
               << " HandleMessageForNonRoutingNodes";
    return HandleMessageForNonRoutingNodes(message);
  }

  // This node is in closest proximity to this message
  if (routing_table_.IsThisNodeInRange(NodeId(message.destination_id()),
                                       Parameters::group_size) ||
      (routing_table_.IsThisNodeClosestTo(NodeId(message.destination_id()), !message.direct()) &&
       message.visited())) {
    LOG(kInfo) << "MessageHandler::HandleMessage " << message.id() << " HandleMessageAsClosestNode";
    return HandleMessageAsClosestNode(message);
  } else {
    LOG(kInfo) << "MessageHandler::HandleMessage " << message.id() << " HandleMessageAsFarNode";
    return HandleMessageAsFarNode(message);
  }
}

void MessageHandler::HandleMessageForNonRoutingNodes(protobuf::Message& message) {
  auto client_routing_nodes(client_routing_table_.GetNodesInfo(NodeId(message.destination_id())));
  assert(!client_routing_nodes.empty() && message.direct());
// Below bit is not needed currently as SendToClosestNode will do this check anyway
// TODO(Team) consider removing the check from SendToClosestNode() after
// adding more client tests
  if (IsClientToClientMessageWithDifferentNodeIds(message, true)) {
    LOG(kWarning) << "This node [" << DebugId(routing_table_.kNodeId())
                  << " Dropping message as client to client message not allowed."
                  << PrintMessage(message);
    return;
  }
  LOG(kInfo) << "This node has message destination in its ClientRoutingTable. Dest id : "
             << HexSubstr(message.destination_id()) << " message id: " << message.id();
  return network_.SendToClosestNode(message);
}

void MessageHandler::HandleRelayRequest(protobuf::Message& message) {
  assert(!message.has_source_id());
  if ((message.destination_id() == routing_table_.kNodeId().string()) && IsRequest(message)) {
    LOG(kVerbose) << "Relay request with this node's ID as destination ID"
                  << " id: " << message.id();
    // If group message request to this node's id sent by relay requester node
    if ((message.destination_id() == routing_table_.kNodeId().string()) && message.request() &&
        !message.direct()) {
      message.set_source_id(routing_table_.kNodeId().string());
      return HandleGroupMessageToSelfId(message);
    } else {
      return HandleMessageForThisNode(message);
    }
  }

  // This node may be closest for group messages.
  if (message.request() && routing_table_.IsThisNodeClosestTo(NodeId(message.destination_id()))) {
    if (message.direct()) {
      return HandleDirectRelayRequestMessageAsClosestNode(message);
    } else {
      return HandleGroupRelayRequestMessageAsClosestNode(message);
    }
  }

  // This node is now the src ID for the relay message and will send back response to original node.
  message.set_source_id(routing_table_.kNodeId().string());
  network_.SendToClosestNode(message);
}

void MessageHandler::HandleDirectRelayRequestMessageAsClosestNode(protobuf::Message& message) {
  assert(message.direct());
  // Dropping direct messages if this node is closest and destination node is not in routing_table_
  // or client_routing_table_.
  NodeId destination_node_id(message.destination_id());
  if (routing_table_.IsThisNodeClosestTo(destination_node_id)) {
    if (routing_table_.Contains(destination_node_id) ||
        client_routing_table_.Contains(destination_node_id)) {
      message.set_source_id(routing_table_.kNodeId().string());
      return network_.SendToClosestNode(message);
    } else {
      LOG(kWarning) << "Dropping message. This node [" << DebugId(routing_table_.kNodeId())
                    << "] is the closest but is not connected to destination node ["
                    << HexSubstr(message.destination_id())
                    << "], Src ID: " << HexSubstr(message.source_id())
                    << ", Relay ID: " << HexSubstr(message.relay_id()) << " id: " << message.id()
                    << PrintMessage(message);
      return;
    }
  } else {
    return network_.SendToClosestNode(message);
  }
}

void MessageHandler::HandleGroupRelayRequestMessageAsClosestNode(protobuf::Message& message) {
  assert(!message.direct());
  bool have_node_with_group_id(routing_table_.Contains(NodeId(message.destination_id())));
  // This node is not closest to the destination node for non-direct message.
  if (!routing_table_.IsThisNodeClosestTo(NodeId(message.destination_id()), !IsDirect(message)) &&
      !have_node_with_group_id) {
    LOG(kInfo) << "This node is not closest, passing it on."
               << " id: " << message.id();
    message.set_source_id(routing_table_.kNodeId().string());
    return network_.SendToClosestNode(message);
  }

  // Confirming from group matrix. If this node is closest to the target id or else passing on to
  // the connected peer which has the closer node.
  NodeInfo closest_to_group_leader_node;
  if (!routing_table_.IsThisNodeGroupLeader(NodeId(message.destination_id()),
                                            closest_to_group_leader_node)) {
    assert(NodeId(message.destination_id()) != closest_to_group_leader_node.node_id);
    message.set_source_id(routing_table_.kNodeId().string());
    return network_.SendToDirect(message, closest_to_group_leader_node.node_id,
                                 closest_to_group_leader_node.connection_id);
  }

  // This node is closest so will send to all replicant nodes
  uint16_t replication(static_cast<uint16_t>(message.replication()));
  if ((replication < 1) || (replication > Parameters::group_size)) {
    LOG(kError) << "Dropping invalid non-direct message."
                << " id: " << message.id();
    return;
  }

  --replication;  // This node will be one of the group member.
  message.set_direct(true);
  if (have_node_with_group_id)
    ++replication;
  auto close(routing_table_.GetClosestNodes(NodeId(message.destination_id()), replication));

  if (have_node_with_group_id)
    close.erase(close.begin());
  std::string group_id(message.destination_id());
  std::string group_members("[" + DebugId(routing_table_.kNodeId()) + "]");

  for (const auto& i : close)
    group_members += std::string("[" + DebugId(i) + "]");
  LOG(kInfo) << "Group members for group_id " << HexSubstr(group_id) << " are: " << group_members;
  // This node relays back the responses
  message.set_source_id(routing_table_.kNodeId().string());
  for (const auto& i : close) {
    LOG(kInfo) << "Replicating message to : " << HexSubstr(i.string())
               << " [ group_id : " << HexSubstr(group_id) << "]"
               << " id: " << message.id();
    message.set_destination_id(i.string());
    NodeInfo node;
    if (routing_table_.GetNodeInfo(i, node)) {
      network_.SendToDirect(message, node.node_id, node.connection_id);
    }
  }

  message.set_destination_id(routing_table_.kNodeId().string());
//  message.clear_source_id();
  if (IsRoutingMessage(message))
    HandleRoutingMessage(message);
  else
    HandleNodeLevelMessageForThisNode(message);
}

// Special case when response of a relay comes through an alternative route.
bool MessageHandler::IsRelayResponseForThisNode(protobuf::Message& message) {
  if (IsRoutingMessage(message) && message.has_relay_id() &&
      (message.relay_id() == routing_table_.kNodeId().string())) {
    LOG(kVerbose) << "Relay response through alternative route";
    return true;
  } else {
    return false;
  }
}

bool MessageHandler::RelayDirectMessageIfNeeded(protobuf::Message& message) {
  assert(message.destination_id() == routing_table_.kNodeId().string());
  if (!message.has_relay_id()) {
    //    LOG(kVerbose) << "Message don't have relay ID.";
    return false;
  }

  if (IsRequest(message) && message.has_actual_destination_is_relay_id() &&
          (message.destination_id() != message.relay_id())) {
    message.clear_destination_id();
    message.clear_actual_destination_is_relay_id();  // so that it is picked currectly at recepient
    LOG(kVerbose) << "Relaying request to " << HexSubstr(message.relay_id())
                  << " id: " << message.id();
    network_.SendToClosestNode(message);
    return true;
  }

  // Only direct responses need to be relayed
  if (IsResponse(message) && (message.destination_id() != message.relay_id())) {
    message.clear_destination_id();  // to allow network util to identify it as relay message
    LOG(kVerbose) << "Relaying response to " << HexSubstr(message.relay_id())
                  << " id: " << message.id();
    network_.SendToClosestNode(message);
    return true;
  }

  // not a relay message response, its for this node
  //    LOG(kVerbose) << "Not a relay message response, it's for this node";
  return false;
}

void MessageHandler::HandleClientMessage(protobuf::Message& message) {
  assert(routing_table_.client_mode() && "Only client node should handle client messages");
  if (message.source_id().empty()) {  // No relays allowed on client.
    LOG(kWarning) << "Stray message at client node. No relays allowed."
                  << " id: " << message.id();
    return;
  }
  if (IsRoutingMessage(message)) {
    LOG(kVerbose) << "Client Routing Response for " << DebugId(routing_table_.kNodeId())
                  << " from " << HexSubstr(message.source_id()) << " id: " << message.id();
    HandleRoutingMessage(message);
  } else if ((message.destination_id() == routing_table_.kNodeId().string())) {
    LOG(kVerbose) << "Client NodeLevel Response for " << DebugId(routing_table_.kNodeId())
                  << " from " << HexSubstr(message.source_id()) << " id: " << message.id();
    HandleNodeLevelMessageForThisNode(message);
  } else {
    LOG(kWarning) << DebugId(routing_table_.kNodeId()) << " silently drop message "
                  << " from " << HexSubstr(message.source_id()) << " id: " << message.id();
  }
}

// Special case : If group message request to self id
bool MessageHandler::IsGroupMessageRequestToSelfId(protobuf::Message& message) {
  return ((message.source_id() == routing_table_.kNodeId().string()) &&
          (message.destination_id() == routing_table_.kNodeId().string()) && message.request() &&
          !message.direct());
}

void MessageHandler::HandleGroupMessageToSelfId(protobuf::Message& message) {
  assert(message.source_id() == routing_table_.kNodeId().string());
  assert(message.destination_id() == routing_table_.kNodeId().string());
  assert(message.request());
  assert(!message.direct());
  LOG(kInfo) << "Sending group message to self id. Passing on to the closest peer to replicate";
  network_.SendToClosestNode(message);
}

void MessageHandler::InvokeTypedMessageReceivedFunctor(const protobuf::Message& proto_message) {
  if ((!proto_message.has_group_source() && !proto_message.has_group_destination()) &&
      typed_message_received_functors_.single_to_single) {  // Single to Single
    typed_message_received_functors_.single_to_single(CreateSingleToSingleMessage(proto_message));
  } else if ((!proto_message.has_group_source() && proto_message.has_group_destination()) &&
             typed_message_received_functors_.single_to_group) {
    // Single to Group
    if (proto_message.has_relay_id() && proto_message.has_relay_connection_id()) {
      typed_message_received_functors_.single_to_group_relay(
          CreateSingleToGroupRelayMessage(proto_message));
    } else {
      typed_message_received_functors_.single_to_group(CreateSingleToGroupMessage(proto_message));
    }
  } else if ((proto_message.has_group_source() && !proto_message.has_group_destination()) &&
             typed_message_received_functors_.group_to_single) {
    typed_message_received_functors_.group_to_single(CreateGroupToSingleMessage(proto_message));
  } else if ((proto_message.has_group_source() && proto_message.has_group_destination()) &&
             typed_message_received_functors_.group_to_group) {  // Group to Group
    typed_message_received_functors_.group_to_group(CreateGroupToGroupMessage(proto_message));
  } else {
    assert(false);
  }
}

void MessageHandler::set_message_and_caching_functor(MessageAndCachingFunctors functors) {
  message_received_functor_ = functors.message_received;
  // Initialise caching functors here
}

void MessageHandler::set_typed_message_and_caching_functor(TypedMessageAndCachingFunctor functors) {
  typed_message_received_functors_.single_to_single = functors.single_to_single.message_received;
  typed_message_received_functors_.single_to_group = functors.single_to_group.message_received;
  typed_message_received_functors_.group_to_single = functors.group_to_single.message_received;
  typed_message_received_functors_.group_to_group = functors.group_to_group.message_received;
  typed_message_received_functors_.single_to_group_relay =
      functors.single_to_group_relay.message_received;
  // Initialise caching functors here
}

void MessageHandler::set_request_public_key_functor(
    RequestPublicKeyFunctor request_public_key_functor) {
  response_handler_->set_request_public_key_functor(request_public_key_functor);
  service_->set_request_public_key_functor(request_public_key_functor);
}

void MessageHandler::HandleCacheLookup(protobuf::Message& message) {
  assert(!routing_table_.client_mode());
  assert(IsCacheableGet(message));
  cache_manager_->HandleGetFromCache(message);
}

void MessageHandler::StoreCacheCopy(const protobuf::Message& message) {
  assert(!routing_table_.client_mode());
  assert(IsCacheablePut(message));
  cache_manager_->AddToCache(message);
}

bool MessageHandler::IsValidCacheableGet(const protobuf::Message& message) {
  // TODO(Prakash): need to differentiate between typed and un typed api
  return (IsCacheableGet(message) && IsNodeLevelMessage(message) && Parameters::caching &&
          !routing_table_.client_mode());
}

bool MessageHandler::IsValidCacheablePut(const protobuf::Message& message) {
  // TODO(Prakash): need to differentiate between typed and un typed api
  return (IsNodeLevelMessage(message) && Parameters::caching && !routing_table_.client_mode() &&
          IsCacheablePut(message) && !IsRequest(message));
}

}  // namespace routing

}  // namespace maidsafe
