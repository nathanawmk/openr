/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/FileUtil.h>
#include <folly/experimental/TestUtil.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#define Config_TEST_FRIENDS FRIEND_TEST(ConfigTest, PopulateInternalDb);

#include <openr/common/Constants.h>
#include <openr/common/MplsUtil.h>
#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <openr/if/gen-cpp2/BgpConfig_types.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/tests/utils/Utils.h>

#include <thrift/lib/cpp2/protocol/Serializer.h>

namespace {
const auto& testSeedPrefix =
    folly::IPAddress::createNetwork("fc00:cafe:babe::/64");
const uint8_t testAllocationPfxLen = 128;

openr::thrift::LinkMonitorConfig
getTestLinkMonitorConfig() {
  openr::thrift::LinkMonitorConfig lmConf;
  lmConf.include_interface_regexes_ref()->emplace_back("fboss.*");
  lmConf.exclude_interface_regexes_ref()->emplace_back("eth.*");
  lmConf.redistribute_interface_regexes_ref()->emplace_back("lo");
  return lmConf;
}

openr::thrift::KvstoreFloodRate
getFloodRate() {
  openr::thrift::KvstoreFloodRate floodrate;
  floodrate.flood_msg_per_sec_ref() = 1;
  floodrate.flood_msg_burst_size_ref() = 1;
  return floodrate;
}

openr::thrift::PrefixAllocationConfig
getPrefixAllocationConfig(openr::thrift::PrefixAllocationMode mode) {
  openr::thrift::PrefixAllocationConfig pfxAllocationConf;
  pfxAllocationConf.prefix_allocation_mode_ref() = mode;
  if (mode == openr::thrift::PrefixAllocationMode::DYNAMIC_ROOT_NODE) {
    pfxAllocationConf.seed_prefix_ref() =
        folly::IPAddress::networkToString(testSeedPrefix);
    pfxAllocationConf.allocate_prefix_len_ref() = testAllocationPfxLen;
  }
  return pfxAllocationConf;
}

openr::thrift::AreaConfig
getAreaConfig(const std::string& areaId) {
  openr::thrift::AreaConfig area;
  area.area_id_ref() = areaId;
  area.include_interface_regexes_ref()->emplace_back("fboss.*");
  area.neighbor_regexes_ref()->emplace_back("rsw.*");
  return area;
}

openr::thrift::SrPolicy
getSRPolicy() {
  openr::thrift::SrPolicy sr_policy;
  sr_policy.name_ref() = "sr_policy_1";

  neteng::config::routing_policy::FilterCriteria criteria;
  criteria.bgpCommunityFilters_ref() = {"COMM1", "COMM2"};
  criteria.openrTagFilters_ref() = {"TAG1", "TAG2"};
  criteria.openrAreaStack_ref() = "empty_area_stack_name";
  sr_policy.matcher_ref()->criterias_ref()->emplace_back(criteria);

  openr::thrift::RouteComputationRules rules;
  openr::thrift::AreaPathComputationRules areaRules;
  rules.areaPathComputationRules_ref()->emplace(kTestingAreaName, areaRules);
  sr_policy.rules_ref() = rules;

  return sr_policy;
}

openr::thrift::SegmentRoutingConfig
getSegmentRoutingConfig() {
  openr::thrift::SegmentRoutingConfig segment_routing_config;
  openr::thrift::SegmentRoutingAdjLabel adj_segment_label;
  openr::thrift::MplsLabelRanges prepend_labels;

  openr::thrift::LabelRange lrp4;
  openr::thrift::LabelRange lrp6;

  // prepend labels
  lrp4.start_label_ref() =
      openr::MplsConstants::kSrV4StaticMplsRouteRange.first;
  lrp4.end_label_ref() = openr::MplsConstants::kSrV4StaticMplsRouteRange.second;
  lrp6.start_label_ref() =
      openr::MplsConstants::kSrV6StaticMplsRouteRange.first;
  lrp6.end_label_ref() = openr::MplsConstants::kSrV6StaticMplsRouteRange.second;
  prepend_labels.v4_ref() = lrp4;
  prepend_labels.v6_ref() = lrp6;

  lrp4.start_label_ref() =
      openr::MplsConstants::kSrV4StaticMplsRouteRange.first;
  lrp4.end_label_ref() = openr::MplsConstants::kSrV4StaticMplsRouteRange.second;

  lrp6.start_label_ref() =
      openr::MplsConstants::kSrV6StaticMplsRouteRange.first;
  lrp6.end_label_ref() = openr::MplsConstants::kSrV6StaticMplsRouteRange.second;

  // adj segment label range
  openr::thrift::LabelRange adj_label_range;
  adj_label_range.start_label_ref() = openr::MplsConstants::kSrLocalRange.first;
  adj_label_range.end_label_ref() = openr::MplsConstants::kSrLocalRange.second;
  adj_segment_label.sr_adj_label_type_ref() =
      openr::thrift::SegmentRoutingAdjLabelType::AUTO_IFINDEX;
  adj_segment_label.adj_label_range_ref() = adj_label_range;

  // segment routing config
  segment_routing_config.sr_adj_label_ref() = adj_segment_label;
  segment_routing_config.prepend_label_ranges_ref() = prepend_labels;

  return segment_routing_config;
}

const std::string myArea = "myArea";

} // namespace

namespace openr {

class ConfigTestFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    validConfig_ = getBasicOpenrConfig();

    auto jsonSerializer = apache::thrift::SimpleJSONSerializer();
    try {
      jsonSerializer.serialize(validConfig_, &validConfigStr_);
    } catch (const std::exception& ex) {
      LOG(ERROR) << "Could not serialize config: " << folly::exceptionStr(ex);
    }

    folly::writeFileAtomic(validConfigFile_.path().string(), validConfigStr_);
  }

 protected:
  thrift::OpenrConfig validConfig_;
  std::string validConfigStr_;
  folly::test::TemporaryFile validConfigFile_;
};

TEST_F(ConfigTestFixture, ConstructFromFile) {
  EXPECT_NO_THROW(Config(validConfigFile_.path().string()));

  // thrift format error
  {
    folly::dynamic invalidConfig;
    invalidConfig = folly::parseJson(validConfigStr_);
    invalidConfig["areas"] = "This should be a vector";
    LOG(INFO) << invalidConfig;

    folly::test::TemporaryFile invalidConfigFile;
    folly::writeFileAtomic(
        invalidConfigFile.path().string(), folly::toJson(invalidConfig));
    EXPECT_ANY_THROW(Config(invalidConfigFile.path().string()));
  }

  // out of range enum: prefix_allocation_mode
  {
    auto validTConf = getBasicOpenrConfig();
    validTConf.enable_prefix_allocation_ref() = true;
    validTConf.prefix_allocation_config_ref() =
        thrift::PrefixAllocationConfig();

    std::string validConfStr;
    EXPECT_NO_THROW(apache::thrift::SimpleJSONSerializer().serialize(
        validTConf, &validConfStr));

    folly::dynamic invalidConf = folly::parseJson(validConfStr);
    // prefix_allocation_mode range [0-2]
    invalidConf["prefix_allocation_config"]["prefix_allocation_mode"] = 3;

    folly::test::TemporaryFile invalidConfFile;
    folly::writeFileAtomic(
        invalidConfFile.path().string(), folly::toJson(invalidConf));
    EXPECT_ANY_THROW(Config(invalidConfFile.path().string()));
  }
  // out of range enum: prefix_forwarding_type
  {
    folly::dynamic invalidConfig;
    invalidConfig = folly::parseJson(validConfigStr_);
    invalidConfig["prefix_forwarding_type"] = 3;
    LOG(INFO) << invalidConfig;

    folly::test::TemporaryFile invalidConfigFile;
    folly::writeFileAtomic(
        invalidConfigFile.path().string(), folly::toJson(invalidConfig));
    EXPECT_ANY_THROW(Config(invalidConfigFile.path().string()));
  }
}

TEST(ConfigTest, PopulateAreaConfig) {
  // area

  // duplicate area id
  {
    auto confInvalidArea = getBasicOpenrConfig();
    confInvalidArea.areas_ref()->emplace_back(getAreaConfig("1"));
    confInvalidArea.areas_ref()->emplace_back(getAreaConfig("1"));
    EXPECT_THROW((Config(confInvalidArea)), std::invalid_argument);
  }
  // cannot find policy definition for area policy
  {
    auto confInvalidAreaPolicy = getBasicOpenrConfig();
    auto areaConfig = getAreaConfig("1");
    areaConfig.set_import_policy_name("BLA");
    confInvalidAreaPolicy.areas_ref()->emplace_back(std::move(areaConfig));
    EXPECT_THROW((Config(confInvalidAreaPolicy)), std::invalid_argument);
  }

  // non-empty interface regex and non-empty domain name
  {
    openr::thrift::AreaConfig areaConfig;
    areaConfig.area_id_ref() = myArea;
    areaConfig.include_interface_regexes_ref()->emplace_back("iface.*");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto confValidArea = getBasicOpenrConfig("node-1", "domain", vec);
    EXPECT_NO_THROW((Config(confValidArea)));

    confValidArea.domain_ref() = "";
    EXPECT_THROW((Config(confValidArea)), std::invalid_argument);
  }

  // non-empty neighbor regexes
  {
    openr::thrift::AreaConfig areaConfig;
    areaConfig.area_id_ref() = myArea;
    areaConfig.neighbor_regexes_ref()->emplace_back("fsw.*");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto confValidArea = getBasicOpenrConfig("node-1", "domain", vec);
    EXPECT_NO_THROW((Config(confValidArea)));
  }

  // non-empty neighbor and interface regexes
  {
    openr::thrift::AreaConfig areaConfig;
    areaConfig.area_id_ref() = myArea;
    areaConfig.include_interface_regexes_ref()->emplace_back("iface.*");
    areaConfig.neighbor_regexes_ref()->emplace_back("fsw.*");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto confValidArea = getBasicOpenrConfig("node-1", "domain", vec);
    EXPECT_NO_THROW((Config(confValidArea)));
  }

  {
    openr::thrift::AreaConfig areaConfig;
    areaConfig.area_id_ref() = myArea;
    areaConfig.include_interface_regexes_ref()->emplace_back("iface.*");
    areaConfig.neighbor_regexes_ref()->emplace_back("fsw.*");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto confValidArea = getBasicOpenrConfig("node-1", "domain", vec);
    Config cfg = Config(confValidArea);
    // default area and domain area
    EXPECT_EQ(cfg.getAreas().size(), 1);
    EXPECT_EQ(cfg.getAreas().count(myArea), 1);
    EXPECT_EQ(cfg.getAreas().count("1"), 0);
  }

  // invalid include_interface_regexes
  {
    openr::thrift::AreaConfig areaConfig;
    areaConfig.area_id_ref() = myArea;
    areaConfig.include_interface_regexes_ref()->emplace_back("[0-9]++");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto conf = getBasicOpenrConfig("node-1", "domain", vec);
    EXPECT_THROW(auto c = Config(conf), std::invalid_argument);
  }
  //  invalid exclude_interface_regexes
  {
    openr::thrift::AreaConfig areaConfig;
    *areaConfig.area_id_ref() = myArea;
    areaConfig.exclude_interface_regexes_ref()->emplace_back("boom\\");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto conf = getBasicOpenrConfig("node-1", "domain", vec);
    EXPECT_THROW(auto c = Config(conf), std::invalid_argument);
  }
  //  invalid redistribute_interface_regexes
  {
    openr::thrift::AreaConfig areaConfig;
    areaConfig.area_id_ref() = myArea;
    areaConfig.redistribute_interface_regexes_ref()->emplace_back("*");
    std::vector<openr::thrift::AreaConfig> vec = {areaConfig};
    auto conf = getBasicOpenrConfig("node-1", "domain", vec);
    EXPECT_THROW(auto c = Config(conf), std::invalid_argument);
  }

  // area segment node label
  {
    auto confAreaPolicy = getBasicOpenrConfig();
    openr::thrift::AreaConfig areaConfig = getAreaConfig("1");

    openr::thrift::SegmentRoutingNodeLabel node_segment_label;
    areaConfig.area_sr_node_label_ref() = node_segment_label;
    confAreaPolicy.areas_ref()->emplace_back(areaConfig);

    // Area config with incomplete segment node label config
    EXPECT_THROW((Config(confAreaPolicy)), std::invalid_argument);

    openr::thrift::LabelRange node_segment_label_range;
    node_segment_label_range.start_label_ref() =
        openr::MplsConstants::kSrGlobalRange.first;
    node_segment_label_range.end_label_ref() =
        openr::MplsConstants::kSrGlobalRange.second;
    node_segment_label.node_segment_label_range_ref() =
        node_segment_label_range;

    // Type is AUTO
    node_segment_label.sr_node_label_type_ref() =
        openr::thrift::SegmentRoutingNodeLabelType::AUTO;

    for (auto& areaConf : *confAreaPolicy.areas_ref()) {
      areaConf.area_sr_node_label_ref() = node_segment_label;
    }

    // valid node segment label config
    EXPECT_NO_THROW((Config(confAreaPolicy)));

    // invalid label range and type is AUTO
    node_segment_label_range.end_label_ref() =
        openr::MplsConstants::kSrGlobalRange.first;
    node_segment_label_range.start_label_ref() =
        openr::MplsConstants::kSrGlobalRange.second;
    node_segment_label.node_segment_label_range_ref() =
        node_segment_label_range;
    for (auto& areaConf : *confAreaPolicy.areas_ref()) {
      areaConf.area_sr_node_label_ref() = node_segment_label;
    }
    EXPECT_THROW((Config(confAreaPolicy)), std::invalid_argument);

    // Type is STATIC but no static label provided
    node_segment_label.sr_node_label_type_ref() =
        openr::thrift::SegmentRoutingNodeLabelType::STATIC;
    for (auto& areaConf : *confAreaPolicy.areas_ref()) {
      areaConf.area_sr_node_label_ref() = node_segment_label;
    }

    EXPECT_THROW((Config(confAreaPolicy)), std::invalid_argument);
  }

  // area prepend label
  {
    auto confAreaPolicy = getBasicOpenrConfig();
    openr::thrift::AreaConfig areaConfig = getAreaConfig("1");

    openr::thrift::MplsLabelRanges prepend_labels;
    openr::thrift::LabelRange lrp4;
    openr::thrift::LabelRange lrp6;

    // prepend labels
    lrp4.start_label_ref() =
        openr::MplsConstants::kSrV4StaticMplsRouteRange.first;
    lrp4.end_label_ref() =
        openr::MplsConstants::kSrV4StaticMplsRouteRange.second;
    lrp6.start_label_ref() =
        openr::MplsConstants::kSrV6StaticMplsRouteRange.first;
    lrp6.end_label_ref() =
        openr::MplsConstants::kSrV6StaticMplsRouteRange.second;
    prepend_labels.v4_ref() = lrp4;
    prepend_labels.v6_ref() = lrp6;

    lrp4.start_label_ref() =
        openr::MplsConstants::kSrV4StaticMplsRouteRange.first;
    lrp4.end_label_ref() =
        openr::MplsConstants::kSrV4StaticMplsRouteRange.second;

    lrp6.start_label_ref() =
        openr::MplsConstants::kSrV6StaticMplsRouteRange.first;
    lrp6.end_label_ref() =
        openr::MplsConstants::kSrV6StaticMplsRouteRange.second;

    areaConfig.prepend_label_ranges_ref() = prepend_labels;

    // valid prepend label config
    confAreaPolicy.areas_ref()->emplace_back(areaConfig);
    EXPECT_NO_THROW((Config(confAreaPolicy)));
  }

  // area adjacency label
  {
    auto confAreaPolicy = getBasicOpenrConfig();
    openr::thrift::AreaConfig areaConfig = getAreaConfig("1");

    openr::thrift::SegmentRoutingAdjLabel adj_segment_label;
    openr::thrift::LabelRange adj_label_range;
    adj_label_range.start_label_ref() =
        openr::MplsConstants::kSrLocalRange.first;
    adj_label_range.end_label_ref() =
        openr::MplsConstants::kSrLocalRange.second;
    adj_segment_label.sr_adj_label_type_ref() =
        openr::thrift::SegmentRoutingAdjLabelType::AUTO_IFINDEX;
    adj_segment_label.adj_label_range_ref() = adj_label_range;

    areaConfig.sr_adj_label_ref() = adj_segment_label;

    // valid adj label config
    confAreaPolicy.areas_ref()->emplace_back(areaConfig);
    EXPECT_NO_THROW((Config(confAreaPolicy)));

    // disabled adj label
    (*confAreaPolicy.areas_ref()).clear();
    adj_segment_label.sr_adj_label_type_ref() =
        openr::thrift::SegmentRoutingAdjLabelType::AUTO_IFINDEX;
    EXPECT_NO_THROW((Config(confAreaPolicy)));
  }
}

TEST(ConfigTest, AreaConfiguration) {
  openr::thrift::AreaConfig areaConfig;
  areaConfig.area_id_ref() = "myArea";
  areaConfig.include_interface_regexes_ref()->emplace_back("iface.*");
  areaConfig.exclude_interface_regexes_ref()->emplace_back(".*400.*");
  areaConfig.exclude_interface_regexes_ref()->emplace_back(".*450.*");
  areaConfig.redistribute_interface_regexes_ref()->emplace_back("loopback1");
  areaConfig.neighbor_regexes_ref()->emplace_back("fsw.*");
  Config cfg{getBasicOpenrConfig("node-1", "domain", {areaConfig})};

  auto const& areaConf = cfg.getAreas().at("myArea");
  EXPECT_TRUE(areaConf.shouldPeerWithNeighbor("fsw001"));
  EXPECT_FALSE(areaConf.shouldPeerWithNeighbor("rsw001"));
  EXPECT_FALSE(areaConf.shouldPeerWithNeighbor(""));

  EXPECT_TRUE(areaConf.shouldDiscoverOnIface("iface20"));
  EXPECT_FALSE(areaConf.shouldDiscoverOnIface("iface400"));
  EXPECT_FALSE(areaConf.shouldDiscoverOnIface("iface450"));
  EXPECT_FALSE(areaConf.shouldDiscoverOnIface("loopback1"));
  EXPECT_FALSE(areaConf.shouldDiscoverOnIface(""));

  EXPECT_TRUE(areaConf.shouldRedistributeIface("loopback1"));
  EXPECT_FALSE(areaConf.shouldRedistributeIface("loopback10"));
  EXPECT_FALSE(areaConf.shouldRedistributeIface("iface450"));
  EXPECT_FALSE(areaConf.shouldRedistributeIface(""));
}

TEST(ConfigTest, BgpTranslationConfig) {
  auto tConfig = getBasicOpenrConfig();
  tConfig.enable_bgp_peering_ref() = true;
  tConfig.bgp_config_ref() = thrift::BgpConfig();
  tConfig.bgp_translation_config_ref() = thrift::BgpRouteTranslationConfig();
  auto& translationConfig = *tConfig.bgp_translation_config_ref();

  // Legacy translation disabled - But new translation hasn't been
  {
    translationConfig.enable_bgp_to_openr_ref() = true;
    translationConfig.enable_openr_to_bgp_ref() = false;
    translationConfig.disable_legacy_translation_ref() = true;
    EXPECT_THROW((Config(tConfig)), std::invalid_argument);
  }

  // Legacy translation disabled - But new translation hasn't been
  {
    translationConfig.enable_bgp_to_openr_ref() = false;
    translationConfig.enable_openr_to_bgp_ref() = true;
    translationConfig.disable_legacy_translation_ref() = true;
    EXPECT_THROW((Config(tConfig)), std::invalid_argument);
  }

  // Legacy translation disabled and new translation enabled
  {
    translationConfig.enable_bgp_to_openr_ref() = true;
    translationConfig.enable_openr_to_bgp_ref() = true;
    translationConfig.disable_legacy_translation_ref() = true;
    EXPECT_NO_THROW((Config(tConfig)));
  }
}

TEST(ConfigTest, PopulateInternalDb) {
  // features

  // KSP2_ED_ECMP with IP
  {
    auto confInvalid = getBasicOpenrConfig();
    confInvalid.prefix_forwarding_type_ref() = thrift::PrefixForwardingType::IP;
    confInvalid.prefix_forwarding_algorithm_ref() =
        thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP;
    EXPECT_THROW((Config(confInvalid)), std::invalid_argument);
  }

  // RibPolicy
  {
    auto conf = getBasicOpenrConfig();
    conf.enable_rib_policy_ref() = true;
    EXPECT_TRUE(Config(conf).isRibPolicyEnabled());
  }

  // kvstore

  // flood_msg_per_sec <= 0
  {
    auto confInvalidFloodMsgPerSec = getBasicOpenrConfig();
    confInvalidFloodMsgPerSec.kvstore_config_ref()->flood_rate_ref() =
        getFloodRate();
    confInvalidFloodMsgPerSec.kvstore_config_ref()
        ->flood_rate_ref()
        ->flood_msg_per_sec_ref() = 0;
    EXPECT_THROW((Config(confInvalidFloodMsgPerSec)), std::out_of_range);
  }
  // flood_msg_burst_size <= 0
  {
    auto confInvalidFloodMsgPerSec = getBasicOpenrConfig();
    confInvalidFloodMsgPerSec.kvstore_config_ref()->flood_rate_ref() =
        getFloodRate();
    confInvalidFloodMsgPerSec.kvstore_config_ref()
        ->flood_rate_ref()
        ->flood_msg_burst_size_ref() = 0;
    EXPECT_THROW((Config(confInvalidFloodMsgPerSec)), std::out_of_range);
  }

  // Spark

  // Exception: neighbor_discovery_port <= 0 or > 65535
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config_ref()->neighbor_discovery_port_ref() = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::out_of_range);

    confInvalidSpark.spark_config_ref()->neighbor_discovery_port_ref() = 65536;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::out_of_range);
  }

  // Exception: hello_time_s <= 0
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config_ref()->hello_time_s_ref() = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::out_of_range);
  }

  // Exception: fastinit_hello_time_ms <= 0
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config_ref()->fastinit_hello_time_ms_ref() = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::out_of_range);
  }

  // Exception: fastinit_hello_time_ms > hello_time_s
  {
    auto confInvalidSpark2 = getBasicOpenrConfig();
    confInvalidSpark2.spark_config_ref()->fastinit_hello_time_ms_ref() = 10000;
    confInvalidSpark2.spark_config_ref()->hello_time_s_ref() = 2;
    EXPECT_THROW(auto c = Config(confInvalidSpark2), std::invalid_argument);
  }

  // Exception: keepalive_time_s <= 0
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config_ref()->keepalive_time_s_ref() = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::out_of_range);
  }

  // Exception: keepalive_time_s > hold_time_s
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config_ref()->keepalive_time_s_ref() = 10;
    confInvalidSpark.spark_config_ref()->hold_time_s_ref() = 5;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
  }

  // Exception: graceful_restart_time_s < 3 * keepalive_time_s
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config_ref()->keepalive_time_s_ref() = 10;
    confInvalidSpark.spark_config_ref()->graceful_restart_time_s_ref() = 20;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
  }

  // Exception step_detector_fast_window_size >= 0
  //           step_detector_slow_window_size >= 0
  //           step_detector_lower_threshold >= 0
  //           step_detector_upper_threshold >= 0
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config_ref()
        ->step_detector_conf_ref()
        ->fast_window_size_ref() = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
    confInvalidSpark.spark_config_ref()
        ->step_detector_conf_ref()
        ->slow_window_size_ref() = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
    confInvalidSpark.spark_config_ref()
        ->step_detector_conf_ref()
        ->lower_threshold_ref() = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
    confInvalidSpark.spark_config_ref()
        ->step_detector_conf_ref()
        ->upper_threshold_ref() = -1;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
  }

  // Exception step_detector_fast_window_size > step_detector_slow_window_size
  //           step_detector_lower_threshold > step_detector_upper_threshold
  {
    auto confInvalidSpark = getBasicOpenrConfig();
    confInvalidSpark.spark_config_ref()
        ->step_detector_conf_ref()
        ->fast_window_size_ref() = 10;
    confInvalidSpark.spark_config_ref()
        ->step_detector_conf_ref()
        ->slow_window_size_ref() = 5;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);

    confInvalidSpark.spark_config_ref()
        ->step_detector_conf_ref()
        ->upper_threshold_ref() = 5;
    confInvalidSpark.spark_config_ref()
        ->step_detector_conf_ref()
        ->lower_threshold_ref() = 10;
    EXPECT_THROW(auto c = Config(confInvalidSpark), std::invalid_argument);
  }

  // Monitor

  // Exception monitor_max_event_log >= 0
  {
    auto confInvalidMon = getBasicOpenrConfig();
    confInvalidMon.monitor_config_ref()->max_event_log_ref() = -1;
    EXPECT_THROW(auto c = Config(confInvalidMon), std::out_of_range);
  }

  // link monitor

  // linkflap_initial_backoff_ms < 0
  {
    auto confInvalidLm = getBasicOpenrConfig();
    confInvalidLm.link_monitor_config_ref()->linkflap_initial_backoff_ms_ref() =
        -1;
    EXPECT_THROW(auto c = Config(confInvalidLm), std::out_of_range);
  }
  // linkflap_max_backoff_ms < 0
  {
    auto confInvalidLm = getBasicOpenrConfig();
    confInvalidLm.link_monitor_config_ref()->linkflap_max_backoff_ms_ref() = -1;
    EXPECT_THROW(auto c = Config(confInvalidLm), std::out_of_range);
  }
  // linkflap_initial_backoff_ms > linkflap_max_backoff_ms
  {
    auto confInvalidLm = getBasicOpenrConfig();
    confInvalidLm.link_monitor_config_ref()->linkflap_initial_backoff_ms_ref() =
        360000;
    confInvalidLm.link_monitor_config_ref()->linkflap_max_backoff_ms_ref() =
        300000;
    EXPECT_THROW(auto c = Config(confInvalidLm), std::out_of_range);
  }

  // prefix allocation

  // enable_prefix_allocation = true, prefix_allocation_config = null
  {
    auto confInvalidPa = getBasicOpenrConfig();
    confInvalidPa.enable_prefix_allocation_ref() = true;
    EXPECT_THROW((Config(confInvalidPa)), std::invalid_argument);
  }
  // prefix_allocation_mode != DYNAMIC_ROOT_NODE, seed_prefix and
  // allocate_prefix_len set
  {
    auto confInvalidPa = getBasicOpenrConfig();
    confInvalidPa.enable_prefix_allocation_ref() = true;
    confInvalidPa.prefix_allocation_config_ref() = getPrefixAllocationConfig(
        thrift::PrefixAllocationMode::DYNAMIC_ROOT_NODE);
    confInvalidPa.prefix_allocation_config_ref()->prefix_allocation_mode_ref() =
        thrift::PrefixAllocationMode::DYNAMIC_LEAF_NODE;
    EXPECT_THROW((Config(confInvalidPa)), std::invalid_argument);
  }
  // prefix_allocation_mode = DYNAMIC_ROOT_NODE, seed_prefix and
  // allocate_prefix_len = null
  {
    auto confInvalidPa = getBasicOpenrConfig();
    confInvalidPa.enable_prefix_allocation_ref() = true;
    confInvalidPa.prefix_allocation_config_ref() =
        thrift::PrefixAllocationConfig();
    confInvalidPa.prefix_allocation_config_ref()->prefix_allocation_mode_ref() =
        thrift::PrefixAllocationMode::DYNAMIC_ROOT_NODE;
    EXPECT_THROW((Config(confInvalidPa)), std::invalid_argument);
  }
  // seed_prefix: invalid ipadrres format
  {
    auto confInvalidPa = getBasicOpenrConfig();
    confInvalidPa.enable_prefix_allocation_ref() = true;
    confInvalidPa.prefix_allocation_config_ref() = getPrefixAllocationConfig(
        thrift::PrefixAllocationMode::DYNAMIC_ROOT_NODE);
    confInvalidPa.prefix_allocation_config_ref()->seed_prefix_ref() =
        "fc00:cafe:babe:/64";
    EXPECT_ANY_THROW((Config(confInvalidPa)));
  }
  // allocate_prefix_len: <= seed_prefix subnet length
  {
    auto confInvalidPa = getBasicOpenrConfig();
    confInvalidPa.enable_prefix_allocation_ref() = true;
    confInvalidPa.prefix_allocation_config_ref() = getPrefixAllocationConfig(
        thrift::PrefixAllocationMode::DYNAMIC_ROOT_NODE);
    confInvalidPa.prefix_allocation_config_ref()->allocate_prefix_len_ref() =
        60;
    EXPECT_THROW((Config(confInvalidPa)), std::out_of_range);
  }
  // seed_prefix v4, enable_v4 = false
  {
    auto confInvalidPa = getBasicOpenrConfig();
    confInvalidPa.enable_v4_ref() = false;

    confInvalidPa.enable_prefix_allocation_ref() = true;
    confInvalidPa.prefix_allocation_config_ref() = getPrefixAllocationConfig(
        thrift::PrefixAllocationMode::DYNAMIC_ROOT_NODE);
    confInvalidPa.prefix_allocation_config_ref()->seed_prefix_ref() =
        "127.0.0.1/24";
    confInvalidPa.prefix_allocation_config_ref()->allocate_prefix_len_ref() =
        32;
    EXPECT_THROW((Config(confInvalidPa)), std::invalid_argument);
  }

  // bgp peering

  // bgp peering enabled with empty bgp_config
  {
    auto confInvalid = getBasicOpenrConfig();
    confInvalid.enable_bgp_peering_ref() = true;

    // Both bgp-config & translation-config are none
    confInvalid.bgp_config_ref().reset();
    confInvalid.bgp_translation_config_ref().reset();
    EXPECT_THROW((Config(confInvalid)), std::invalid_argument);

    // bgp config is set but translation-config is not
    confInvalid.bgp_config_ref() = thrift::BgpConfig();
    confInvalid.bgp_translation_config_ref().reset();
    // TODO: Expect an exception instead of default initialization
    // EXPECT_THROW((Config(confInvalid)), std::invalid_argument);
    EXPECT_EQ(
        thrift::BgpRouteTranslationConfig(),
        Config(confInvalid).getBgpTranslationConfig());

    // translation-config is set but bgp-config is not
    confInvalid.bgp_config_ref().reset();
    confInvalid.bgp_translation_config_ref() =
        thrift::BgpRouteTranslationConfig();
    EXPECT_THROW((Config(confInvalid)), std::invalid_argument);
  }

  // watchdog

  // watchdog enabled with empty watchdog_config
  {
    auto confInvalid = getBasicOpenrConfig();
    confInvalid.enable_watchdog_ref() = true;
    EXPECT_THROW((Config(confInvalid)), std::invalid_argument);
  }

  // vip service
  {
    auto conf = getBasicOpenrConfig();
    EXPECT_FALSE(Config(conf).isVipServiceEnabled());
    conf.enable_vip_service_ref() = true;
    EXPECT_THROW(Config(conf).isVipServiceEnabled(), std::invalid_argument);
    EXPECT_THROW(Config(conf).checkVipServiceConfig(), std::invalid_argument);
    conf.vip_service_config_ref() = {};
    conf.vip_service_config_ref()->ingress_policy_ref() = "test_policy";
    // There is no area_policies, so should throw.
    EXPECT_THROW(Config(conf).checkVipServiceConfig(), std::invalid_argument);
    conf.area_policies_ref() = neteng::config::routing_policy::PolicyConfig();
    conf.area_policies_ref()->filters_ref() =
        neteng::config::routing_policy::PolicyFilters();
    conf.area_policies_ref()->filters_ref()->routePropagationPolicy_ref() =
        neteng::config::routing_policy::Filters();
    // There is policies, but no vip ingress policy, should throw
    EXPECT_THROW(Config(conf).checkVipServiceConfig(), std::invalid_argument);
    std::map<std::string, neteng::config::routing_policy::Filter> policy;
    policy["test_policy"] = neteng::config::routing_policy::Filter();
    conf.area_policies_ref()
        ->filters_ref()
        ->routePropagationPolicy_ref()
        ->objects_ref() = policy;
    // There is vip ingress policy in area_policies, should pass
    EXPECT_NO_THROW(Config(conf).checkVipServiceConfig());
  }

  // FIB route deletion
  {
    auto conf = getBasicOpenrConfig();
    conf.route_delete_delay_ms_ref() = -1;
    EXPECT_THROW((Config(conf)), std::invalid_argument);

    conf.route_delete_delay_ms_ref() = 0;
    EXPECT_NO_THROW((Config(conf)));

    conf.route_delete_delay_ms_ref() = 1000;
    EXPECT_NO_THROW((Config(conf)));
  }

  // Validate SR Policy configuration
  {
    auto conf = getBasicOpenrConfig();
    conf.enable_segment_routing_ref() = true;

    // Test SR Policy config with no area path computation rules
    thrift::SegmentRoutingConfig srCfg;
    thrift::SrPolicy srPolicyCfg1;
    srPolicyCfg1.name_ref() = "SR Policy 1";
    srCfg.sr_policies_ref() = {srPolicyCfg1};
    conf.segment_routing_config_ref() = srCfg;
    EXPECT_THROW((Config(conf)), std::invalid_argument);

    // Add area path computation rules for an area with no configuration
    thrift::AreaPathComputationRules areaRules;
    conf.segment_routing_config_ref()
        ->sr_policies_ref()
        ->at(0)
        .rules_ref()
        ->areaPathComputationRules_ref()
        ->emplace("foo", areaRules);
    EXPECT_THROW((Config(conf)), std::invalid_argument);

    // Add an invalid matcher
    neteng::config::routing_policy::FilterCriteria criteria;
    criteria.bgpCommunityFilters_ref() = {"COMM1", "COMM2"};
    conf.segment_routing_config_ref()
        ->sr_policies_ref()
        ->at(0)
        .matcher_ref()
        ->criterias_ref()
        ->emplace_back(criteria);
    conf.segment_routing_config_ref()
        ->sr_policies_ref()
        ->at(0)
        .rules_ref()
        ->areaPathComputationRules_ref()
        ->clear();
    conf.segment_routing_config_ref()
        ->sr_policies_ref()
        ->at(0)
        .rules_ref()
        ->areaPathComputationRules_ref()
        ->emplace("test_area_name", areaRules);
    EXPECT_THROW((Config(conf)), std::invalid_argument);
  }
}

TEST(ConfigTest, GeneralGetter) {
  // config without bgp peering
  {
    auto tConfig = getBasicOpenrConfig(
        "node-1",
        "domain",
        {}, /* area config */
        true /* enableV4 */,
        false /* enableSegmentRouting */,
        true /*dryrun*/);
    auto config = Config(tConfig);

    // getNodeName
    EXPECT_EQ("node-1", config.getNodeName());

    // getDomainName
    EXPECT_EQ("domain", config.getDomainName());

    // getAreaIds
    EXPECT_EQ(1, config.getAreas().size());
    EXPECT_EQ(1, config.getAreas().count(kTestingAreaName));

    // enable_v4
    EXPECT_TRUE(config.isV4Enabled());
    // enable_segment_routing
    EXPECT_FALSE(config.isSegmentRoutingEnabled());
    // isBgpPeeringEnabled
    EXPECT_FALSE(config.isBgpPeeringEnabled());
    // enable_flood_optimization
    EXPECT_FALSE(config.isFloodOptimizationEnabled());
    // enable_best_route_selection
    EXPECT_FALSE(config.isBestRouteSelectionEnabled());
    // enable_v4_over_v6_nexthop
    EXPECT_FALSE(config.isV4OverV6NexthopEnabled());
    // enable_vip_service
    EXPECT_FALSE(config.isVipServiceEnabled());

    // getSparkConfig
    EXPECT_EQ(*tConfig.spark_config_ref(), config.getSparkConfig());
  }

  // config without bgp peering and only for v4_over_v6_nexthop
  {
    auto tConfig = getBasicOpenrConfig(
        "node-1",
        "domain",
        {} /* area config */,
        true /* enable v4 */,
        false /* enableSegmentRouting */,
        true /* dryrun */,
        true /* enableV4OverV6Nexthop */);
    auto config = Config(tConfig);

    // enable_v4_over_v6_nexthop
    EXPECT_TRUE(config.isV4OverV6NexthopEnabled());
  }

  // config with watchdog
  {
    auto tConfig = getBasicOpenrConfig("fsw001");
    tConfig.enable_watchdog_ref() = true;
    const auto& watchdogConf = thrift::WatchdogConfig();
    tConfig.watchdog_config_ref() = watchdogConf;

    auto config = Config(tConfig);

    EXPECT_TRUE(config.isWatchdogEnabled());
    EXPECT_EQ(watchdogConf, config.getWatchdogConfig());
  }
}

TEST(ConfigTest, KvstoreGetter) {
  auto tConfig = getBasicOpenrConfig();
  auto config = Config(tConfig);
  const auto& kvstoreConf = thrift::KvstoreConfig();

  // getKvStoreConfig
  EXPECT_EQ(kvstoreConf, config.getKvStoreConfig());

  // getKvStoreKeyTtl
  EXPECT_EQ(std::chrono::milliseconds(300000), config.getKvStoreKeyTtl());
}

TEST(ConfigTest, LinkMonitorGetter) {
  auto tConfig = getBasicOpenrConfig();
  const auto& lmConf = getTestLinkMonitorConfig();
  tConfig.link_monitor_config_ref() = lmConf;
  // set empty area list to see doamin get converted to area
  tConfig.areas_ref().emplace();
  auto config = Config(tConfig);

  // getLinkMonitorConfig
  EXPECT_EQ(lmConf, config.getLinkMonitorConfig());

  // check to see the link monitor options got converted to an area config with
  // domainName
  auto const& domainNameArea =
      config.getAreas().at(thrift::Types_constants::kDefaultArea());
  EXPECT_TRUE(domainNameArea.shouldDiscoverOnIface("fboss10"));
  EXPECT_FALSE(domainNameArea.shouldDiscoverOnIface("eth0"));

  EXPECT_TRUE(domainNameArea.shouldRedistributeIface("lo"));
  EXPECT_FALSE(domainNameArea.shouldRedistributeIface("eth0"));
}

TEST(ConfigTest, PrefixAllocatorGetter) {
  auto tConfig = getBasicOpenrConfig();
  tConfig.enable_prefix_allocation_ref() = true;
  const auto paConf = getPrefixAllocationConfig(
      thrift::PrefixAllocationMode::DYNAMIC_ROOT_NODE);
  tConfig.prefix_allocation_config_ref() = paConf;
  auto config = Config(tConfig);

  // isPrefixAllocationEnabled
  EXPECT_TRUE(config.isPrefixAllocationEnabled());

  // getPrefixAllocationConfig
  EXPECT_EQ(paConf, config.getPrefixAllocationConfig());

  // getPrefixAllocationParams
  const PrefixAllocationParams& params = {testSeedPrefix, testAllocationPfxLen};
  EXPECT_EQ(params, config.getPrefixAllocationParams());
}

TEST(ConfigTest, SRPolicyConfig) {
  auto tConfig = getBasicOpenrConfig();
  auto srConf = getSegmentRoutingConfig();
  auto srPolicy = getSRPolicy();
  srConf.sr_policies_ref() = {srPolicy};
  tConfig.segment_routing_config_ref() = srConf;

  thrift::BgpConfig bgpConfig;
  thrift::BgpPeer bgpPeer;
  bgpPeer.add_path_ref() = thrift::AddPath::RECEIVE;
  bgpPeer.peer_addr_ref() = "::1";
  bgpConfig.peers_ref() = {bgpPeer};
  tConfig.enable_bgp_peering_ref() = true;
  tConfig.enable_segment_routing_ref() = true;
  tConfig.bgp_config_ref() = bgpConfig;

  // Segment routing (SR) policies should not be configured
  // when BGP add_path is configured
  EXPECT_THROW((Config(tConfig)), std::invalid_argument);

  tConfig.enable_segment_routing_ref() = false;
  // Segment routing should be configured when BGP add_path is configured
  EXPECT_THROW((Config(tConfig)), std::invalid_argument);

  tConfig.enable_segment_routing_ref() = true;
  tConfig.segment_routing_config_ref()->sr_policies_ref().reset();

  // No SR Policy configured and BGP add_path is enabled
  EXPECT_NO_THROW((Config(tConfig)));
}

TEST(ConfigTest, SegmentRoutingConfig) {
  auto tConfig = getBasicOpenrConfig();
  const auto& srConf = getSegmentRoutingConfig();
  tConfig.segment_routing_config_ref() = srConf;
  auto config = Config(tConfig);

  // getSegmentRoutingConfig
  EXPECT_EQ(srConf, config.getSegmentRoutingConfig());
  EXPECT_EQ(
      *config.getAdjSegmentLabels().sr_adj_label_type_ref(),
      openr::thrift::SegmentRoutingAdjLabelType::AUTO_IFINDEX);
}

TEST(ConfigTest, AddPathConfig) {
  auto tConfig = getBasicOpenrConfig();
  thrift::BgpConfig bgpConfig;
  thrift::BgpPeer bgpPeer;
  bgpPeer.add_path_ref() = thrift::AddPath::RECEIVE;
  bgpPeer.peer_addr_ref() = "::1";
  bgpConfig.peers_ref() = {bgpPeer};
  tConfig.enable_bgp_peering_ref() = true;
  tConfig.enable_segment_routing_ref() = false;
  tConfig.bgp_config_ref() = bgpConfig;
  EXPECT_THROW((Config(tConfig)), std::invalid_argument);

  tConfig.enable_segment_routing_ref() = true;
  EXPECT_NO_THROW((Config(tConfig)));
  EXPECT_TRUE(Config(tConfig).isBgpAddPathEnabled());

  bgpPeer.add_path_ref() = thrift::AddPath::NONE;
  bgpConfig.peers_ref() = {bgpPeer};
  tConfig.bgp_config_ref() = bgpConfig;
  EXPECT_FALSE(Config(tConfig).isBgpAddPathEnabled());
}

TEST(ConfigTest, EorTime) {
  auto tConfig = getBasicOpenrConfig();
  tConfig.spark_config_ref()->keepalive_time_s_ref() = 2;

  // Create config without any explicit EOR and ensure that it is implicitly set
  {
    EXPECT_FALSE(tConfig.eor_time_s_ref());
    auto config = Config(tConfig);
    ASSERT_TRUE(config.getConfig().eor_time_s_ref());
    EXPECT_EQ(6, *config.getConfig().eor_time_s_ref());
  }

  // Explicitly set eor time and make sure it is not altered
  {
    EXPECT_FALSE(tConfig.eor_time_s_ref());
    tConfig.eor_time_s_ref() = 2;
    auto config = Config(tConfig);
    ASSERT_TRUE(config.getConfig().eor_time_s_ref());
    EXPECT_EQ(2, *config.getConfig().eor_time_s_ref());
  }
}

} // namespace openr
