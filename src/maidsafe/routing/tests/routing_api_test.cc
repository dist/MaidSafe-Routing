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

#include <memory>
#include <vector>
#include "boost/filesystem/exception.hpp"
#include "maidsafe/common/test.h"
#include "maidsafe/routing/routing_api.h"
#include "maidsafe/common/test.h"
#include "maidsafe/common/utils.h"
#include "maidsafe/routing/routing_table.h"
#include "maidsafe/rudp/managed_connections.h"
#include "maidsafe/routing/node_id.h"
#include "maidsafe/routing/log.h"


namespace maidsafe {
namespace routing {
namespace test {

class RoutingTableAPI {
  RoutingTableAPI();
};

NodeInfo MakeNodeInfo() {
  NodeInfo node;
  node.node_id = NodeId(RandomString(64));
  asymm::Keys keys;
  asymm::GenerateKeyPair(&keys);
  node.public_key = keys.public_key;
  node.endpoint.address().from_string("192.168.1.1");
  node.endpoint.port(5000);
  return node;
}

asymm::Keys MakeKeys() {
  NodeInfo node(MakeNodeInfo());
  asymm::Keys keys;
  keys.identity = node.node_id.String();
  keys.public_key = node.public_key;
  return keys;
}

 TEST(APICtrTest, API_BadconfigFile) {
  asymm::Keys keys(MakeKeys());
   boost::filesystem::path bad_file("/bad file/ not found/ I hope/");
   boost::filesystem::path good_file
                (fs::unique_path(fs::temp_directory_path() / "test"));
   EXPECT_THROW({Routing RtAPI(keys, bad_file, false);},
                boost::filesystem::filesystem_error);
   EXPECT_NO_THROW({Routing RtAPI(keys, good_file, false);});
   EXPECT_TRUE(boost::filesystem::remove(good_file));
}

}  // namespace test
}  // namespace routing
}  // namespace maidsafe
