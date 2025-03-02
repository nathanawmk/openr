/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fmt/core.h>
#include <folly/Format.h>
#include <folly/IPAddress.h>
#include <thrift/lib/cpp/util/EnumUtils.h>
#include <thrift/lib/cpp2/Thrift.h>
#include "openr/common/NetworkUtil.h"
#include "openr/if/gen-cpp2/Network_types.h"

#include <openr/common/Constants.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/OpenrCtrl_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <type_traits>
#include <unordered_set>

namespace std {

/**
 * Make IpPrefix hashable
 */
template <>
struct hash<openr::thrift::IpPrefix> {
  size_t operator()(openr::thrift::IpPrefix const&) const;
};

/**
 * Make BinaryAddress hashable
 */
template <>
struct hash<openr::thrift::BinaryAddress> {
  size_t operator()(openr::thrift::BinaryAddress const&) const;
};

/**
 * Make MplsAction hashable
 */
template <>
struct hash<openr::thrift::MplsAction> {
  size_t operator()(openr::thrift::MplsAction const&) const;
};

/**
 * Make NextHopThrift hashable
 */
template <>
struct hash<openr::thrift::NextHopThrift> {
  size_t operator()(openr::thrift::NextHopThrift const&) const;
};

template <>
struct hash<std::unordered_set<openr::thrift::NextHopThrift>> {
  size_t operator()(
      std::unordered_set<openr::thrift::NextHopThrift> const&) const;
};

/**
 * Make UnicastRoute hashable
 */
template <>
struct hash<openr::thrift::UnicastRoute> {
  size_t operator()(openr::thrift::UnicastRoute const&) const;
};

} // namespace std

namespace openr {

template <class IPAddressVx>
thrift::BinaryAddress
toBinaryAddressImpl(const IPAddressVx& addr) {
  thrift::BinaryAddress result;
  result.addr_ref()->append(
      reinterpret_cast<const char*>(addr.bytes()), IPAddressVx::byteCount());
  return result;
}

inline thrift::BinaryAddress
toBinaryAddress(const folly::IPAddress& addr) {
  return addr.isV4() ? toBinaryAddressImpl(addr.asV4())
      : addr.isV6()  ? toBinaryAddressImpl(addr.asV6())
                     : thrift::BinaryAddress();
}

inline thrift::BinaryAddress
toBinaryAddress(const std::string& addr) {
  return toBinaryAddress(folly::IPAddress(addr));
}

inline thrift::BinaryAddress
toBinaryAddress(const thrift::NextHopThrift& nh) {
  return *nh.address_ref();
}

template <typename T>
inline folly::IPAddress
toIPAddress(const T& input) {
  return input.type != decltype(input.type)::VUNSPEC
      ? folly::IPAddress(input.addr)
      : folly::IPAddress();
}

inline folly::IPAddress
toIPAddress(const std::string& binAddr) {
  return folly::IPAddress::fromBinary(folly::ByteRange(
      reinterpret_cast<const uint8_t*>(binAddr.data()), binAddr.size()));
}

inline folly::IPAddress
toIPAddress(const thrift::BinaryAddress& addr) {
  return folly::IPAddress::fromBinary(folly::ByteRange(
      reinterpret_cast<const unsigned char*>(addr.addr_ref()->data()),
      addr.addr_ref()->size()));
}

// construct thrift::IpPrefix
inline thrift::IpPrefix
createIpPrefix(
    thrift::BinaryAddress const& prefixAddress, int16_t prefixLength) {
  thrift::IpPrefix ipPrefix;
  ipPrefix.prefixAddress_ref() = prefixAddress;
  ipPrefix.prefixLength_ref() = prefixLength;
  return ipPrefix;
}

inline thrift::IpPrefix
toIpPrefix(const folly::CIDRNetwork& network) {
  return createIpPrefix(toBinaryAddress(network.first), network.second);
}

inline thrift::IpPrefix
toIpPrefix(const std::string& prefix) {
  thrift::IpPrefix ipPrefix;
  try {
    ipPrefix = toIpPrefix(folly::IPAddress::createNetwork(prefix));
  } catch (const folly::IPAddressFormatException& e) {
    throw thrift::OpenrError(
        fmt::format("Invalid IPAddress: {}, exception: {}", prefix, e.what()));
  }
  return ipPrefix;
}

inline std::string
toString(const thrift::BinaryAddress& addr) {
  return addr.addr_ref()->empty() ? "" : toIPAddress(addr).str();
}

inline std::string
toString(const thrift::IpPrefix& ipPrefix) {
  return fmt::format(
      "{}/{}",
      toString(*ipPrefix.prefixAddress_ref()),
      *ipPrefix.prefixLength_ref());
}

inline std::string
toString(const thrift::MplsAction& mplsAction) {
  return fmt::format(
      "mpls {} {}{}",
      apache::thrift::util::enumNameSafe(*mplsAction.action_ref()),
      mplsAction.swapLabel_ref() ? std::to_string(*mplsAction.swapLabel_ref())
                                 : "",
      mplsAction.pushLabels_ref()
          ? folly::join("/", *mplsAction.pushLabels_ref())
          : "");
}

inline std::string
toString(const thrift::NextHopThrift& nextHop) {
  return fmt::format(
      "via {} dev {} weight {} metric {} area {} {} neighbor {}",
      toIPAddress(*nextHop.address_ref()).str(),
      nextHop.address_ref()->ifName_ref().value_or("N/A"),
      *nextHop.weight_ref(),
      *nextHop.metric_ref(),
      nextHop.area_ref().value_or("N/A"),
      nextHop.mplsAction_ref().has_value()
          ? toString(nextHop.mplsAction_ref().value())
          : "",
      nextHop.neighborNodeName_ref().has_value()
          ? *nextHop.neighborNodeName_ref()
          : "");
}

inline std::string
toString(const folly::IPAddress& addr) {
  return addr.str();
}

inline std::string
toString(const thrift::UnicastRoute& route) {
  std::vector<std::string> lines;
  lines.emplace_back(fmt::format("> Prefix: {}", toString(*route.dest_ref())));
  for (const auto& nh : *route.nextHops_ref()) {
    lines.emplace_back("  " + toString(nh));
  }
  return folly::join("\n", lines);
}

inline std::string
toString(const thrift::MplsRoute& route) {
  std::vector<std::string> lines;
  lines.emplace_back(fmt::format("> Label: {}", *route.topLabel_ref()));
  for (const auto& nh : *route.nextHops_ref()) {
    lines.emplace_back("  " + toString(nh));
  }
  return folly::join("\n", lines);
}

inline folly::CIDRNetwork
toIPNetwork(const thrift::IpPrefix& prefix, bool applyMask = true) {
  folly::CIDRNetwork network;
  try {
    network = folly::IPAddress::createNetwork(
        toIPAddress(*prefix.prefixAddress_ref()).str(),
        *prefix.prefixLength_ref(),
        applyMask);
  } catch (const folly::IPAddressFormatException& e) {
    throw thrift::OpenrError(fmt::format(
        "Invalid IPAddress: {}, exception: {}", toString(prefix), e.what()));
  }
  return network;
}

/**
 * TO BE DEPRECATED SOON: Backward compatible with empty remoteIfName
 * Translate remote interface name from local interface name
 * This is only applicable when remoteIfName is empty from peer adjacency
 * update It returns remoteIfName if it is there else constructs one from
 * localIfName
 */
std::string getRemoteIfName(const thrift::Adjacency& adj);

inline bool
isAddressFamilyV4(const folly::IPAddress& addr) {
  return addr.isV4();
}

inline bool
isAddressFamilyV4(const thrift::NextHopThrift& nh) {
  return toIPAddress(*nh.address_ref()).isV4();
}

} // namespace openr
