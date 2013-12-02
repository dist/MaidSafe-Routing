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

#include "maidsafe/routing/acknowledgement.h"

#include "maidsafe/common/asio_service.h"
#include "boost/date_time.hpp"
#include "maidsafe/common/log.h"
#include "maidsafe/routing/return_codes.h"
#include "maidsafe/routing/network_utils.h"
#include "maidsafe/routing/routing.pb.h"

namespace maidsafe {

namespace routing {

Acknowledgement::Acknowledgement(AsioService &io_service)
    : io_service_(io_service), ack_id_(RandomUint32()), mutex_(), queue_() {}

Acknowledgement::~Acknowledgement() {
  RemoveAll();
}

void Acknowledgement::RemoveAll() {
  std::vector<AckId> ack_ids;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Timers& timer : queue_) {
      ack_ids.push_back(std::get<0>(timer));
    }
  }
  LOG(kVerbose) << "Size of list: " << ack_ids.size();
  for (auto ack_id : ack_ids) {
    Remove(ack_id);
  }
}

AckId Acknowledgement::GetId() {
  std::lock_guard<std::mutex> lock(mutex_);
  return ++ack_id_;
}

void Acknowledgement::Add(const protobuf::Message& message, Handler handler, int timeout) {
  std::lock_guard<std::mutex> lock(mutex_);
  assert(message.has_ack_id() && "non-existing ack id");
  assert((message.ack_id() != 0) && "invalid ack id");

  AckId ack_id = message.ack_id();
  auto const it = std::find_if(std::begin(queue_), std::end(queue_),
                               [ack_id] (const Timers &i)->bool {
                                 return ack_id == std::get<0>(i);
                               });
  if (it == std::end(queue_)) {
    TimerPointer timer(new asio::deadline_timer(io_service_.service(),
                                                boost::posix_time::seconds(timeout)));
    timer->async_wait(handler);
    queue_.emplace_back(std::make_tuple(ack_id, message, timer, 0));
    LOG(kVerbose) << "AddAck added an ack, with id: " << ack_id;

  } else {
    LOG(kVerbose) << "Acknowledgement re-sends " << message.id();
    std::get<3>(*it)++;
    std::get<2>(*it)->expires_from_now(boost::posix_time::seconds(timeout));
    if (std::get<3>(*it) == Parameters::max_ack_attempts) {
      std::get<2>(*it)->async_wait([=] (const boost::system::error_code &/*error*/) {
                                     Remove(ack_id);
                                   });
     } else {
        std::get<2>(*it)->async_wait(handler);
     }
  }
}

void Acknowledgement::Remove(const AckId& ack_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto const it = std::find_if(std::begin(queue_), std::end(queue_),
                               [ack_id] (const Timers &i)->bool {
                                 return ack_id == std::get<0>(i);
                               });
  // assert((it != queue_.end()) && "attempt to cancel handler for non existant timer");
  if (it != queue_.end()) {
    // ack timed out or ack killed
    std::get<2>(*it)->cancel();
    queue_.erase(it);
    LOG(kVerbose) << "Clean up after ack with id: " << ack_id << " queue size: " << queue_.size();
  } else {
    LOG(kVerbose) << "Attempt to clean up a non existent ack with id" << ack_id
                  << " queue size: " << queue_.size();
  }
}

void Acknowledgement::HandleMessage(int32_t ack_id) {
  assert((ack_id != 0) && "Invalid acknowledgement id");
  LOG(kVerbose) << "MessageHandler::HandleAckMessage " << ack_id;
  Remove(ack_id);
}

bool Acknowledgement::NeedsAck(const protobuf::Message& message, const NodeId& node_id) {
  LOG(kVerbose) << "node_id: " << HexSubstr(node_id.string());

// Ack messages do not need an ack
  if (IsAck(message))
    return false;

  if (IsGroupUpdate(message))
    return false;

//  A communication between two nodes, in which one side is a relay at neither end
//  involves setting a timer.
  if (IsResponse(message) && (message.destination_id() == message.relay_id()))
    return false;

  if (message.source_id().empty())
    return false;

  LOG(kVerbose) << PrintMessage(message);
  return true;
}

}  // namespace maidsafe

}  // namespace routing
