/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fbzmq/zmq/Zmq.h>
#include <folly/FileUtil.h>
#include <folly/Format.h>
#include <folly/IPAddress.h>
#include <folly/Memory.h>
#include <folly/gen/Base.h>
#include <folly/gen/String.h>
#include <folly/init/Init.h>
#include <folly/system/ThreadName.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <sodium.h>

#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp2/Thrift.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/decision/Decision.h>
#include <openr/fib/Fib.h>
#include <openr/kvstore/KvStore.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/tests/OpenrWrapper.h>
#include <openr/tests/mocks/MockIoProvider.h>

using namespace std;
using namespace openr;

using apache::thrift::CompactSerializer;

namespace {

const std::chrono::seconds kMaxOpenrSyncTime(3);

const std::chrono::milliseconds kSpark2HelloTime(100);
const std::chrono::milliseconds kSpark2FastInitHelloTime(20);
const std::chrono::milliseconds kSpark2HandshakeTime(20);
const std::chrono::milliseconds kSpark2HeartbeatTime(20);
const std::chrono::milliseconds kSpark2HandshakeHoldTime(200);
const std::chrono::milliseconds kSpark2HeartbeatHoldTime(500);
const std::chrono::milliseconds kSpark2GRHoldTime(1000);
const std::chrono::milliseconds kLinkFlapInitialBackoff(1);
const std::chrono::milliseconds kLinkFlapMaxBackoff(8);

const string iface12{"1/2"};
const string iface13{"1/3"};
const string iface14{"1/4"};
const string iface21{"2/1"};
const string iface23{"2/3"};
const string iface24{"2/4"};
const string iface31{"3/1"};
const string iface32{"3/2"};
const string iface34{"3/4"};
const string iface41{"4/1"};
const string iface42{"4/2"};
const string iface43{"4/3"};

const int ifIndex12{12};
const int ifIndex13{13};
const int ifIndex14{14};
const int ifIndex21{21};
const int ifIndex23{23};
const int ifIndex24{24};
const int ifIndex41{41};
const int ifIndex42{42};
const int ifIndex43{43};
const int ifIndex31{31};
const int ifIndex32{32};
const int ifIndex34{34};

const folly::CIDRNetwork ip1V4(folly::IPAddress("192.168.0.1"), 32);
const folly::CIDRNetwork ip2V4(folly::IPAddress("192.168.0.2"), 32);
const folly::CIDRNetwork ip3V4(folly::IPAddress("192.168.0.3"), 32);
const folly::CIDRNetwork ip4V4(folly::IPAddress("192.168.0.4"), 32);

const folly::CIDRNetwork ip1V6(folly::IPAddress("fe80::1"), 128);
const folly::CIDRNetwork ip2V6(folly::IPAddress("fe80::2"), 128);
const folly::CIDRNetwork ip3V6(folly::IPAddress("fe80::3"), 128);
const folly::CIDRNetwork ip4V6(folly::IPAddress("fe80::4"), 128);

// R1 -> R2, R3, R4
const auto adj12 =
    createAdjacency("2", "1/2", "2/1", "fe80::2", "192.168.0.2", 1, 0);
const auto adj13 =
    createAdjacency("3", "1/3", "3/1", "fe80::3", "192.168.0.3", 1, 0);
const auto adj14 =
    createAdjacency("4", "1/4", "4/1", "fe80::4", "192.168.0.4", 1, 0);
// R2 -> R1, R3, R4
const auto adj21 =
    createAdjacency("1", "2/1", "1/2", "fe80::1", "192.168.0.1", 1, 0);
const auto adj23 =
    createAdjacency("3", "2/3", "3/2", "fe80::3", "192.168.0.3", 1, 0);
const auto adj24 =
    createAdjacency("4", "2/4", "4/2", "fe80::4", "192.168.0.4", 1, 0);
// R3 -> R1, R2, R4
const auto adj31 =
    createAdjacency("1", "3/1", "1/3", "fe80::1", "192.168.0.1", 1, 0);
const auto adj32 =
    createAdjacency("2", "3/2", "2/3", "fe80::2", "192.168.0.2", 1, 0);
const auto adj34 =
    createAdjacency("4", "3/4", "4/3", "fe80::4", "192.168.0.4", 1, 0);
// R4 -> R1, R2, R3
const auto adj41 =
    createAdjacency("1", "4/1", "1/4", "fe80::1", "192.168.0.1", 1, 0);
const auto adj42 =
    createAdjacency("2", "4/2", "2/4", "fe80::2", "192.168.0.2", 1, 0);
const auto adj43 =
    createAdjacency("3", "4/3", "3/4", "fe80::3", "192.168.0.3", 1, 0);

using NextHop = pair<string /* ifname */, folly::IPAddress /* nexthop ip */>;
// Note: use unordered_set bcoz paths in a route can be in arbitrary order
using NextHopsWithMetric =
    unordered_set<pair<NextHop /* nexthop */, int32_t /* path metric */>>;
using RouteMap = unordered_map<
    pair<string /* node name */, string /* ip prefix */>,
    NextHopsWithMetric>;

// disable V4 by default
NextHop
toNextHop(thrift::Adjacency adj, bool isV4 = false) {
  return {
      *adj.ifName_ref(),
      toIPAddress(isV4 ? *adj.nextHopV4_ref() : *adj.nextHopV6_ref())};
}

// Note: routeMap will be modified
void
fillRouteMap(
    const string& node,
    RouteMap& routeMap,
    const thrift::RouteDatabase& routeDb) {
  for (auto const& route : *routeDb.unicastRoutes_ref()) {
    auto prefix = toString(*route.dest_ref());
    for (const auto& nextHop : *route.nextHops_ref()) {
      const auto nextHopAddr = toIPAddress(*nextHop.address_ref());
      assert(nextHop.address_ref()->ifName_ref());
      VLOG(4) << "node: " << node << " prefix: " << prefix << " -> "
              << nextHop.address_ref()->ifName_ref().value() << " : "
              << nextHopAddr << " (" << *nextHop.metric_ref() << ")";

      routeMap[make_pair(node, prefix)].insert(
          {{nextHop.address_ref()->ifName_ref().value(), nextHopAddr},
           *nextHop.metric_ref()});
    }
  }
}

} // namespace

/**
 * Fixture for abstracting out common functionality for test
 */
class OpenrFixture : public ::testing::Test {
 protected:
  void
  SetUp() override {
    mockIoProvider = std::make_shared<MockIoProvider>();

    // start mock IoProvider thread
    mockIoProviderThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Starting mockIoProvider thread.";
      mockIoProvider->start();
      LOG(INFO) << "mockIoProvider thread got stopped.";
    });
    mockIoProvider->waitUntilRunning();
  }

  void
  TearDown() override {
    // clean up common resources
    LOG(INFO) << "Stopping mockIoProvider thread.";
    mockIoProvider->stop();
    mockIoProviderThread->join();

    // DO NOT explicitly call stop() method for Open/R instances
    // as DESCTRUCTOR in OpenrWrapper will take care of them.
  }

  /**
   * Helper function to create OpenrWrapper
   */
  OpenrWrapper<CompactSerializer>*
  createOpenr(
      std::string nodeId,
      bool v4Enabled,
      uint32_t memLimit = openr::memLimitMB) {
    auto ptr = std::make_unique<OpenrWrapper<CompactSerializer>>(
        context,
        nodeId,
        v4Enabled,
        kSpark2HelloTime,
        kSpark2FastInitHelloTime,
        kSpark2HandshakeTime,
        kSpark2HeartbeatTime,
        kSpark2HandshakeHoldTime,
        kSpark2HeartbeatHoldTime,
        kSpark2GRHoldTime,
        kLinkFlapInitialBackoff,
        kLinkFlapMaxBackoff,
        mockIoProvider,
        memLimit);
    openrWrappers_.emplace_back(std::move(ptr));
    return openrWrappers_.back().get();
  }

  // public member variables
  fbzmq::Context context;
  std::shared_ptr<MockIoProvider> mockIoProvider{nullptr};
  std::unique_ptr<std::thread> mockIoProviderThread{nullptr};

 private:
  std::vector<std::unique_ptr<OpenrWrapper<CompactSerializer>>>
      openrWrappers_{};
};

//
// Test topology:
//
//  1------2
//  |      |
//  |      |
//  3------4
//
// Test on v4 for now
//
class SimpleRingTopologyFixture : public OpenrFixture,
                                  public ::testing::WithParamInterface<bool> {};

INSTANTIATE_TEST_CASE_P(
    SimpleRingTopologyInstance, SimpleRingTopologyFixture, ::testing::Bool());

//
// Verify multi path in ring topology for both v4 and v6 and
// test IP prefix add and withdraw.
//
// TODO: need to figure out way to test e2e by addressing kvstore thrift-sync
/*
TEST_P(SimpleRingTopologyFixture, RingTopologyMultiPathTest) {
  // define interface names for the test
  mockIoProvider->addIfNameIfIndex(
      {{iface12, ifIndex12},
       {iface13, ifIndex13},
       {iface21, ifIndex21},
       {iface24, ifIndex24},
       {iface31, ifIndex31},
       {iface34, ifIndex34},
       {iface42, ifIndex42},
       {iface43, ifIndex43}});
  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface12, {{iface21, 100}}},
      {iface21, {{iface12, 100}}},
      {iface24, {{iface42, 100}}},
      {iface42, {{iface24, 100}}},
      {iface13, {{iface31, 100}}},
      {iface31, {{iface13, 100}}},
      {iface34, {{iface43, 100}}},
      {iface43, {{iface34, 100}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  bool v4Enabled(GetParam());
  v4Enabled = false;

  auto openr1 = createOpenr("1", v4Enabled, openr::memLimitMB);
  auto openr2 = createOpenr("2", v4Enabled, openr::memLimitMB);
  auto openr3 = createOpenr("3", v4Enabled, openr::memLimitMB);
  auto openr4 = createOpenr("4", v4Enabled, openr::memLimitMB);

  openr1->run();
  openr2->run();
  openr3->run();
  openr4->run();

  // wait until all aquamen got synced on kvstore
  std::this_thread::sleep_for(kMaxOpenrSyncTime);

  // make sure every openr has a prefix allocated
  EXPECT_TRUE(openr1->getIpPrefix().has_value());
  EXPECT_TRUE(openr2->getIpPrefix().has_value());
  EXPECT_TRUE(openr3->getIpPrefix().has_value());
  EXPECT_TRUE(openr4->getIpPrefix().has_value());

  // start tracking iface1
  openr1->updateInterfaceDb(
      {InterfaceInfo(
           iface12,
           true,
           ifIndex12,
           {ip1V4, ip1V6}),
       InterfaceInfo(
           iface13,
           true,
           ifIndex13,
           {ip1V4, ip1V6})});

  // start tracking iface2
  openr2->updateInterfaceDb(
      {InterfaceInfo(
           iface21,
           true,
           ifIndex21,
           {ip2V4, ip2V6}),
       InterfaceInfo(
           iface24,
           true,
           ifIndex24,
           {ip2V4, ip2V6})});

  // start tracking iface3
  openr3->updateInterfaceDb(
      {InterfaceInfo(
           iface31,
           true,
           ifIndex31,
           {ip3V4, ip3V6}),
       InterfaceInfo(
           iface34,
           true,
           ifIndex34,
           {ip3V4, ip3V6})});

  // start tracking iface4
  openr4->updateInterfaceDb(
      {InterfaceInfo(
           iface42,
           true,
           ifIndex42,
           {ip4V4, ip4V6}),
       InterfaceInfo(
           iface43,
           true,
           ifIndex43,
           {ip4V4, ip4V6})});

  // wait until all aquamen got synced on kvstore
  std::this_thread::sleep_for(kMaxOpenrSyncTime);

  // make sure the kvstores are synced
  EXPECT_TRUE(openr1->checkKeyExists("prefix:1"));
  EXPECT_TRUE(openr1->checkKeyExists("prefix:2"));
  EXPECT_TRUE(openr1->checkKeyExists("prefix:3"));
  EXPECT_TRUE(openr1->checkKeyExists("prefix:4"));
  EXPECT_TRUE(openr2->checkKeyExists("prefix:1"));
  EXPECT_TRUE(openr2->checkKeyExists("prefix:2"));
  EXPECT_TRUE(openr2->checkKeyExists("prefix:3"));
  EXPECT_TRUE(openr2->checkKeyExists("prefix:4"));
  EXPECT_TRUE(openr3->checkKeyExists("prefix:1"));
  EXPECT_TRUE(openr3->checkKeyExists("prefix:2"));
  EXPECT_TRUE(openr3->checkKeyExists("prefix:3"));
  EXPECT_TRUE(openr3->checkKeyExists("prefix:4"));
  EXPECT_TRUE(openr4->checkKeyExists("prefix:1"));
  EXPECT_TRUE(openr4->checkKeyExists("prefix:2"));
  EXPECT_TRUE(openr4->checkKeyExists("prefix:3"));
  EXPECT_TRUE(openr4->checkKeyExists("prefix:4"));

  const auto addr1 = openr1->getIpPrefix().value();
  const auto addr2 = openr2->getIpPrefix().value();
  const auto addr3 = openr3->getIpPrefix().value();
  const auto addr4 = openr4->getIpPrefix().value();
  const auto addr1V4 = openr1->getIpPrefix().value();
  const auto addr2V4 = openr2->getIpPrefix().value();
  const auto addr3V4 = openr3->getIpPrefix().value();
  const auto addr4V4 = openr4->getIpPrefix().value();

  // make sure every node has a prefix assigned
  EXPECT_NE(toString(addr1), "");
  EXPECT_NE(toString(addr2), "");
  EXPECT_NE(toString(addr3), "");
  EXPECT_NE(toString(addr4), "");

  // make sure every prefix is unique
  EXPECT_NE(toString(addr1), toString(addr2));
  EXPECT_NE(toString(addr1), toString(addr3));
  EXPECT_NE(toString(addr1), toString(addr4));
  EXPECT_NE(toString(addr2), toString(addr3));
  EXPECT_NE(toString(addr2), toString(addr4));
  EXPECT_NE(toString(addr3), toString(addr4));

  RouteMap routeMap;

  auto routeDb1 = openr1->fibDumpRouteDatabase();
  auto routeDb2 = openr2->fibDumpRouteDatabase();
  auto routeDb3 = openr3->fibDumpRouteDatabase();
  auto routeDb4 = openr4->fibDumpRouteDatabase();

  fillRouteMap("1", routeMap, routeDb1);
  fillRouteMap("2", routeMap, routeDb2);
  fillRouteMap("3", routeMap, routeDb3);
  fillRouteMap("4", routeMap, routeDb4);

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHopsWithMetric({make_pair(toNextHop(adj12, v4Enabled), 1)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHopsWithMetric({make_pair(toNextHop(adj13, v4Enabled), 1)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHopsWithMetric(
          {make_pair(toNextHop(adj12, v4Enabled), 2),
           make_pair(toNextHop(adj13, v4Enabled), 2)}));

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHopsWithMetric({make_pair(toNextHop(adj21, v4Enabled), 1)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHopsWithMetric({make_pair(toNextHop(adj24, v4Enabled), 1)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHopsWithMetric(
          {make_pair(toNextHop(adj21, v4Enabled), 2),
           make_pair(toNextHop(adj24, v4Enabled), 2)}));

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHopsWithMetric({make_pair(toNextHop(adj31, v4Enabled), 1)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHopsWithMetric({make_pair(toNextHop(adj34, v4Enabled), 1)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHopsWithMetric(
          {make_pair(toNextHop(adj31, v4Enabled), 2),
           make_pair(toNextHop(adj34, v4Enabled), 2)}));

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHopsWithMetric({make_pair(toNextHop(adj42, v4Enabled), 1)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHopsWithMetric({make_pair(toNextHop(adj43, v4Enabled), 1)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHopsWithMetric(
          {make_pair(toNextHop(adj42, v4Enabled), 2),
           make_pair(toNextHop(adj43, v4Enabled), 2)}));

  // test IP prefix add and withdraw. Add prefixes and withdraw prefixes
  // using prefix manager client, and verify the FIB route dump reflects
  // those changes on all the nodes

  const auto paddr1 = toIpPrefix("5502::/64");
  const auto prefixEntry1 =
      createPrefixEntry(paddr1, thrift::PrefixType::DEFAULT);

  // openr1 uses separate IP prefix key for each prefix
  auto resp = openr1->addPrefixEntries({prefixEntry1});
  EXPECT_TRUE(resp);
  std::this_thread::sleep_for(kMaxOpenrSyncTime);

  routeDb2 = openr2->fibDumpRouteDatabase();
  routeDb3 = openr3->fibDumpRouteDatabase();
  routeDb4 = openr4->fibDumpRouteDatabase();

  EXPECT_TRUE(
      OpenrWrapper<CompactSerializer>::checkPrefixExists(paddr1, routeDb2));
  EXPECT_TRUE(
      OpenrWrapper<CompactSerializer>::checkPrefixExists(paddr1, routeDb3));
  EXPECT_TRUE(
      OpenrWrapper<CompactSerializer>::checkPrefixExists(paddr1, routeDb4));

  const auto paddr2 = toIpPrefix("5503::/64");
  const auto prefixEntry2 =
      createPrefixEntry(paddr2, thrift::PrefixType::DEFAULT);

  // openr2 uses one prefixKey for all prefixes
  resp = openr2->addPrefixEntries({prefixEntry2});
  EXPECT_TRUE(resp);
  std::this_thread::sleep_for(kMaxOpenrSyncTime);

  routeDb1 = openr1->fibDumpRouteDatabase();
  routeDb3 = openr3->fibDumpRouteDatabase();
  routeDb4 = openr4->fibDumpRouteDatabase();

  EXPECT_TRUE(
      OpenrWrapper<CompactSerializer>::checkPrefixExists(paddr2, routeDb1));
  EXPECT_TRUE(
      OpenrWrapper<CompactSerializer>::checkPrefixExists(paddr2, routeDb3));
  EXPECT_TRUE(
      OpenrWrapper<CompactSerializer>::checkPrefixExists(paddr2, routeDb4));

  // withdraw prefix1 from openr1, check prefix1 is withdrawn and prefix2
  // is still there
  // openr1 uses separate IP prefix key for each prefix
  resp = openr1->withdrawPrefixEntries({prefixEntry1});
  EXPECT_TRUE(resp);
  std::this_thread::sleep_for(kMaxOpenrSyncTime);

  routeDb1 = openr1->fibDumpRouteDatabase();
  routeDb2 = openr2->fibDumpRouteDatabase();
  routeDb3 = openr3->fibDumpRouteDatabase();
  routeDb4 = openr4->fibDumpRouteDatabase();

  // check paddr1 is deleted from FIB
  EXPECT_FALSE(
      OpenrWrapper<CompactSerializer>::checkPrefixExists(paddr1, routeDb2));
  EXPECT_FALSE(
      OpenrWrapper<CompactSerializer>::checkPrefixExists(paddr1, routeDb3));
  EXPECT_FALSE(
      OpenrWrapper<CompactSerializer>::checkPrefixExists(paddr1, routeDb4));

  // check paddr2 exists
  EXPECT_TRUE(
      OpenrWrapper<CompactSerializer>::checkPrefixExists(paddr2, routeDb1));
  EXPECT_TRUE(
      OpenrWrapper<CompactSerializer>::checkPrefixExists(paddr2, routeDb3));
  EXPECT_TRUE(
      OpenrWrapper<CompactSerializer>::checkPrefixExists(paddr2, routeDb4));

  // Delete prefix from openr2 which uses single prefix key for all prefixes,
  // then check prefix is deleted from all other nodes
  resp = openr2->withdrawPrefixEntries({prefixEntry2});
  EXPECT_TRUE(resp);
  std::this_thread::sleep_for(kMaxOpenrSyncTime);

  routeDb1 = openr1->fibDumpRouteDatabase();
  routeDb3 = openr3->fibDumpRouteDatabase();
  routeDb4 = openr4->fibDumpRouteDatabase();

  // check paddr2 is deleted from FIB
  EXPECT_FALSE(
      OpenrWrapper<CompactSerializer>::checkPrefixExists(paddr2, routeDb1));
  EXPECT_FALSE(
      OpenrWrapper<CompactSerializer>::checkPrefixExists(paddr2, routeDb3));
  EXPECT_FALSE(
      OpenrWrapper<CompactSerializer>::checkPrefixExists(paddr2, routeDb4));
}
*/

//
// Verify system metrics
//
TEST_P(SimpleRingTopologyFixture, ResourceMonitor) {
  // define interface names for the test
  mockIoProvider->addIfNameIfIndex(
      {{iface12, ifIndex12}, {iface21, ifIndex21}});
  // connect interfaces directly
  ConnectedIfPairs connectedPairs = {
      {iface12, {{iface21, 100}}},
      {iface21, {{iface12, 100}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  bool v4Enabled(GetParam());
  v4Enabled = false;

  std::string memKey{"process.memory.rss"};
  std::string cpuKey{"process.cpu.pct"};
  std::string upTimeKey{"process.uptime.seconds"};
  uint32_t rssMemInUse{0};

  // find out rss memory in use
  {
    auto openr2 = createOpenr("2", v4Enabled);
    openr2->run();

    auto counters2 = openr2->getCounters();
    /* sleep override */
    std::this_thread::sleep_for(kMaxOpenrSyncTime);
    while (counters2.size() == 0) {
      counters2 = openr2->getCounters();
    }
    rssMemInUse = counters2[memKey] / 1e6;
  }

  uint32_t memLimitMB = static_cast<uint32_t>(rssMemInUse) + 500;
  auto openr1 = createOpenr("1", v4Enabled, memLimitMB);
  openr1->run();

  /* sleep override */
  // wait until all aquamen got synced on kvstore
  std::this_thread::sleep_for(kMaxOpenrSyncTime);

  // make sure every openr has a prefix allocated
  EXPECT_TRUE(openr1->getIpPrefix().has_value());

  // Wait for calling getCPUpercentage() twice for calculating the cpu% counter.
  // Check if counters contain the uptime, cpu and memory usage counters.
  auto counters1 = openr1->getCounters();
  while (true) {
    if (counters1.find(cpuKey) != counters1.end()) {
      EXPECT_EQ(counters1.count(cpuKey), 1);
      EXPECT_EQ(counters1.count(memKey), 1);
      break;
    }
    counters1 = openr1->getCounters();
    std::this_thread::yield();
  }
  // allocate memory to go beyond memory limit and check if watchdog
  // catches the over the limit condition
  uint32_t memUsage = static_cast<uint32_t>(counters1[memKey] / 1e6);

  if (memUsage < memLimitMB) {
    EXPECT_FALSE(openr1->watchdog->memoryLimitExceeded());
    uint32_t allocMem = memLimitMB - memUsage + 10;

    LOG(INFO) << "Allocating:" << allocMem << ", Mem in use:" << memUsage
              << ", Memory limit:" << memLimitMB << "MB";
    vector<int8_t> v((allocMem)*0x100000);
    fill(v.begin(), v.end(), 1);
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::seconds(5));
    EXPECT_TRUE(openr1->watchdog->memoryLimitExceeded());
  } else {
    // memory already recached above the limit
    EXPECT_TRUE(openr1->watchdog->memoryLimitExceeded());
  }
}

int
main(int argc, char** argv) {
  // parse command line flags
  testing::InitGoogleTest(&argc, argv);
  folly::init(&argc, &argv);
  google::InstallFailureSignalHandler();

  // init sodium security library
  if (::sodium_init() == -1) {
    LOG(ERROR) << "Failed initializing sodium";
    return 1;
  }

  // Run the tests
  return RUN_ALL_TESTS();
}
