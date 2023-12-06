// Copyright  The Ray Authors.
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

namespace ray {
namespace gcs {

template <typename Key, typename Data>
class MockGcsTable : public GcsTable<Key, Data> {
 public:
  MOCK_METHOD(Status,
              Put,
              (const Key &key, const Data &value, const StatusCallback &callback),
              (override));
  MOCK_METHOD(Status,
              Delete,
              (const Key &key, const StatusCallback &callback),
              (override));
  MOCK_METHOD(Status,
              BatchDelete,
              (const std::vector<Key> &keys, const StatusCallback &callback),
              (override));
};

}  // namespace gcs
}  // namespace ray

namespace ray {
namespace gcs {

template <typename Key, typename Data>
class MockGcsTableWithJobId : public GcsTableWithJobId<Key, Data> {
 public:
  MOCK_METHOD(Status,
              Put,
              (const Key &key, const Data &value, const StatusCallback &callback),
              (override));
  MOCK_METHOD(Status,
              Delete,
              (const Key &key, const StatusCallback &callback),
              (override));
  MOCK_METHOD(Status,
              BatchDelete,
              (const std::vector<Key> &keys, const StatusCallback &callback),
              (override));
  MOCK_METHOD(JobID, GetJobIdFromKey, (const Key &key), (override));
};

}  // namespace gcs
}  // namespace ray

namespace ray {
namespace gcs {

class MockGcsJobTable : public GcsJobTable {
 public:
};

}  // namespace gcs
}  // namespace ray

namespace ray {
namespace gcs {

class MockGcsActorTable : public GcsActorTable {
 public:
  MockGcsActorTable() : GcsActorTable(nullptr) {}

  MOCK_METHOD(JobID, GetJobIdFromKey, (const ActorID &key), (override));
};

}  // namespace gcs
}  // namespace ray

namespace ray {
namespace gcs {

class MockGcsPlacementGroupTable : public GcsPlacementGroupTable {
 public:
};

}  // namespace gcs
}  // namespace ray

namespace ray {
namespace gcs {

class MockGcsActorTaskSpecTable : public GcsActorTaskSpecTable {
 public:
  MockGcsActorTaskSpecTable() : GcsActorTaskSpecTable(GetNullClient()) {}

  MOCK_METHOD(Status,
              Put,
              (const ActorID &, const TaskSpec &, const StatusCallback &),
              (override));

  std::shared_ptr<StoreClient> &GetNullClient() {
    static std::shared_ptr<StoreClient> store_client = nullptr;
    return store_client;
  }
};

class MockGcsNodeTable : public GcsNodeTable {
 public:
  MockGcsNodeTable() : GcsNodeTable(nullptr){};

  MOCK_METHOD(Status,
              Put,
              (const NodeID &key,
               const GcsNodeInfo &value,
               const StatusCallback &callback),
              (override));
};

}  // namespace gcs
}  // namespace ray

namespace ray {
namespace gcs {

class MockGcsPlacementGroupScheduleTable : public GcsPlacementGroupScheduleTable {
 public:
};

}  // namespace gcs
}  // namespace ray

namespace ray {
namespace gcs {

class MockGcsResourceUsageBatchTable : public GcsResourceUsageBatchTable {
 public:
};

}  // namespace gcs
}  // namespace ray

namespace ray {
namespace gcs {

class MockGcsWorkerTable : public GcsWorkerTable {
 public:
};

}  // namespace gcs
}  // namespace ray

namespace ray {
namespace gcs {

class MockGcsInternalConfigTable : public GcsInternalConfigTable {
 public:
};

}  // namespace gcs
}  // namespace ray

namespace ray {
namespace gcs {

/*
class FakeGcsTableStorage : public GcsTableStorage {
 public:
  explicit FakeGcsTableStorage() : FakeGcsTableStorage(nullptr) {}

  FakeGcsTableStorage(std::shared_ptr<StoreClient> store_client) :
GcsTableStorage(store_client) { job_table_ =
std::make_unique<MockGcsJobTable>(store_client_); actor_table_ =
std::make_unique<MockGcsActorTable>(store_client_); actor_task_spec_table_ =
std::make_unique<MockGcsActorTaskSpecTable>(store_client_); placement_group_table_ =
std::make_unique<MockGcsPlacementGroupTable>(store_client_); node_table_ =
std::make_unique<MockGcsNodeTable>(store_client_); placement_group_schedule_table_ =
        std::make_unique<MockGcsPlacementGroupScheduleTable>(store_client_);
    resource_usage_batch_table_ =
        std::make_unique<MockGcsResourceUsageBatchTable>(store_client_);
    worker_table_ = std::make_unique<MockGcsWorkerTable>(store_client_);
    system_config_table_ = std::make_unique<MockGcsInternalConfigTable>(store_client_);
  }
};
*/

class MockGcsTableStorage : public GcsTableStorage {
 public:
  MockGcsTableStorage() : GcsTableStorage(nullptr) {}

  MOCK_METHOD((GcsNodeTable &), NodeTable, (), (override));
  MOCK_METHOD((GcsActorTaskSpecTable &), ActorTaskSpecTable, (), (override));
  MOCK_METHOD((GcsActorTable &), ActorTable, (), (override));
};

}  // namespace gcs
}  // namespace ray

namespace ray {
namespace gcs {

class MockRedisGcsTableStorage : public RedisGcsTableStorage {
 public:
};

}  // namespace gcs
}  // namespace ray

namespace ray {
namespace gcs {

class MockInMemoryGcsTableStorage : public InMemoryGcsTableStorage {
 public:
};

}  // namespace gcs
}  // namespace ray
