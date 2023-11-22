// Copyright 2017 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ray/gcs/gcs_server/gcs_node_manager.h"

#include <optional>
#include <utility>

#include "ray/common/ray_config.h"
#include "ray/gcs/pb_util.h"
#include "ray/stats/stats.h"
#include "ray/util/event.h"
#include "ray/util/event_label.h"
#include "ray/util/logging.h"
#include "src/ray/protobuf/gcs.pb.h"

namespace ray {
namespace gcs {

//////////////////////////////////////////////////////////////////////////////////////////
GcsNodeManager::GcsNodeManager(
    std::shared_ptr<GcsPublisher> gcs_publisher,
    std::shared_ptr<gcs::GcsTableStorage> gcs_table_storage,
    std::shared_ptr<rpc::NodeManagerClientPool> raylet_client_pool,
    const ClusterID &cluster_id)
    : gcs_publisher_(std::move(gcs_publisher)),
      gcs_table_storage_(std::move(gcs_table_storage)),
      raylet_client_pool_(std::move(raylet_client_pool)),
      cluster_id_(cluster_id) {}

// Note: ServerCall will populate the cluster_id.
void GcsNodeManager::HandleGetClusterId(rpc::GetClusterIdRequest request,
                                        rpc::GetClusterIdReply *reply,
                                        rpc::SendReplyCallback send_reply_callback) {
  RAY_LOG(DEBUG) << "Registering GCS client!";
  reply->set_cluster_id(cluster_id_.Binary());
  GCS_RPC_SEND_REPLY(send_reply_callback, reply, Status::OK());
}

void GcsNodeManager::HandleRegisterNode(rpc::RegisterNodeRequest request,
                                        rpc::RegisterNodeReply *reply,
                                        rpc::SendReplyCallback send_reply_callback) {
  NodeID node_id = NodeID::FromBinary(request.node_info().node_id());
  RAY_LOG(INFO) << "Registering node info, node id = " << node_id
                << ", address = " << request.node_info().node_manager_address()
                << ", node name = " << request.node_info().node_name();
  auto on_done = [this, node_id, request, reply, send_reply_callback](
                     const Status &status) {
    RAY_CHECK_OK(status);
    RAY_LOG(INFO) << "Finished registering node info, node id = " << node_id
                  << ", address = " << request.node_info().node_manager_address()
                  << ", node name = " << request.node_info().node_name();
    RAY_CHECK_OK(gcs_publisher_->PublishNodeInfo(node_id, request.node_info(), nullptr));
    AddNode(std::make_shared<rpc::GcsNodeInfo>(request.node_info()));
    GCS_RPC_SEND_REPLY(send_reply_callback, reply, status);
  };
  if (request.node_info().is_head_node()) {
    // mark all old head nodes as dead if exists:
    // 1. should never happen when HA is not used
    // 2. happens when a new head node is started

    std::vector<NodeID> head_nodes;
    for (auto &node : alive_nodes_) {
      if (node.second->is_head_node()) {
        head_nodes.push_back(node.first);
      }
    }

    assert(head_nodes.size() <= 1);
    if (head_nodes.size() == 1) {
      OnNodeFailure(head_nodes[0],
                    [this, request, on_done, node_id](const Status &status) {
                      RAY_CHECK_OK(status);
                      RAY_CHECK_OK(gcs_table_storage_->NodeTable().Put(
                          node_id, request.node_info(), on_done));
                    });
    } else {
      RAY_CHECK_OK(
          gcs_table_storage_->NodeTable().Put(node_id, request.node_info(), on_done));
    }
  } else {
    RAY_CHECK_OK(
        gcs_table_storage_->NodeTable().Put(node_id, request.node_info(), on_done));
  }
  ++counts_[CountType::REGISTER_NODE_REQUEST];
}

bool GcsNodeManager::IsNodePreempted(const std::string &raylet_addr) {
  auto iter = node_map_.right.find(raylet_addr);
  if (iter == node_map_.right.end()) {
    return false;
  }
  auto maybe_node = GetDeadNode(iter->second);
  if (!maybe_node.has_value() || maybe_node.value() == nullptr) {
    return false;
  }

  auto &death_info = maybe_node.value()->death_info();
  return death_info.reason() == rpc::NodeDeathInfo::AUTOSCALER_DRAIN &&
         death_info.drain_reason() ==
             rpc::autoscaler::DrainNodeReason::DRAIN_NODE_REASON_PREEMPTION;
}

void GcsNodeManager::HandleCheckAlive(rpc::CheckAliveRequest request,
                                      rpc::CheckAliveReply *reply,
                                      rpc::SendReplyCallback send_reply_callback) {
  reply->set_ray_version(kRayVersion);
  for (const auto &addr : request.raylet_address()) {
    bool is_alive = node_map_.right.count(addr) != 0;
    reply->mutable_raylet_alive()->Add(is_alive);
    bool is_preempted = !is_alive && IsNodePreempted(addr);
    reply->mutable_raylet_preempted()->Add(is_preempted);
  }

  GCS_RPC_SEND_REPLY(send_reply_callback, reply, Status::OK());
}

void GcsNodeManager::HandleDrainNode(rpc::DrainNodeRequest request,
                                     rpc::DrainNodeReply *reply,
                                     rpc::SendReplyCallback send_reply_callback) {
  auto num_drain_request = request.drain_node_data_size();
  for (auto i = 0; i < num_drain_request; i++) {
    const auto &node_drain_request = request.drain_node_data(i);
    const auto node_id = NodeID::FromBinary(node_drain_request.node_id());

    DrainNode(node_id);
    auto drain_node_status = reply->add_drain_node_status();
    drain_node_status->set_node_id(node_id.Binary());
  };
  GCS_RPC_SEND_REPLY(send_reply_callback, reply, Status::OK());
  ++counts_[CountType::DRAIN_NODE_REQUEST];
}

void GcsNodeManager::DrainNode(const NodeID &node_id) {
  RAY_LOG(INFO) << "Draining node info, node id = " << node_id;
  auto node = RemoveNode(node_id, /* is_intended = */ true);
  if (!node) {
    RAY_LOG(INFO) << "Node " << node_id << " is already removed";
    return;
  }

  // Do the procedure to drain a node.
  node->set_state(rpc::GcsNodeInfo::DEAD);
  node->set_end_time_ms(current_sys_time_ms());
  AddDeadNodeToCache(node);
  auto node_info_delta = std::make_shared<rpc::GcsNodeInfo>();
  node_info_delta->set_node_id(node->node_id());
  node_info_delta->set_state(node->state());
  node_info_delta->set_end_time_ms(node->end_time_ms());

  RAY_CHECK_EQ(node->death_info().reason(), rpc::NodeDeathInfo::AUTOSCALER_DRAIN);

  // Set the address.
  rpc::Address remote_address;
  remote_address.set_raylet_id(node->node_id());
  remote_address.set_ip_address(node->node_manager_address());
  remote_address.set_port(node->node_manager_port());
  auto on_put_done = [this,
                      remote_address = remote_address,
                      node_id,
                      node_info_delta = node_info_delta](const Status &status) {
    auto raylet_client = raylet_client_pool_->GetOrConnectByAddress(remote_address);
    RAY_CHECK(raylet_client);
    // NOTE(sang): Drain API is not supposed to kill the raylet, but we are doing
    // this until the proper "drain" behavior is implemented. Currently, before
    // raylet is killed, it sends a drain request to GCS. That said, this can
    // happen;
    // - GCS updates the drain state and kills a raylet gracefully.
    // - Raylet kills itself and send a drain request of itself to GCS.
    // - Drain request will become a no-op in GCS.
    // This behavior is redundant, but harmless. We'll keep this behavior until we
    // implement the right drain behavior for the simplicity. Check
    // https://github.com/ray-project/ray/pull/19350 for more details.
    raylet_client->ShutdownRaylet(
        node_id,
        /*graceful*/ true,
        [this, node_id, node_info_delta = node_info_delta](
            const Status &status, const rpc::ShutdownRayletReply &reply) {
          RAY_LOG(INFO) << "Raylet " << node_id << " is drained. Status " << status
                        << ". The information will be published to the cluster.";
          /// Once the raylet is shutdown, inform all nodes that the raylet is dead.
          RAY_CHECK_OK(
              gcs_publisher_->PublishNodeInfo(node_id, *node_info_delta, nullptr));
        });
  };
  // Update node state to DEAD instead of deleting it.
  RAY_CHECK_OK(gcs_table_storage_->NodeTable().Put(node_id, *node, on_put_done));
}

void GcsNodeManager::HandleGetAllNodeInfo(rpc::GetAllNodeInfoRequest request,
                                          rpc::GetAllNodeInfoReply *reply,
                                          rpc::SendReplyCallback send_reply_callback) {
  // Here the unsafe allocate is safe here, because entry.second's life cycle is longer
  // then reply.
  // The request will be sent when call send_reply_callback and after that, reply will
  // not be used any more. But entry is still valid.
  for (const auto &entry : alive_nodes_) {
    *reply->add_node_info_list() = *entry.second;
  }
  for (const auto &entry : dead_nodes_) {
    *reply->add_node_info_list() = *entry.second;
  }
  GCS_RPC_SEND_REPLY(send_reply_callback, reply, Status::OK());
  ++counts_[CountType::GET_ALL_NODE_INFO_REQUEST];
}

void GcsNodeManager::HandleGetInternalConfig(rpc::GetInternalConfigRequest request,
                                             rpc::GetInternalConfigReply *reply,
                                             rpc::SendReplyCallback send_reply_callback) {
  auto get_system_config = [reply, send_reply_callback](
                               const ray::Status &status,
                               const boost::optional<rpc::StoredConfig> &config) {
    if (config.has_value()) {
      reply->set_config(config.get().config());
    }
    GCS_RPC_SEND_REPLY(send_reply_callback, reply, status);
  };
  RAY_CHECK_OK(
      gcs_table_storage_->InternalConfigTable().Get(UniqueID::Nil(), get_system_config));
  ++counts_[CountType::GET_INTERNAL_CONFIG_REQUEST];
}

absl::optional<std::shared_ptr<rpc::GcsNodeInfo>> GcsNodeManager::GetAliveNode(
    const ray::NodeID &node_id) const {
  auto iter = alive_nodes_.find(node_id);
  if (iter == alive_nodes_.end()) {
    return {};
  }

  return iter->second;
}

void GcsNodeManager::AddNode(std::shared_ptr<rpc::GcsNodeInfo> node) {
  auto node_id = NodeID::FromBinary(node->node_id());
  auto iter = alive_nodes_.find(node_id);
  if (iter == alive_nodes_.end()) {
    auto node_addr =
        node->node_manager_address() + ":" + std::to_string(node->node_manager_port());
    node_map_.insert(NodeIDAddrBiMap::value_type(node_id, node_addr));
    alive_nodes_.emplace(node_id, node);
    // Notify all listeners.
    for (auto &listener : node_added_listeners_) {
      listener(node);
    }
  }
}

std::shared_ptr<rpc::GcsNodeInfo> GcsNodeManager::RemoveNode(
    const ray::NodeID &node_id, bool is_intended /*= false*/) {
  std::shared_ptr<rpc::GcsNodeInfo> removed_node;
  auto iter = alive_nodes_.find(node_id);
  if (iter != alive_nodes_.end()) {
    removed_node = std::move(iter->second);
    RAY_LOG(INFO) << "Removing node, node id = " << node_id
                  << ", node name = " << removed_node->node_name();
    // Record stats that there's a new removed node.
    stats::NodeFailureTotal.Record(1);
    // Remove from alive nodes.
    alive_nodes_.erase(iter);
    node_map_.left.erase(node_id);
    if (!is_intended) {
      // Broadcast a warning to all of the drivers indicating that the node
      // has been marked as dead.
      // TODO(rkn): Define this constant somewhere else.
      std::string type = "node_removed";
      std::ostringstream error_message;
      error_message
          << "The node with node id: " << node_id
          << " and address: " << removed_node->node_manager_address()
          << " and node name: " << removed_node->node_name()
          << " has been marked dead because the detector"
          << " has missed too many heartbeats from it. This can happen when a "
             "\t(1) raylet crashes unexpectedly (OOM, preempted node, etc.) \n"
          << "\t(2) raylet has lagging heartbeats due to slow network or busy workload.";
      RAY_EVENT(ERROR, EL_RAY_NODE_REMOVED)
              .WithField("node_id", node_id.Hex())
              .WithField("ip", removed_node->node_manager_address())
          << error_message.str();
      RAY_LOG(WARNING) << error_message.str();
      auto error_data_ptr =
          gcs::CreateErrorTableData(type, error_message.str(), current_time_ms());
      RAY_CHECK_OK(gcs_publisher_->PublishError(node_id.Hex(), *error_data_ptr, nullptr));
    }

    // Notify all listeners.
    for (auto &listener : node_removed_listeners_) {
      listener(removed_node);
    }
  }
  return removed_node;
}

void GcsNodeManager::OnNodeFailure(const NodeID &node_id,
                                   const StatusCallback &node_table_updated_callback) {
  if (auto node = RemoveNode(node_id, /* is_intended = */ false)) {
    node->set_state(rpc::GcsNodeInfo::DEAD);
    node->set_end_time_ms(current_sys_time_ms());
    if (node->death_info().reason() == rpc::NodeDeathInfo::UNSPECIFIED) {
      // There was no drain in progress.
      node->mutable_death_info()->set_reason(rpc::NodeDeathInfo::UNEXPECTED_TERMINATION);
    }
    AddDeadNodeToCache(node);
    auto node_info_delta = std::make_shared<rpc::GcsNodeInfo>();
    node_info_delta->set_node_id(node->node_id());
    node_info_delta->set_state(node->state());
    node_info_delta->set_end_time_ms(node->end_time_ms());

    auto on_done = [this, node_id, node_table_updated_callback, node_info_delta](
                       const Status &status) {
      if (node_table_updated_callback != nullptr) {
        node_table_updated_callback(Status::OK());
      }
      RAY_CHECK_OK(gcs_publisher_->PublishNodeInfo(node_id, *node_info_delta, nullptr));
    };
    RAY_CHECK_OK(gcs_table_storage_->NodeTable().Put(node_id, *node, on_done));
  } else if (node_table_updated_callback != nullptr) {
    node_table_updated_callback(Status::OK());
  }
}

void GcsNodeManager::Initialize(const GcsInitData &gcs_init_data) {
  for (const auto &[node_id, node_info] : gcs_init_data.Nodes()) {
    if (node_info.state() == rpc::GcsNodeInfo::ALIVE) {
      AddNode(std::make_shared<rpc::GcsNodeInfo>(node_info));

      // Ask the raylet to do initialization in case of GCS restart.
      // The protocol is correct because when a new node joined, Raylet will do:
      //    - RegisterNode (write node to the node table)
      //    - Setup subscription
      // With this, it means we only need to ask the node registered to do resubscription.
      // And for the node failed to register, they will crash on the client side due to
      // registeration failure.
      rpc::Address remote_address;
      remote_address.set_raylet_id(node_info.node_id());
      remote_address.set_ip_address(node_info.node_manager_address());
      remote_address.set_port(node_info.node_manager_port());
      auto raylet_client = raylet_client_pool_->GetOrConnectByAddress(remote_address);
      raylet_client->NotifyGCSRestart(nullptr);
    } else if (node_info.state() == rpc::GcsNodeInfo::DEAD) {
      dead_nodes_.emplace(node_id, std::make_shared<rpc::GcsNodeInfo>(node_info));
      sorted_dead_node_list_.emplace_back(node_id, node_info.end_time_ms());
    }
  }
  sorted_dead_node_list_.sort(
      [](const std::pair<NodeID, int64_t> &left,
         const std::pair<NodeID, int64_t> &right) { return left.second < right.second; });
}

std::optional<std::shared_ptr<rpc::GcsNodeInfo>> GcsNodeManager::GetDeadNode(
    const ray::NodeID &node_id) const {
  if (auto iter = dead_nodes_.find(node_id); iter != dead_nodes_.end()) {
    return iter->second;
  }
  if (auto iter = alive_nodes_.find(node_id); iter != alive_nodes_.end()) {
    return std::nullopt;
  }

  // In the event that we don't find the node in memory, we will have to fetch
  // from storage.
  std::promise<std::optional<std::shared_ptr<rpc::GcsNodeInfo>>> node_info;
  auto fut = node_info.get_future();
  gcs_table_storage_->NodeTable().Get(
      node_id,
      [&node_info](Status status, const boost::optional<GcsNodeInfo> &maybe_info) {
        if (maybe_info.has_value()) {
          node_info.set_value(std::make_shared<GcsNodeInfo>(maybe_info.value()));
        } else {
          node_info.set_value(std::nullopt);
        }
      });

  fut.wait();
  return fut.get();
}

void GcsNodeManager::AddDeadNodeToCache(std::shared_ptr<rpc::GcsNodeInfo> node) {
  if (dead_nodes_.size() >= RayConfig::instance().maximum_gcs_dead_node_cached_count()) {
    const auto &node_id = sorted_dead_node_list_.begin()->first;
    RAY_CHECK_OK(gcs_table_storage_->NodeTable().Delete(node_id, nullptr));
    dead_nodes_.erase(sorted_dead_node_list_.begin()->first);
    sorted_dead_node_list_.erase(sorted_dead_node_list_.begin());
  }
  auto node_id = NodeID::FromBinary(node->node_id());
  dead_nodes_.emplace(node_id, node);
  sorted_dead_node_list_.emplace_back(node_id, node->end_time_ms());
}

std::string GcsNodeManager::DebugString() const {
  std::ostringstream stream;
  stream << "GcsNodeManager: "
         << "\n- RegisterNode request count: "
         << counts_[CountType::REGISTER_NODE_REQUEST]
         << "\n- DrainNode request count: " << counts_[CountType::DRAIN_NODE_REQUEST]
         << "\n- GetAllNodeInfo request count: "
         << counts_[CountType::GET_ALL_NODE_INFO_REQUEST]
         << "\n- GetInternalConfig request count: "
         << counts_[CountType::GET_INTERNAL_CONFIG_REQUEST];
  return stream.str();
}

}  // namespace gcs
}  // namespace ray
