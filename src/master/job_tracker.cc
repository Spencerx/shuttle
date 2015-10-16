#include "job_tracker.h"

#include <boost/lexical_cast.hpp>
#include <boost/bind.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/algorithm/string.hpp>
#include <gflags/gflags.h>
#include <algorithm>
#include <iterator>
#include <string>
#include <sstream>
#include <set>

#include "google/protobuf/repeated_field.h"
#include "timer.h"
#include "logging.h"
#include "proto/minion.pb.h"
#include "resource_manager.h"
#include "master_impl.h"

DECLARE_int32(galaxy_deploy_step);
DECLARE_string(minion_path);
DECLARE_int32(timeout_bound);
DECLARE_int32(replica_num);
DECLARE_int32(replica_begin);
DECLARE_int32(retry_bound);
DECLARE_string(nexus_server_list);

namespace baidu {
namespace shuttle {

const std::string shuttle_label = "map_reduce_shuttle";

JobTracker::JobTracker(MasterImpl* master, ::baidu::galaxy::Galaxy* galaxy_sdk,
                       const JobDescriptor& job) :
                      master_(master),
                      sdk_(galaxy_sdk),
                      map_completed_(0),
                      last_map_no_(-1),
                      last_map_attempt_(0),
                      reduce_completed_(0),
                      last_reduce_no_(-1),
                      last_reduce_attempt_(0) {
    char time_chars[32];
    ::baidu::common::timer::now_time_str(time_chars, 32);
    std::string time_str = time_chars;
    boost::replace_all(time_str, " ", "-");
    job_id_ = time_str;
    job_id_ += boost::lexical_cast<std::string>(random());
    job_descriptor_.CopyFrom(job);
    state_ = kPending;
    rpc_client_ = new RpcClient();
    std::vector<std::string> inputs;
    const ::google::protobuf::RepeatedPtrField<std::string>& input_filenames = job.inputs();
    std::copy(input_filenames.begin(), input_filenames.end(), std::back_inserter(inputs));
    map_manager_ = new ResourceManager(inputs);
    job_descriptor_.set_map_total(map_manager_->SumOfItem());
    if (job.job_type() == kMapReduceJob) {
        reduce_manager_ = new BasicManager(job_descriptor_.reduce_total());
    } else {
        reduce_manager_ = NULL;
    }
    monitor_.AddTask(boost::bind(&JobTracker::KeepMonitoring, this));
}

JobTracker::~JobTracker() {
    Kill();
    delete map_manager_;
    delete reduce_manager_;
    delete rpc_client_;
    {
        MutexLock lock(&alloc_mu_);
        for (std::list<AllocateItem*>::iterator it = allocation_table_.begin();
                it != allocation_table_.end(); ++it) {
            delete *it;
        }
    }
}

Status JobTracker::Start() {
    if (job_descriptor_.map_total() < 1) {
        LOG(INFO, "map input may not inexist, failed: %s", job_id_.c_str());
        job_descriptor_.set_reduce_total(0);
        state_ = kFailed;
        return kNoMore;
    }
    ::baidu::galaxy::JobDescription galaxy_job;
    galaxy_job.job_name = job_descriptor_.name() + "_map@minion";
    galaxy_job.type = "kBatch";
    galaxy_job.priority = "kOnline";
    galaxy_job.replica = job_descriptor_.map_capacity();
    galaxy_job.deploy_step = FLAGS_galaxy_deploy_step;
    galaxy_job.pod.requirement.millicores = job_descriptor_.millicores();
    galaxy_job.pod.requirement.memory = job_descriptor_.memory();
    std::stringstream ss;
    ss << "app_package=" << job_descriptor_.files(0) << " ./minion_boot.sh"
       << " -jobid=" << job_id_ << " -nexus_addr=" << FLAGS_nexus_server_list
       << " -work_mode=" << ((job_descriptor_.job_type() == kMapOnlyJob) ? "map-only" : "map");
    ::baidu::galaxy::TaskDescription minion;
    minion.offset = 1;
    minion.binary = FLAGS_minion_path;
    minion.source_type = "kSourceTypeFTP";
    minion.start_cmd = ss.str().c_str();
    minion.requirement = galaxy_job.pod.requirement;
    galaxy_job.pod.tasks.push_back(minion);
    std::string minion_id;
    if (sdk_->SubmitJob(galaxy_job, &minion_id)) {
        LOG(INFO, "start a new map reduce job: %s -> %s",
                job_descriptor_.name().c_str(), minion_id.c_str());
        MutexLock lock(&mu_);
        map_minion_ = minion_id;
        return kOk;
    }
    LOG(WARNING, "galaxy report error when submitting a new job: %s",
            job_descriptor_.name().c_str());
    return kGalaxyError;
}

Status JobTracker::Update(const std::string& priority,
                          int map_capacity,
                          int reduce_capacity) {
    if (!map_minion_.empty()) {
        mu_.Lock();
        ::baidu::galaxy::JobDescription map_desc = map_description_;
        mu_.Unlock();
        map_desc.priority = priority;
        map_desc.replica = map_capacity;
        if (sdk_->UpdateJob(map_minion_, map_desc)) {
            MutexLock lock(&mu_);
            map_description_.priority = priority;
            map_description_.replica = map_capacity;
        } else {
            return kGalaxyError;
        }
    }

    if (!reduce_minion_.empty()) {
        mu_.Lock();
        ::baidu::galaxy::JobDescription reduce_desc = reduce_description_;
        mu_.Unlock();
        reduce_desc.priority = priority;
        reduce_desc.replica = reduce_capacity;
        if (sdk_->UpdateJob(reduce_minion_, reduce_desc)) {
            MutexLock lock(&mu_);
            reduce_description_.priority = priority;
            reduce_description_.replica = reduce_capacity;
        } else {
            return kGalaxyError;
        }
    }
    return kOk;
}

Status JobTracker::Kill() {
    MutexLock lock(&mu_);
    if (!map_minion_.empty() && !sdk_->TerminateJob(map_minion_)) {
        LOG(INFO, "map minion finished, kill: %s", job_id_.c_str());
        return kGalaxyError;
    }
    map_minion_ = "";
    if (!reduce_minion_.empty() && !sdk_->TerminateJob(reduce_minion_)) {
        LOG(INFO, "reduce minion finished, kill: %s", job_id_.c_str());
        return kGalaxyError;
    }
    reduce_minion_ = "";
    if (state_ != kCompleted) {
        state_ = kKilled;
    }
    return kOk;
}

ResourceItem* JobTracker::AssignMap(const std::string& endpoint) {
    if (state_ == kPending) {
        state_ = kRunning;
    }
    ResourceItem* cur = NULL;
    AllocateItem* alloc = new AllocateItem();
    alloc->endpoint = endpoint;
    alloc->state = kTaskRunning;
    // TODO Considered a new duplication management method here
    if (last_map_no_ != -1 && map_manager_->SumOfItem() - last_map_no_ < FLAGS_replica_begin &&
            last_map_attempt_ <= FLAGS_replica_num) {
        cur = map_manager_->GetCertainItem(last_map_no_);
        if (cur == NULL) {
            delete alloc;
            LOG(INFO, "assign map: no more: %s", job_id_.c_str());
            return NULL;
        }
        alloc->resource_no = last_map_no_;
    } else {
        cur = map_manager_->GetItem();
        if (cur == NULL) {
            delete alloc;
            LOG(INFO, "assign map: no more: %s", job_id_.c_str());
            return NULL;
        }
        alloc->resource_no = cur->no;
    }
    alloc->attempt = cur->attempt;
    alloc->state = kTaskRunning;
    alloc->is_map = true;
    alloc->alloc_time = std::time(NULL);
    last_map_no_ = cur->no;
    last_map_attempt_ = cur->attempt;

    int attempt_bound = FLAGS_retry_bound;
    if (map_manager_->SumOfItem() - cur->no < FLAGS_replica_begin) {
        attempt_bound += FLAGS_replica_num;
    }
    if (cur->attempt > attempt_bound) {
        LOG(INFO, "map failed, kill job: %s", job_id_.c_str());
        master_->RetractJob(job_id_);
        state_ = kFailed;
        delete alloc;
        return NULL;
    }
    MutexLock lock(&alloc_mu_);
    allocation_table_.push_back(alloc);
    time_heap_.push(alloc);
    LOG(INFO, "assign map: < no - %d, attempt - %d >, to %s: %s",
            alloc->resource_no, alloc->attempt, endpoint.c_str(), job_id_.c_str());
    return cur;
}

IdItem* JobTracker::AssignReduce(const std::string& endpoint) {
    if (state_ == kPending) {
        state_ = kRunning;
    }
    IdItem* cur = NULL;
    AllocateItem* alloc = new AllocateItem();
    alloc->endpoint = endpoint;
    alloc->state = kTaskRunning;
    if (last_reduce_no_ != -1 && reduce_manager_->SumOfItem() - last_reduce_no_ < FLAGS_replica_begin
            && last_reduce_attempt_ <= FLAGS_replica_num) {
        cur = reduce_manager_->GetCertainItem(last_reduce_no_);
        if (cur == NULL) {
            delete alloc;
            LOG(INFO, "assign reduce: no more: %s", job_id_.c_str());
            return NULL;
        }
        alloc->resource_no = last_reduce_no_;
    } else {
        cur = reduce_manager_->GetItem();
        if (cur == NULL) {
            delete alloc;
            LOG(INFO, "assign reduce: no more: %s", job_id_.c_str());
            return NULL;
        }
        alloc->resource_no = cur->no;
    }
    alloc->attempt = cur->attempt;
    alloc->state = kTaskRunning;
    alloc->is_map = false;
    alloc->alloc_time = std::time(NULL);
    last_reduce_no_ = cur->no;
    last_reduce_attempt_ = cur->attempt;

    int attempt_bound = FLAGS_retry_bound;
    if (reduce_manager_->SumOfItem() - cur->no < FLAGS_replica_begin) {
        attempt_bound += FLAGS_replica_num;
    }
    if (cur->attempt > attempt_bound) {
        LOG(INFO, "reduce failed, kill job: %s", job_id_.c_str());
        master_->RetractJob(job_id_);
        state_ = kFailed;
        delete alloc;
        return NULL;
    }
    MutexLock lock(&alloc_mu_);
    allocation_table_.push_back(alloc);
    time_heap_.push(alloc);
    LOG(INFO, "assign reduce: < no - %d, attempt - %d >, to %s: %s",
            alloc->resource_no, alloc->attempt, endpoint.c_str(), job_id_.c_str());
    return cur;
}

Status JobTracker::FinishMap(int no, int attempt, TaskState state) {
    AllocateItem* cur = NULL;
    {
        MutexLock lock(&alloc_mu_);
        std::list<AllocateItem*>::iterator it;
        for (it = allocation_table_.begin(); it != allocation_table_.end(); ++it) {
            if ((*it)->resource_no == no && (*it)->attempt == attempt) {
                if ((*it)->state == kTaskRunning) {
                    cur = *it;
                }
                break;
            }
        }
    }
    if (cur == NULL) {
        LOG(WARNING, "try to finish an inexist map task: < no - %d, attempt - %d >: %s",
                no, attempt, job_id_.c_str());
        return kNoMore;
    }
    LOG(INFO, "finish a map task: < no - %d, attempt - %d >, state %s: %s",
            cur->resource_no, cur->attempt, TaskState_Name(state).c_str(), job_id_.c_str());
    cur->state = state;
    {
        MutexLock lock(&mu_);
        // TODO Statistics do not work when minion failed
        switch (state) {
        case kTaskCompleted:
            map_completed_ ++;
            LOG(INFO, "complete a map task(%d/%d): %s",
                    map_completed_, map_manager_->SumOfItem(), job_id_.c_str());
            if (map_completed_ == map_manager_->SumOfItem()) {
                if (job_descriptor_.job_type() == kMapOnlyJob) {
                    LOG(INFO, "map-only job finish: %s", job_id_.c_str());
                    mu_.Unlock();
                    master_->RetractJob(job_id_);
                    mu_.Lock();
                    state_ = kCompleted;
                } else {
                    // Pull up reduce task
                    LOG(INFO, "map phrase end now, pull up reduce tasks: %s", job_id_.c_str());
                    ::baidu::galaxy::JobDescription galaxy_job;
                    galaxy_job.job_name = job_descriptor_.name() + "_reduce@minion";
                    galaxy_job.type = "kBatch";
                    galaxy_job.priority = "kOnline";
                    galaxy_job.replica = job_descriptor_.reduce_capacity();
                    galaxy_job.deploy_step = FLAGS_galaxy_deploy_step;
                    galaxy_job.pod.requirement.millicores = job_descriptor_.millicores();
                    galaxy_job.pod.requirement.memory = job_descriptor_.memory();
                    std::stringstream ss;
                    ss << "app_package=" << job_descriptor_.files(0) << " ./minion_boot.sh"
                       << " -jobid=" << job_id_ << " -nexus_addr=" << FLAGS_nexus_server_list
                       << " -work_mode=" << "reduce";
                    ::baidu::galaxy::TaskDescription minion;
                    minion.offset = 1;
                    minion.binary = FLAGS_minion_path;
                    minion.source_type = "kSourceTypeFTP";
                    minion.start_cmd = ss.str().c_str();
                    minion.requirement = galaxy_job.pod.requirement;
                    galaxy_job.pod.tasks.push_back(minion);
                    std::string minion_id;
                    if (sdk_->SubmitJob(galaxy_job, &minion_id)) {
                        reduce_minion_ = minion_id;
                    }
                    if (!map_minion_.empty()) {
                        LOG(INFO, "map minion finished, kill: %s", job_id_.c_str());
                        sdk_->TerminateJob(map_minion_);
                        map_minion_ = "";
                    }
                }
            }
            break;
        case kTaskFailed:
            map_manager_->ReturnBackItem(cur->resource_no);
            break;
        case kKilled: break;
        case kTaskCanceled: break; // TODO Think carefully
        default: LOG(WARNING, "unfamiliar task finish status: %d", cur->state);
        }
    }
    if (state != kTaskCompleted) {
        return kOk;
    }
    Minion_Stub* stub = NULL;
    CancelTaskRequest request;
    CancelTaskResponse response;
    request.set_job_id(job_id_);
    MutexLock lock(&alloc_mu_);
    for (std::list<AllocateItem*>::iterator it = allocation_table_.begin();
            it != allocation_table_.end(); ++it) {
        if ((*it)->resource_no == no && (*it)->attempt != attempt) {
            rpc_client_->GetStub((*it)->endpoint, &stub);
            boost::scoped_ptr<Minion_Stub> stub_guard(stub);
            request.set_task_id((*it)->resource_no);
            request.set_attempt_id((*it)->attempt);
            bool ok = rpc_client_->SendRequest(stub, &Minion_Stub::CancelTask,
                                               &request, &response, 2, 1);
            if (!ok) {
                LOG(WARNING, "failed to rpc: %s", (*it)->endpoint.c_str());
            }
            // TODO Maybe check returned state here?
            (*it)->state = kTaskCanceled;
        }
    }
    return kOk;
}

Status JobTracker::FinishReduce(int no, int attempt, TaskState state) {
    AllocateItem* cur = NULL;
    {
        MutexLock lock(&alloc_mu_);
        std::list<AllocateItem*>::iterator it;
        for (it = allocation_table_.begin(); it != allocation_table_.end(); ++it) {
            if (!((*it)->is_map) && (*it)->resource_no == no && (*it)->attempt == attempt) {
                if ((*it)->state == kTaskRunning) {
                    cur = *it;
                }
                break;
            }
        }
    }
    if (cur == NULL) {
        LOG(WARNING, "try to finish an inexist reduce task: < no - %d, attempt - %d >: %s",
                no, attempt, job_id_.c_str());
        return kNoMore;
    }
    LOG(INFO, "finish a reduce task: < no - %d, attempt - %d >, state %s: %s",
            cur->resource_no, cur->attempt, TaskState_Name(state).c_str(), job_id_.c_str());
    cur->state = state;
    {
        MutexLock lock(&mu_);
        switch (state) {
        case kTaskCompleted:
            reduce_completed_ ++;
            LOG(INFO, "complete a reduce task(%d/%d): %s",
                    reduce_completed_, reduce_manager_->SumOfItem(), job_id_.c_str());
            if (reduce_completed_ == reduce_manager_->SumOfItem()) {
                LOG(INFO, "map-reduce job finish: %s", job_id_.c_str());
                mu_.Unlock();
                master_->RetractJob(job_id_);
                mu_.Lock();
                state_ = kCompleted;
            }
            break;
        case kTaskFailed:
            reduce_manager_->ReturnBackItem(cur->resource_no);
            break;
        case kKilled: break;
        case kTaskCanceled: break; // TODO Think carefully
        default: LOG(WARNING, "unfamiliar task finish status: %d", cur->state);
        }
    }
    if (state != kTaskCompleted) {
        return kOk;
    }
    Minion_Stub* stub = NULL;
    CancelTaskRequest request;
    CancelTaskResponse response;
    request.set_job_id(job_id_);
    MutexLock lock(&alloc_mu_);
    for (std::list<AllocateItem*>::iterator it = allocation_table_.begin();
            it != allocation_table_.end(); ++it) {
        if (!((*it)->is_map) && (*it)->resource_no == no && (*it)->attempt != attempt) {
            rpc_client_->GetStub((*it)->endpoint, &stub);
            boost::scoped_ptr<Minion_Stub> stub_guard(stub);
            request.set_task_id((*it)->resource_no);
            request.set_attempt_id((*it)->attempt);
            bool ok = rpc_client_->SendRequest(stub, &Minion_Stub::CancelTask,
                                               &request, &response, 2, 1);
            if (!ok) {
                LOG(WARNING, "failed to rpc: %s", (*it)->endpoint.c_str());
            }
            // TODO Maybe check returned state here?
            (*it)->state = kTaskCanceled;
        }
    }
    return kOk;
}

TaskStatistics JobTracker::GetMapStatistics() {
    std::set<int> map_blocks;
    int running = 0, failed = 0, killed = 0;
    {
        MutexLock lock(&alloc_mu_);
        for (std::list<AllocateItem*>::iterator it = allocation_table_.begin();
                it != allocation_table_.end(); ++it) {
            AllocateItem* cur = *it;
            if (!cur->is_map) {
                continue;
            }
            switch (cur->state) {
            case kTaskRunning:
                if (map_blocks.find(cur->resource_no) != map_blocks.end()) {
                    continue;
                }
                ++ running; break;
            case kTaskFailed: ++ failed; break;
            case kTaskKilled: ++ killed; break;
            default: break;
            }
            map_blocks.insert(cur->resource_no);
        }
    }
    MutexLock lock(&mu_);
    TaskStatistics task;
    task.set_total(map_manager_->SumOfItem());
    task.set_pending(task.total() - running - map_completed_);
    task.set_running(running);
    task.set_failed(failed);
    task.set_killed(killed);
    task.set_completed(map_completed_);
    return task;
}

TaskStatistics JobTracker::GetReduceStatistics() {
    if (reduce_manager_ == NULL) {
        return TaskStatistics();
    }
    std::set<int> reduce_blocks;
    int running = 0, failed = 0, killed = 0;
    {
        MutexLock lock(&alloc_mu_);
        for (std::list<AllocateItem*>::iterator it = allocation_table_.begin();
                it != allocation_table_.end(); ++it) {
            AllocateItem* cur = *it;
            if (cur->is_map) {
                continue;
            }
            switch (cur->state) {
            case kTaskRunning:
                if (reduce_blocks.find(cur->resource_no) != reduce_blocks.end()) {
                    continue;
                }
                ++ running; break;
            case kTaskFailed: ++ failed; break;
            case kTaskKilled: ++ killed; break;
            default: break;
            }
            reduce_blocks.insert(cur->resource_no);
        }
    }
    MutexLock lock(&mu_);
    TaskStatistics task;
    task.set_total(reduce_manager_->SumOfItem());
    task.set_pending(task.total() - running - reduce_completed_);
    task.set_running(running);
    task.set_failed(failed);
    task.set_killed(killed);
    task.set_completed(reduce_completed_);
    return task;
}

void JobTracker::KeepMonitoring() {
    time_t now = std::time(NULL);
    while (!time_heap_.empty()) {
        alloc_mu_.Lock();
        AllocateItem* top = time_heap_.top();
        if (now - top->alloc_time < FLAGS_timeout_bound) {
            alloc_mu_.Unlock();
            break;
        }
        time_heap_.pop();
        alloc_mu_.Unlock();
        if (top->state != kTaskRunning) {
            continue;
        }
        if (top->is_map) {
            map_manager_->ReturnBackItem(top->resource_no);
        } else {
            reduce_manager_->ReturnBackItem(top->resource_no);
        }
        Minion_Stub* stub = NULL;
        rpc_client_->GetStub(top->endpoint, &stub);
        boost::scoped_ptr<Minion_Stub> stub_guard(stub);
        CancelTaskRequest request;
        CancelTaskResponse response;
        request.set_job_id(job_id_);
        request.set_task_id(top->resource_no);
        request.set_attempt_id(top->attempt);
        bool ok = rpc_client_->SendRequest(stub, &Minion_Stub::CancelTask,
                                           &request, &response, 2, 1);
        if (!ok) {
            LOG(WARNING, "failed to rpc: %s", top->endpoint.c_str());
        }
        // TODO Maybe check returned state here?
    }
    monitor_.DelayTask(FLAGS_timeout_bound * 1000,
                       boost::bind(&JobTracker::KeepMonitoring, this));
}

}
}

