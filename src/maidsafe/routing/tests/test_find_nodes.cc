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

#include <vector>

#include "boost/thread/future.hpp"

#include "maidsafe/routing/routing_api.h"
#include "maidsafe/routing/routing_api_impl.h"
#include "maidsafe/routing/rpcs.h"
#include "maidsafe/routing/network_utils.h"
#include "maidsafe/routing/routing_pb.h"
#include "maidsafe/routing/tests/routing_network.h"

namespace args = std::placeholders;

namespace maidsafe {

namespace routing {

namespace test {


class FindNode : public GenericNode {
 public:
  explicit FindNode(bool client_mode = false)
      : GenericNode(client_mode),
        messages_() {
    functors_.message_received = std::bind(&FindNode::MessageReceived, this, args::_1, args::_2,
                                           args::_3, args::_4);
    LOG(kVerbose) << "RoutingNode constructor";
  }

  virtual ~FindNode() {}

  void MessageReceived(const int32_t &mesasge_type,
                       const std::string &message,
                       const NodeId &/*group_id*/,
                       ReplyFunctor reply_functor) {
    LOG(kInfo) << id_ << " -- Received: type <" << mesasge_type
               << "> message : " << message.substr(0, 10);
    std::lock_guard<std::mutex> guard(mutex_);
    messages_.push_back(std::make_pair(mesasge_type, message));
    reply_functor("Response to " + message);
  }

  void RudpSend(const Endpoint &peer_endpoint,
                const std::string &message,
                rudp::MessageSentFunctor message_sent_functor) {
    routing_->impl_->rudp_.Send(peer_endpoint, message, message_sent_functor);
  }

 protected:
  std::vector<std::pair<int32_t, std::string> > messages_;
};

template <typename NodeType>
class FindNodeNetwork : public GenericNetwork<NodeType> {
 public:
  FindNodeNetwork(void) : GenericNetwork<NodeType>() {}

 protected:
  testing::AssertionResult Find() {
    std::string find_node_rpc(rpcs::FindNodes(this->nodes_[5]->Id(), this->nodes_[3]->Id(),
        true, this->nodes_[3]->endpoint()).SerializeAsString());
    boost::promise<bool> message_sent_promise;
    auto message_sent_future = message_sent_promise.get_future();
    uint8_t attempts(0);
    rudp::MessageSentFunctor message_sent_functor = [this,
        &message_sent_promise, &attempts, &find_node_rpc, &message_sent_functor]
            (bool message_sent) {
        if (message_sent) {
          message_sent_promise.set_value(true);
        } else if (attempts < 3) {
          this->nodes_[3]->RudpSend(
              this->nodes_[1]->endpoint(),
              find_node_rpc,
              message_sent_functor);
        } else {
          message_sent_promise.set_value(false);
        }
      };
    this->nodes_[3]->RudpSend(this->nodes_[1]->endpoint(),
                              find_node_rpc, message_sent_functor);
    return testing::AssertionSuccess();
  }
};


TYPED_TEST_CASE_P(FindNodeNetwork);

TYPED_TEST_P(FindNodeNetwork, FUNC_FindNodes) {
  this->SetUpNetwork(9);
  EXPECT_TRUE(this->Find());
}

REGISTER_TYPED_TEST_CASE_P(FindNodeNetwork, FUNC_FindNodes);
INSTANTIATE_TYPED_TEST_CASE_P(MAIDSAFE, FindNodeNetwork, FindNode);

}  // namespace test

}  // namespace routing

}  // namespace maidsafe
