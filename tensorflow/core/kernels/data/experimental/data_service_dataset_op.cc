/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/core/kernels/data/experimental/data_service_dataset_op.h"

#include <map>
#include <memory>
#include <queue>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_cat.h"
#include "tensorflow/core/data/dataset.pb.h"
#include "tensorflow/core/data/service/data_service.h"
#include "tensorflow/core/data/service/grpc_util.h"
#include "tensorflow/core/distributed_runtime/rpc/grpc_util.h"
#include "tensorflow/core/framework/dataset.h"
#include "tensorflow/core/framework/model.h"
#include "tensorflow/core/framework/partial_tensor_shape.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/kernels/data/dataset_utils.h"
#include "tensorflow/core/kernels/data/name_utils.h"
#include "tensorflow/core/kernels/data/serialization_utils.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/gtl/cleanup.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/snappy.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/profiler/lib/traceme.h"
#include "tensorflow/core/profiler/lib/traceme_encode.h"
#include "tensorflow/core/protobuf/error_codes.pb.h"

namespace tensorflow {
namespace data {

/* static */ constexpr const char* const DataServiceDatasetOp::kDatasetType;
/* static */ constexpr const char* const DataServiceDatasetOp::kDatasetId;
/* static */ constexpr const char* const DataServiceDatasetOp::kProcessingMode;
/* static */ constexpr const char* const DataServiceDatasetOp::kAddress;
/* static */ constexpr const char* const DataServiceDatasetOp::kProtocol;
/* static */ constexpr const char* const DataServiceDatasetOp::kJobName;
/* static */ constexpr const char* const DataServiceDatasetOp::kConsumerIndex;
/* static */ constexpr const char* const DataServiceDatasetOp::kNumConsumers;
/* static */ constexpr const char* const
    DataServiceDatasetOp::kMaxOutstandingRequests;
/* static */ constexpr const char* const
    DataServiceDatasetOp::kTaskRefreshIntervalHintMs;
/* static */ constexpr const char* const
    DataServiceDatasetOp::kIterationCounter;
/* static */ constexpr const char* const DataServiceDatasetOp::kOutputTypes;
/* static */ constexpr const char* const DataServiceDatasetOp::kOutputShapes;

namespace {
// Default interval between task list refreshes.
const int64 kDefaultTaskRefreshIntervalMs = 1000;  // 1 second.

constexpr char kDataServiceDatasetV1[] = "DataServiceDataset";
constexpr char kDataServiceDatasetV2[] = "DataServiceDatasetV2";
}  // namespace

// Dataset for reading data from the tf.data service non-deterministically.
//
// This dataset interleaves dataset elements produced by multiple tf.data
// workers. We periodically query the dispatcher to determine which workers
// to read from (in case workers are added or removed).
class DataServiceDatasetOp::Dataset : public DatasetBase {
 public:
  Dataset(OpKernelContext* ctx, int op_version, int64 dataset_id,
          ProcessingMode processing_mode, const std::string& address,
          const std::string& protocol, const std::string& job_name,
          absl::optional<int64> consumer_index,
          absl::optional<int64> num_consumers, int64 max_outstanding_requests,
          int64 task_refresh_interval_ms, IterationCounter* iteration_counter,
          bool owns_resource, ResourceHandle iteration_counter_handle,
          const DataTypeVector& output_types,
          const std::vector<PartialTensorShape>& output_shapes)
      : DatasetBase(DatasetContext(ctx)),
        op_version_(op_version),
        dataset_id_(dataset_id),
        processing_mode_(processing_mode),
        address_(address),
        protocol_(protocol),
        job_name_(job_name),
        consumer_index_(consumer_index),
        num_consumers_(num_consumers),
        max_outstanding_requests_(max_outstanding_requests),
        task_refresh_interval_ms_(task_refresh_interval_ms),
        iteration_counter_(iteration_counter),
        owns_resource_(owns_resource),
        iteration_counter_handle_(iteration_counter_handle),
        resource_mgr_(ctx->resource_manager()),
        output_types_(output_types),
        output_shapes_(output_shapes) {}

  ~Dataset() override {
    iteration_counter_->Unref();
    if (owns_resource_) {
      Status s = resource_mgr_->Delete<IterationCounter>(
          iteration_counter_handle_.container(),
          iteration_counter_handle_.name());
      if (!s.ok()) {
        LOG(WARNING) << "Failed to delete iteration counter resource: " << s;
      }
    }
  }

  std::unique_ptr<IteratorBase> MakeIteratorInternal(
      const string& prefix) const override {
    return absl::make_unique<Iterator>(
        Iterator::Params{this,
                         name_utils::IteratorPrefix(kDatasetType, prefix)},
        iteration_counter_->GetAndIncrement());
  }

  const DataTypeVector& output_dtypes() const override { return output_types_; }

  const std::vector<PartialTensorShape>& output_shapes() const override {
    return output_shapes_;
  }

  string DebugString() const override {
    return name_utils::DatasetDebugString(kDatasetType);
  }

  Status CheckExternalState() const override {
    return Status(
        error::FAILED_PRECONDITION,
        strings::StrCat(DebugString(), " does not yet support serialization."));
  }

 protected:
  Status AsGraphDefInternal(SerializationContext* ctx,
                            DatasetGraphDefBuilder* b,
                            Node** output) const override {
    std::vector<Node*> inputs;

    Node* dataset_id;
    TF_RETURN_IF_ERROR(b->AddScalar(dataset_id_, &dataset_id));
    inputs.push_back(dataset_id);

    Node* processing_mode;
    tstring processing_mode_str = ProcessingModeToString(processing_mode_);
    TF_RETURN_IF_ERROR(b->AddScalar(processing_mode_str, &processing_mode));
    inputs.push_back(processing_mode);

    Node* address;
    TF_RETURN_IF_ERROR(b->AddScalar(address_, &address));
    inputs.push_back(address);

    Node* protocol;
    TF_RETURN_IF_ERROR(b->AddScalar(protocol_, &protocol));
    inputs.push_back(protocol);

    Node* job_name;
    TF_RETURN_IF_ERROR(b->AddScalar(job_name_, &job_name));
    inputs.push_back(job_name);

    if (op_version_ == 2) {
      Node* consumer_index;
      TF_RETURN_IF_ERROR(
          b->AddScalar(consumer_index_.value_or(-1), &consumer_index));
      inputs.push_back(consumer_index);

      Node* num_consumers;
      TF_RETURN_IF_ERROR(
          b->AddScalar(num_consumers_.value_or(-1), &num_consumers));
      inputs.push_back(num_consumers);
    }

    Node* max_outstanding_requests;
    TF_RETURN_IF_ERROR(
        b->AddScalar(max_outstanding_requests_, &max_outstanding_requests));
    inputs.push_back(max_outstanding_requests);

    Node* iteration_counter_handle = nullptr;
    Tensor handle(DT_RESOURCE, TensorShape({}));
    handle.scalar<ResourceHandle>()() = iteration_counter_handle_;
    TF_RETURN_IF_ERROR(b->AddTensor(handle, &iteration_counter_handle));
    inputs.push_back(iteration_counter_handle);

    AttrValue task_refresh_interval_hint_ms;
    b->BuildAttrValue(task_refresh_interval_ms_,
                      &task_refresh_interval_hint_ms);

    TF_RETURN_IF_ERROR(
        b->AddDataset(this, inputs,
                      {std::make_pair(kTaskRefreshIntervalHintMs,
                                      task_refresh_interval_hint_ms)},
                      output));
    return Status::OK();
  }

 private:
  class Iterator : public DatasetIterator<Dataset> {
   public:
    explicit Iterator(const Params& params, int64 iterator_index)
        : DatasetIterator<Dataset>(params),
          iterator_index_(iterator_index),
          max_outstanding_requests_(params.dataset->max_outstanding_requests_) {
    }

    ~Iterator() override {
      VLOG(1) << "Destroying data service dataset iterator for job id "
              << job_client_id_;
      CancelThreads();
      if (deregister_fn_) deregister_fn_();
      task_thread_manager_.reset();
      if (initialized_) {
        Status s = dispatcher_->ReleaseJobClient(job_client_id_);
        if (!s.ok()) {
          LOG(WARNING) << "Failed to release job client id: " << s;
        }
      }
      for (auto& worker_thread : worker_threads_) {
        worker_thread.reset();
      }

      VLOG(1) << "Destroyed data service dataset iterator for job id "
              << job_client_id_;
    }

    void CancelThreads() TF_LOCKS_EXCLUDED(mu_) {
      mutex_lock l(mu_);
      cancelled_ = true;
      worker_thread_cv_.notify_all();
      manager_thread_cv_.notify_all();
      get_next_cv_.notify_all();
    }

    Status Initialize(IteratorContext* ctx) override {
      VLOG(3) << "Connecting to " << dataset()->address_
              << " in data service dataset op";
      TF_RETURN_IF_ERROR(RegisterCancellationCallback(
          ctx->cancellation_manager(), [this]() { CancelThreads(); },
          &deregister_fn_));
      dispatcher_ = absl::make_unique<DataServiceDispatcherClient>(
          dataset()->address_, dataset()->protocol_);
      int64 deadline_micros = kint64max;
      absl::optional<JobKey> key;
      if (!dataset()->job_name_.empty()) {
        key.emplace();
        key.value().set_job_name(std::string(dataset()->job_name_));
        key.value().set_job_name_index(iterator_index_);
      }
      TF_RETURN_IF_ERROR(grpc_util::Retry(
          [&]() {
            return dispatcher_->GetOrCreateJob(
                dataset()->dataset_id_, dataset()->processing_mode_, key,
                dataset()->num_consumers_, job_client_id_);
          },
          /*description=*/
          strings::StrCat("get or create job with dispatcher at ",
                          dataset()->address_),
          deadline_micros));
      initialized_ = true;
      VLOG(1) << "Created data service job with id " << job_client_id_;
      return Status::OK();
    }

    Status GetNextInternal(IteratorContext* ctx,
                           std::vector<Tensor>* out_tensors,
                           bool* end_of_sequence) override {
      VLOG(3) << "Calling GetNext in data service dataset op";
      mutex_lock l(mu_);
      if (!task_thread_manager_ && !cancelled_) {
        task_thread_manager_ =
            ctx->StartThread("task-thread-manager", [this, ctx]() {
              TaskThreadManager(absl::make_unique<IteratorContext>(*ctx));
            });
      }

      while ((results_.empty() || !results_.front().ready) &&
             !(job_finished_ && num_running_worker_threads_ == 0) &&
             !cancelled_ && status_.ok()) {
        VLOG(3) << "Blocking in GetNext. results_.size():" << results_.size()
                << " results_.front().ready:"
                << (!results_.empty() && results_.front().ready)
                << " job_finished_:" << job_finished_
                << " num_running_worker_threads_:"
                << num_running_worker_threads_;
        get_next_cv_.wait(l);
      }
      if (cancelled_) {
        VLOG(3) << "Returning from GetNext due to cancellation";
        return errors::Cancelled("Data service iterator was cancelled");
      }
      if (!status_.ok()) {
        VLOG(3) << "Returning from GetNext with error " << status_;
        return status_;
      }
      if (results_.empty()) {
        *end_of_sequence = true;
        VLOG(3) << "Returning from GetNext with end_of_sequence";
        return Status::OK();
      }
      *end_of_sequence = results_.front().end_of_sequence;
      if (!*end_of_sequence) {
        out_tensors->swap(results_.front().element);
      }
      results_.pop();
      worker_thread_cv_.notify_one();

      VLOG(3) << "Returning from GetNext with an element";
      return Status::OK();
    }

   protected:
    std::shared_ptr<model::Node> CreateNode(
        IteratorContext* ctx, model::Node::Args args) const override {
      return model::MakeKnownRatioNode(std::move(args),
                                       /*ratio=*/1);
    }

    Status SaveInternal(SerializationContext* ctx,
                        IteratorStateWriter* writer) override {
      return errors::Unimplemented("SaveInternal is not yet supported");
    }

    Status RestoreInternal(IteratorContext* ctx,
                           IteratorStateReader* reader) override {
      return errors::Unimplemented("RestoreInternal is not yet supported");
    }

    data::TraceMeMetadata GetTraceMeMetadata() const override {
      data::TraceMeMetadata result;
      int64 num_tasks = -1;
      if (mu_.try_lock()) {
        num_tasks = tasks_.size() - finished_tasks_;
        mu_.unlock();
      }
      std::string num_tasks_string =
          (num_tasks == -1)
              ? "unavailable"
              : strings::Printf("%lld", static_cast<long long>(num_tasks));
      result.push_back(std::make_pair("num_tasks", num_tasks_string));
      result.push_back(std::make_pair("job_name", dataset()->job_name_));
      result.push_back(std::make_pair(
          "max_outstanding_requests",
          strings::Printf("%lld", static_cast<long long>(
                                      dataset()->max_outstanding_requests_))));
      return result;
    }

   private:
    struct Task {
      Task(int64 task_id, const std::string& address,
           std::unique_ptr<DataServiceWorkerClient> worker)
          : task_id(task_id), address(address), worker(std::move(worker)) {}

      const int64 task_id;
      // Address of the tf.data service worker for task `task_id`.
      const std::string address;
      // Client for fetching task elements from the tf.data service worker.
      const std::unique_ptr<DataServiceWorkerClient> worker;
      // Number of elements read by the task.
      int64 elements_read = 0;
      // Indicates whether a worker thread is currently processing the task.
      bool in_use TF_GUARDED_BY(&Iterator::mu_) = false;
      // Indicates whether the worker has returned end_of_sequence for the task.
      bool end_of_sequence TF_GUARDED_BY(&Iterator::mu_) = false;
    };

    struct Result {
      // Whether the result has been computed yet. GetNext needs to block
      // until the next result is ready.
      bool ready TF_GUARDED_BY(&Iterator::mu_) = false;
      std::vector<Tensor> element TF_GUARDED_BY(&Iterator::mu_);
      bool end_of_sequence TF_GUARDED_BY(&Iterator::mu_) = false;
    };

    // Periodically refresh the task list.
    // Maintain one thread fetching elements for each task.
    // TODO(aaudibert): Instead of polling, have dispatcher send updates when
    // the list of tasks changes.
    void TaskThreadManager(std::unique_ptr<IteratorContext> ctx) {
      auto cleanup =
          gtl::MakeCleanup([] { VLOG(1) << "Task thread manager exiting"; });
      VLOG(1) << "Starting task thread manager";
      uint64 next_check = Env::Default()->NowMicros();
      while (true) {
        {
          mutex_lock l(mu_);
          // All units are microseconds.
          while (!cancelled_ && Env::Default()->NowMicros() < next_check) {
            int64 remaining_time = next_check - Env::Default()->NowMicros();
            VLOG(4) << "Task thread manager waiting for " << remaining_time
                    << "us";
            manager_thread_cv_.wait_for(
                l, std::chrono::microseconds(remaining_time));
          }
          if (cancelled_) {
            VLOG(3) << "Task thread manager finished";
            return;
          }
        }
        UpdateTasks();
        UpdateWorkerThreads(ctx.get());
        next_check = Env::Default()->NowMicros() +
                     dataset()->task_refresh_interval_ms_ * 1000;
      }
    }

    void UpdateTasks() TF_LOCKS_EXCLUDED(mu_) {
      VLOG(4) << "Updating tasks";
      std::vector<TaskInfo> tasks;
      bool job_finished;
      Status s = dispatcher_->GetTasks(job_client_id_, tasks, job_finished);
      if (!s.ok()) {
        LOG(WARNING) << "Failed to get task info for job client id "
                     << job_client_id_ << ": " << s;
        return;
      }
      absl::flat_hash_map<int64, TaskInfo> task_id_to_task;
      for (auto& task : tasks) {
        task_id_to_task[task.task_id()] = task;
      }
      mutex_lock l(mu_);
      job_finished_ = job_finished;
      if (job_finished) {
        get_next_cv_.notify_all();
        worker_thread_cv_.notify_all();
        return;
      }
      for (int i = 0; i < tasks_.size(); ++i) {
        std::shared_ptr<Task> task = tasks_[i];
        if (task_id_to_task.contains(task->task_id)) {
          // Remove already-known tasks from `task_id_to_task`, so that at the
          // end of the loop, only new tasks remain.
          task_id_to_task.erase(task->task_id);
        } else {
          // Task has been removed.
          if (task->end_of_sequence) {
            finished_tasks_--;
          }
          tasks_[i] = tasks_[tasks_.size() - 1];
          tasks_.pop_back();
        }
      }
      for (auto& task : tasks) {
        auto it = task_id_to_task.find(task.task_id());
        if (it == task_id_to_task.end()) {
          continue;
        }
        TaskInfo& task_info = it->second;
        std::unique_ptr<DataServiceWorkerClient> worker;
        Status s = CreateDataServiceWorkerClient(task_info.worker_address(),
                                                 dataset()->protocol_, worker);
        if (!s.ok()) {
          status_ = s;
          get_next_cv_.notify_all();
          continue;
        }
        tasks_.push_back(std::make_shared<Task>(task_info.task_id(),
                                                task_info.worker_address(),
                                                std::move(worker)));
      }
      if (dataset()->max_outstanding_requests_ == model::kAutotune) {
        // Adjust max_outstanding_requests to account for newly added tasks.
        max_outstanding_requests_ = tasks_.size();
      }
    }

    void UpdateWorkerThreads(IteratorContext* ctx) TF_LOCKS_EXCLUDED(mu_) {
      mutex_lock l(mu_);
      while (num_running_worker_threads_ < max_outstanding_requests_) {
        num_running_worker_threads_++;
        outstanding_requests_++;
        auto done = [this]() {
          mutex_lock l(mu_);
          num_running_worker_threads_--;
          outstanding_requests_--;
          get_next_cv_.notify_all();
        };
        worker_threads_.push_back(ctx->StartThread(
            "tf-data-service-task_thread", [this, done = std::move(done)]() {
              RunWorkerThread(std::move(done));
            }));
      }
    }

    void RunWorkerThread(std::function<void()> done) {
      auto cleanup = gtl::MakeCleanup([done = std::move(done)]() {
        done();
        VLOG(1) << "Worker thread exiting";
      });
      VLOG(1) << "Starting worker thread";
      std::shared_ptr<Task> task_to_process;
      while (true) {
        Result* result;
        {
          mutex_lock l(mu_);
          if (task_to_process) {
            task_to_process->in_use = false;
            task_to_process = nullptr;
            worker_thread_cv_.notify_one();
          }
          outstanding_requests_--;
          while (!cancelled_ && !(ElementSpaceAvailable() && TaskAvailable()) &&
                 !job_finished_) {
            if (VLOG_IS_ON(3)) {
              VLOG(3) << "Sleeping with results_.size=" << results_.size()
                      << ", outstanding_requests_=" << outstanding_requests_
                      << ", max_oustanding_requests="
                      << max_outstanding_requests_
                      << " finished_tasks=" << finished_tasks_
                      << " tasks_.size()=" << tasks_.size();
            }
            worker_thread_cv_.wait(l);
          }
          outstanding_requests_++;
          if (cancelled_ || job_finished_) {
            return;
          }
          if (StrictRoundRobin()) {
            task_to_process = tasks_[next_task_index_];
            // Reserve a spot in the results_ queue.
            results_.emplace();
            result = &results_.back();
            next_task_index_ = (next_task_index_ + 1) % tasks_.size();
            DCHECK(!task_to_process->in_use);
          } else {
            // Search for a task to update.
            int num_tasks = tasks_.size();
            for (int i = 0; i < num_tasks; ++i) {
              int index = (next_task_index_ + i) % num_tasks;
              std::shared_ptr<Task>& task = tasks_[index];
              if (!task->in_use && !task->end_of_sequence) {
                task_to_process = task;
                next_task_index_ = (index + 1) % num_tasks;
                break;
              }
            }
          }
          DCHECK(task_to_process != nullptr);
          task_to_process->in_use = true;
          VLOG(3) << "Processing task " << task_to_process->task_id;
        }
        int64 deadline_micros = kint64max;
        Status s;
        if (StrictRoundRobin()) {
          s = GetElement(task_to_process.get(), deadline_micros,
                         /*enqueue_result=*/false, *result);
        } else {
          Result r;
          s = GetElement(task_to_process.get(), deadline_micros,
                         /*enqueue_result=*/true, r);
        }
        if (!s.ok()) {
          mutex_lock l(mu_);
          VLOG(1) << "Failed to get element from worker "
                  << task_to_process->address << ": " << s;
          task_to_process->in_use = false;
          status_ = Status(
              s.code(),
              absl::StrCat("Failed to get element from worker ",
                           task_to_process->address, ": ", s.error_message()));
          get_next_cv_.notify_all();
          return;
        }
      }
    }

    // Gets an element from a task and stores the element in `result`. If
    // `enqueue_result` is true, `GetElement` also enqueues (via std::move) any
    // element-producing result in the `results_` queue.
    Status GetElement(Task* task, int64 deadline_micros, bool enqueue_result,
                      Result& result) TF_LOCKS_EXCLUDED(mu_) {
      VLOG(3) << "Getting an element for task id " << task->task_id;
      tensorflow::profiler::TraceMe activity(
          "GetDataServiceElement", tensorflow::profiler::TraceMeLevel::kInfo);
      activity.AppendMetadata([&]() {
        return profiler::TraceMeEncode({{"address", task->address}});
      });
      CompressedElement compressed;
      bool end_of_sequence;
      for (int num_retries = 0;; ++num_retries) {
        absl::optional<int64> consumer_index = dataset()->consumer_index_;
        absl::optional<int64> round_index;
        if (StrictRoundRobin()) {
          round_index = task->elements_read;
          VLOG(3) << "Requesting element from consumer index "
                  << consumer_index.value() << ", round "
                  << round_index.value();
          activity.AppendMetadata([&]() {
            return profiler::TraceMeEncode(
                {{"consumer_index", consumer_index.value()},
                 {"round_index", round_index.value()}});
          });
        }
        Status s =
            task->worker->GetElement(task->task_id, consumer_index, round_index,
                                     compressed, end_of_sequence);
        if (s.ok()) {
          break;
        }
        // Retry all errors that could indicate preemption.
        if (!errors::IsUnavailable(s) && !errors::IsCancelled(s) &&
            !errors::IsAborted(s)) {
          return s;
        }
        {
          mutex_lock l(mu_);
          // If `UpdateTaskThreads` finds that the task has been cancelled, it
          // will set end_of_sequence to `true`.
          if (task->end_of_sequence || cancelled_) {
            end_of_sequence = true;
            break;
          }
        }
        const int64 now_micros = EnvTime::NowMicros();
        if (now_micros > deadline_micros) {
          return s;
        }
        const int64 deadline_with_backoff_micros =
            now_micros + ::tensorflow::ComputeBackoffMicroseconds(num_retries);
        // Wait for a short period of time before retrying the RPC. If our
        // backoff would put us past the RPC deadline, we truncate it to ensure
        // our RPC starts before the deadline.
        const auto backoff_until =
            (deadline_micros > deadline_with_backoff_micros)
                ? deadline_with_backoff_micros
                : deadline_micros;
        VLOG(1) << "Failed to get an element from worker " << task->address
                << ": " << s << ". Will retry in "
                << (backoff_until - now_micros) << " microseconds";
        Env::Default()->SleepForMicroseconds(backoff_until - now_micros);
      }

      std::vector<Tensor> element;
      if (!end_of_sequence) {
        Tensor tensor(DT_VARIANT, TensorShape{});
        tensor.scalar<Variant>()() = std::move(compressed);
        element.push_back(tensor);
      }
      mutex_lock l(mu_);
      result.ready = true;
      result.end_of_sequence = end_of_sequence;
      if (end_of_sequence) {
        task->end_of_sequence = true;
        finished_tasks_++;
        return Status::OK();
      }
      task->elements_read++;
      result.element = std::move(element);
      if (enqueue_result) {
        results_.push(std::move(result));
      }
      get_next_cv_.notify_all();
      VLOG(3) << "Got an element for task id " << task->task_id;
      return Status::OK();
    }

    // Reports whether we can request another element without violating
    // max_outstanding_requests.
    bool ElementSpaceAvailable() TF_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
      // When doing round-robin reads, outstanding requests pre-allocate a
      // result in `results_`, so we only need to check the size of `results_`.
      if (StrictRoundRobin()) {
        return results_.size() < max_outstanding_requests_;
      }
      // Otherwise, results aren't added to `results_` until the data has been
      // successfully retrieved. We need to count requests already added to
      // `results_` as well as in-progress requests.
      return results_.size() + outstanding_requests_ <
             max_outstanding_requests_;
    }

    bool TaskAvailable() TF_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
      if (StrictRoundRobin()) {
        return !tasks_[next_task_index_]->in_use;
      }
      return finished_tasks_ + outstanding_requests_ < tasks_.size();
    }

    bool StrictRoundRobin() { return dataset()->num_consumers_.has_value(); }

    const int64 iterator_index_;

    mutable mutex mu_;
    condition_variable get_next_cv_ TF_GUARDED_BY(mu_);
    condition_variable worker_thread_cv_ TF_GUARDED_BY(mu_);
    condition_variable manager_thread_cv_ TF_GUARDED_BY(mu_);
    bool cancelled_ TF_GUARDED_BY(mu_) = false;
    // Method for deregistering the cancellation callback.
    std::function<void()> deregister_fn_;

    int64 outstanding_requests_ TF_GUARDED_BY(mu_) = 0;
    // max_outstanding_requests controls how many elements may be held in memory
    // at the same time. This count includes both in-progress requests for
    // elements as well as completed requests which haven't yet been produced.
    int64 max_outstanding_requests_ TF_GUARDED_BY(mu_);

    // The number of threads in `worker_threads_` which are still running.
    int64 num_running_worker_threads_ TF_GUARDED_BY(mu_) = 0;

    // The index of the next task in `tasks_` to read from.
    int64 next_task_index_ TF_GUARDED_BY(mu_) = 0;

    // The number tasks in the `tasks_` list that have reached end_of_sequence.
    int64 finished_tasks_ TF_GUARDED_BY(mu_) = 0;

    // List of tasks to read from.
    std::vector<std::shared_ptr<Task>> tasks_ TF_GUARDED_BY(mu_);

    // A status to be returned from the next call to `GetNext`. This is set by
    // asynchronous threads when they encounter errors.
    Status status_ TF_GUARDED_BY(mu_) = Status::OK();
    // A queue of results for `GetElement` requests to read from. When doing
    // strict round robin reads, the queue will contain placeholder results with
    // their `Result::ready` field false until their data has been retrieved
    // from a worker. When not doing round-robin reads, results are only added
    // to the queue after they are ready, to avoid head-of-line blocking.
    std::queue<Result> results_ TF_GUARDED_BY(mu_);

    bool initialized_ = false;
    // Set once in Initialize().
    int64 job_client_id_;
    std::unique_ptr<DataServiceDispatcherClient> dispatcher_;

    bool job_finished_ = false;
    std::vector<std::unique_ptr<Thread>> worker_threads_ TF_GUARDED_BY(mu_);
    std::unique_ptr<Thread> task_thread_manager_ TF_GUARDED_BY(mu_);
  };

  const int op_version_;
  const int64 dataset_id_;
  const ProcessingMode processing_mode_;
  const tstring address_;
  const tstring protocol_;
  const tstring job_name_;
  const absl::optional<int64> consumer_index_;
  const absl::optional<int64> num_consumers_;
  const int64 max_outstanding_requests_;
  const int64 task_refresh_interval_ms_;
  IterationCounter* const iteration_counter_;  // Owned
  const bool owns_resource_;
  const ResourceHandle iteration_counter_handle_;
  ResourceMgr* const resource_mgr_;  // Not owned
  const DataTypeVector output_types_;
  const std::vector<PartialTensorShape> output_shapes_;
};

DataServiceDatasetOp::DataServiceDatasetOp(OpKernelConstruction* ctx)
    : DatasetOpKernel(ctx) {
  OP_REQUIRES_OK(ctx, ctx->GetAttr(kTaskRefreshIntervalHintMs,
                                   &task_refresh_interval_hint_ms_));
  if (task_refresh_interval_hint_ms_ == model::kAutotune) {
    task_refresh_interval_hint_ms_ = kDefaultTaskRefreshIntervalMs;
  }
  OP_REQUIRES_OK(ctx, ctx->GetAttr(kOutputTypes, &output_types_));
  OP_REQUIRES_OK(ctx, ctx->GetAttr(kOutputShapes, &output_shapes_));
  auto& op_name = ctx->def().op();
  if (op_name == kDataServiceDatasetV1) {
    op_version_ = 1;
  } else if (op_name == kDataServiceDatasetV2) {
    op_version_ = 2;
  } else {
    ctx->CtxFailure(errors::FailedPrecondition(
        "Unrecognized data service dataset op name: ", op_name));
    return;
  }
}

void DataServiceDatasetOp::MakeDataset(OpKernelContext* ctx,
                                       DatasetBase** output) {
  int64 dataset_id;
  OP_REQUIRES_OK(ctx, ParseScalarArgument(ctx, kDatasetId, &dataset_id));

  tstring processing_mode_str;
  OP_REQUIRES_OK(
      ctx, ParseScalarArgument(ctx, kProcessingMode, &processing_mode_str));
  ProcessingMode processing_mode;
  OP_REQUIRES_OK(ctx,
                 ParseProcessingMode(processing_mode_str, processing_mode));

  tstring address;
  OP_REQUIRES_OK(ctx, ParseScalarArgument(ctx, kAddress, &address));
  OP_REQUIRES(ctx, !address.empty(),
              errors::InvalidArgument(kAddress, " must be non-empty."));

  tstring protocol;
  OP_REQUIRES_OK(ctx, ParseScalarArgument(ctx, kProtocol, &protocol));
  OP_REQUIRES(ctx, !protocol.empty(),
              errors::InvalidArgument(kProtocol, " must be non-empty."));

  tstring job_name;
  OP_REQUIRES_OK(ctx, ParseScalarArgument(ctx, kJobName, &job_name));

  absl::optional<int64> consumer_index;
  absl::optional<int64> num_consumers;
  if (op_version_ >= 2) {
    int64 consumer_index_int;
    OP_REQUIRES_OK(
        ctx, ParseScalarArgument(ctx, kConsumerIndex, &consumer_index_int));
    if (consumer_index_int >= 0) {
      consumer_index = consumer_index_int;
    }

    int64 num_consumers_int;
    OP_REQUIRES_OK(ctx,
                   ParseScalarArgument(ctx, kNumConsumers, &num_consumers_int));
    if (num_consumers_int >= 0) {
      num_consumers = num_consumers_int;
    }
  }

  int64 max_outstanding_requests;
  OP_REQUIRES_OK(ctx, ParseScalarArgument(ctx, kMaxOutstandingRequests,
                                          &max_outstanding_requests));

  ResourceHandle iteration_counter_handle;
  OP_REQUIRES_OK(
      ctx, HandleFromInput(ctx, kIterationCounter, &iteration_counter_handle));
  IterationCounter* iteration_counter = nullptr;
  Status s = ctx->resource_manager()->Lookup<IterationCounter>(
      iteration_counter_handle.container(), iteration_counter_handle.name(),
      &iteration_counter);
  bool owns_resource = false;
  if (errors::IsNotFound(s)) {
    owns_resource = true;
    static std::atomic<int64> resource_id_counter(0);
    const std::string& container = ctx->resource_manager()->default_container();
    std::string name =
        strings::StrCat(ctx->op_kernel().name(), "/", kIterationCounter, "_",
                        resource_id_counter.fetch_add(1));
    OP_REQUIRES_OK(ctx,
                   ctx->resource_manager()->LookupOrCreate<IterationCounter>(
                       container, name, &iteration_counter,
                       [](IterationCounter** counter) {
                         *counter = new IterationCounter();
                         return Status::OK();
                       }));
    iteration_counter_handle =
        MakeResourceHandle<IterationCounter>(ctx, container, name);
  } else {
    OP_REQUIRES_OK(ctx, s);
  }

  OP_REQUIRES(
      ctx,
      max_outstanding_requests == model::kAutotune ||
          max_outstanding_requests > 0,
      errors::InvalidArgument(kMaxOutstandingRequests, " must be positive or ",
                              model::kAutotune));

  *output = new Dataset(
      ctx, op_version_, dataset_id, processing_mode, address, protocol,
      job_name, consumer_index, num_consumers, max_outstanding_requests,
      task_refresh_interval_hint_ms_, iteration_counter, owns_resource,
      iteration_counter_handle, output_types_, output_shapes_);
}

REGISTER_KERNEL_BUILDER(Name("DataServiceDataset").Device(DEVICE_CPU),
                        DataServiceDatasetOp);
REGISTER_KERNEL_BUILDER(Name("DataServiceDatasetV2").Device(DEVICE_CPU),
                        DataServiceDatasetOp);
REGISTER_KERNEL_BUILDER(Name("DummyIterationCounter").Device(DEVICE_CPU),
                        DummyResourceOp<IterationCounter>);

}  // namespace data
}  // namespace tensorflow
