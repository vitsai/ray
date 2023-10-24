// Copyright 2023 The Ray Authors.
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
#pragma once

#include "ray/gcs/gcs_server/gcs_autoscaler_state_manager.h"

namespace ray {
namespace gcs {

class MockGcsAutoscalerStateManager : public GcsAutoscalerStateManager {
 public:
  MockGcsAutoscalerStateManager(const GcsNodeManager &gcs_node_manager, 
  const GcsPlacementGroupManager &gcs_placement_group_manager,
  std::shared_ptr<rpc::NodeManagerClientPool> raylet_client_pool = nullptr) : GcsAutoscalerStateManager("", gcs_node_manager, gcs_placement_group_manager, raylet_client_pool){}

};

} // namespace gcs
} // namespace ray