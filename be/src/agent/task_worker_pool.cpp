// Copyright (c) 2017, Baidu.com, Inc. All Rights Reserved

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "agent/task_worker_pool.h"
#include <pthread.h>
#include <sys/stat.h>
#include <atomic>
#include <csignal>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include "boost/filesystem.hpp"
#include "boost/lexical_cast.hpp"
#include "agent/pusher.h"
#include "agent/status.h"
#include "agent/utils.h"
#include "gen_cpp/FrontendService.h"
#include "gen_cpp/Types_types.h"
#include "olap/olap_common.h"
#include "olap/olap_engine.h"
#include "olap/olap_table.h"
#include "olap/utils.h"
#include "common/resource_tls.h"
#include "agent/cgroups_mgr.h"
#include "service/backend_options.h"

using std::deque;
using std::list;
using std::lock_guard;
using std::map;
using std::set;
using std::string;
using std::stringstream;
using std::to_string;
using std::vector;

namespace palo {

const uint32_t DOWNLOAD_FILE_MAX_RETRY = 3;
const uint32_t TASK_FINISH_MAX_RETRY = 3;
const uint32_t PUSH_MAX_RETRY = 1;
const uint32_t REPORT_TASK_WORKER_COUNT = 1;
const uint32_t REPORT_DISK_STATE_WORKER_COUNT = 1;
const uint32_t REPORT_OLAP_TABLE_WORKER_COUNT = 1;
const uint32_t LIST_REMOTE_FILE_TIMEOUT = 15;
const std::string HTTP_REQUEST_PREFIX = "/api/_tablet/_download?";
const std::string HTTP_REQUEST_TOKEN_PARAM = "&token=";
const std::string HTTP_REQUEST_FILE_PARAM = "&file=";

std::atomic_ulong TaskWorkerPool::_s_report_version(time(NULL) * 10000);
MutexLock TaskWorkerPool::_s_task_signatures_lock;
MutexLock TaskWorkerPool::_s_running_task_user_count_lock;
map<TTaskType::type, set<int64_t>> TaskWorkerPool::_s_task_signatures;
map<TTaskType::type, map<string, uint32_t>> TaskWorkerPool::_s_running_task_user_count;
map<TTaskType::type, map<string, uint32_t>> TaskWorkerPool::_s_total_task_user_count;
map<TTaskType::type, uint32_t> TaskWorkerPool::_s_total_task_count;
FrontendServiceClientCache TaskWorkerPool::_master_service_client_cache;
boost::mutex TaskWorkerPool::_disk_broken_lock;
boost::posix_time::time_duration TaskWorkerPool::_wait_duration;

TaskWorkerPool::TaskWorkerPool(
        const TaskWorkerType task_worker_type,
        const TMasterInfo& master_info) :
        _master_info(master_info),
        _worker_thread_condition_lock(_worker_thread_lock),
        _task_worker_type(task_worker_type) {
    _agent_utils = new AgentUtils();
    _master_client = new MasterServerClient(_master_info, &_master_service_client_cache);
    _command_executor = new CommandExecutor();
    _backend.__set_host(BackendOptions::get_localhost());
    _backend.__set_be_port(config::be_port);
    _backend.__set_http_port(config::webserver_port);
}

TaskWorkerPool::~TaskWorkerPool() {
    if (_agent_utils != NULL) {
        delete _agent_utils;
        _agent_utils = NULL;
    }
    if (_master_client != NULL) {
        delete _master_client;
        _master_client = NULL;
    }
    if (_command_executor != NULL) {
        delete _command_executor;
        _command_executor = NULL;
    }
}

void TaskWorkerPool::start() {
    // Init task pool and task workers
    switch (_task_worker_type) {
    case TaskWorkerType::CREATE_TABLE:
        _worker_count = config::create_table_worker_count;
        _callback_function = _create_table_worker_thread_callback;
        break;
    case TaskWorkerType::DROP_TABLE:
        _worker_count = config::drop_table_worker_count;
        _callback_function = _drop_table_worker_thread_callback;
        break;
    case TaskWorkerType::PUSH:
        _worker_count =  config::push_worker_count_normal_priority
                + config::push_worker_count_high_priority;
        _callback_function = _push_worker_thread_callback;
        break;
    case TaskWorkerType::DELETE:
        _worker_count = config::delete_worker_count;
        _callback_function = _push_worker_thread_callback;
        break;
    case TaskWorkerType::ALTER_TABLE:
        _worker_count = config::alter_table_worker_count;
        _callback_function = _alter_table_worker_thread_callback;
        break;
    case TaskWorkerType::CLONE:
        _worker_count = config::clone_worker_count;
        _callback_function = _clone_worker_thread_callback;
        break;
    case TaskWorkerType::STORAGE_MEDIUM_MIGRATE:
        _worker_count = config::storage_medium_migrate_count;
        _callback_function = _storage_medium_migrate_worker_thread_callback;
        break;
    case TaskWorkerType::CANCEL_DELETE_DATA:
        _worker_count = config::cancel_delete_data_worker_count;
        _callback_function = _cancel_delete_data_worker_thread_callback;
        break;
    case TaskWorkerType::CHECK_CONSISTENCY:
        _worker_count = config::check_consistency_worker_count;
        _callback_function = _check_consistency_worker_thread_callback;
        break;
    case TaskWorkerType::REPORT_TASK:
        _worker_count = REPORT_TASK_WORKER_COUNT;
        _callback_function = _report_task_worker_thread_callback;
        break;
    case TaskWorkerType::REPORT_DISK_STATE:
        _wait_duration = boost::posix_time::time_duration(0, 0, config::report_disk_state_interval_seconds, 0);
        _worker_count = REPORT_DISK_STATE_WORKER_COUNT;
        _callback_function = _report_disk_state_worker_thread_callback;
        break;
    case TaskWorkerType::REPORT_OLAP_TABLE:
        _wait_duration = boost::posix_time::time_duration(0, 0, config::report_olap_table_interval_seconds, 0);
        _worker_count = REPORT_OLAP_TABLE_WORKER_COUNT;
        _callback_function = _report_olap_table_worker_thread_callback;
        break;
    case TaskWorkerType::UPLOAD:
        _worker_count = config::upload_worker_count;
        _callback_function = _upload_worker_thread_callback;
        break;
    case TaskWorkerType::RESTORE:
        _worker_count = config::restore_worker_count;
        _callback_function = _restore_worker_thread_callback;
        break;
    case TaskWorkerType::MAKE_SNAPSHOT:
        _worker_count = config::make_snapshot_worker_count;
        _callback_function = _make_snapshot_thread_callback;
        break;
    case TaskWorkerType::RELEASE_SNAPSHOT:
        _worker_count = config::release_snapshot_worker_count;
        _callback_function = _release_snapshot_thread_callback;
        break;
    default:
        // pass
        break;
    }

#ifndef BE_TEST
    for (uint32_t i = 0; i < _worker_count; i++) {
        _spawn_callback_worker_thread(_callback_function);
    }
#endif
}

void TaskWorkerPool::submit_task(const TAgentTaskRequest& task) {
    // Submit task to dequeue
    TTaskType::type task_type = task.task_type;
    int64_t signature = task.signature;
    string user("");
    if (task.__isset.resource_info) {
        user = task.resource_info.user;
    }

    bool ret = _record_task_info(task_type, signature, user);
    if (ret == true) {
        {
            lock_guard<MutexLock> worker_thread_lock(_worker_thread_lock);
            _tasks.push_back(task);
            _worker_thread_condition_lock.notify();
        }
    }
}

bool TaskWorkerPool::_record_task_info(
        const TTaskType::type task_type,
        int64_t signature,
        const string& user) {
    bool ret = true;
    lock_guard<MutexLock> task_signatures_lock(_s_task_signatures_lock);

    set<int64_t>& signature_set = _s_task_signatures[task_type];
    if (signature_set.count(signature) > 0) {
        OLAP_LOG_INFO("type: %d, signature: %ld has exist. queue size: %d",
                task_type, signature, signature_set.size());
        ret = false;
    } else {
        signature_set.insert(signature);
        OLAP_LOG_INFO("type: %d, signature: %ld insert success. queue size: %d",
                task_type, signature, signature_set.size());
        if (task_type == TTaskType::PUSH) {
            _s_total_task_user_count[task_type][user] += 1;
            _s_total_task_count[task_type] += 1;
        }
    }

    return ret;
}

void TaskWorkerPool::_remove_task_info(
        const TTaskType::type task_type,
        int64_t signature,
        const string& user) {
    lock_guard<MutexLock> task_signatures_lock(_s_task_signatures_lock);
    set<int64_t>& signature_set = _s_task_signatures[task_type];
    signature_set.erase(signature);

    if (task_type == TTaskType::PUSH) {
        _s_total_task_user_count[task_type][user] -= 1;
        _s_total_task_count[task_type] -= 1;

        {
            lock_guard<MutexLock> running_task_user_count_lock(_s_running_task_user_count_lock);
            _s_running_task_user_count[task_type][user] -= 1;
        }
    }

    OLAP_LOG_INFO("type: %d, signature: %ld has been erased. queue size: %d",
            task_type, signature, signature_set.size());
}

void TaskWorkerPool::_spawn_callback_worker_thread(CALLBACK_FUNCTION callback_func) {
    // Create worker thread
    pthread_t thread;
    sigset_t mask;
    sigset_t omask;
    int err = 0;

    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGPIPE);
    pthread_sigmask(SIG_SETMASK, &mask, &omask);

    while (true) {
        err = pthread_create(&thread, NULL, callback_func, this);
        if (err != 0) {
            OLAP_LOG_WARNING("failed to spawn a thread. error: %d", err);
            sleep(config::sleep_one_second);
        } else {
            pthread_detach(thread);
            break;
        }
    }
}

void TaskWorkerPool::_finish_task(const TFinishTaskRequest& finish_task_request) {
    // Return result to fe
    TMasterResult result;
    int32_t try_time = 0;

    while (try_time < TASK_FINISH_MAX_RETRY) {
        AgentStatus client_status = _master_client->finish_task(finish_task_request, &result);

        if (client_status == PALO_SUCCESS) {
            OLAP_LOG_INFO("finish task success.result: %d", result.status.status_code);
            break;
        } else {
            OLAP_LOG_WARNING("finish task failed.result: %d", result.status.status_code);
            try_time += 1;
        }
#ifndef BE_TEST
        sleep(config::sleep_one_second);
#endif
    }
}

uint32_t TaskWorkerPool::_get_next_task_index(
        int32_t thread_count,
        std::deque<TAgentTaskRequest>& tasks,
        TPriority::type priority) {
    deque<TAgentTaskRequest>::size_type task_count = tasks.size();
    string user;
    int32_t index = -1;
    set<string> improper_users;

    for (uint32_t i = 0; i < task_count; ++i) {
        TAgentTaskRequest task = tasks[i];
        if (task.__isset.resource_info) {
            user = task.resource_info.user;
        }

        if (priority == TPriority::HIGH) {
            if (task.__isset.priority && task.priority == TPriority::HIGH) {
                index = i;
                break;
            } else {
                continue;
            }
        }

        if (improper_users.count(user) != 0) {
            continue;
        }

        float user_total_rate = 0;
        float user_running_rate = 0;
        {
            lock_guard<MutexLock> task_signatures_lock(_s_task_signatures_lock);
            user_total_rate = _s_total_task_user_count[task.task_type][user] * 1.0 /
                              _s_total_task_count[task.task_type];
            user_running_rate = (_s_running_task_user_count[task.task_type][user] + 1) * 1.0 /
                                thread_count;
        }

        OLAP_LOG_INFO("get next task. signature: %ld, user: %s, "
                      "total_task_user_count: %ud, total_task_count: %ud, "
                      "running_task_user_count: %ud, thread_count: %d, "
                      "user_total_rate: %f, user_running_rate: %f",
                      task.signature, user.c_str(),
                      _s_total_task_user_count[task.task_type][user],
                      _s_total_task_count[task.task_type],
                      _s_running_task_user_count[task.task_type][user] + 1,
                      thread_count, user_total_rate, user_running_rate);
        if (_s_running_task_user_count[task.task_type][user] == 0
                || user_running_rate <= user_total_rate) {
            index = i;
            break;
        } else {
            improper_users.insert(user);
        }
    }

    if (index == -1) {
        if (priority == TPriority::HIGH) {
            return index;
        }

        index = 0;
        if (tasks[0].__isset.resource_info) {
            user = tasks[0].resource_info.user;
        } else {
            user = "";
        }
    }

    {
        lock_guard<MutexLock> running_task_user_count_lock(_s_running_task_user_count_lock);
        _s_running_task_user_count[tasks[index].task_type][user] += 1;
    }
    return index;
}

void* TaskWorkerPool::_create_table_worker_thread_callback(void* arg_this) {
    TaskWorkerPool* worker_pool_this = (TaskWorkerPool*)arg_this;

#ifndef BE_TEST
    while (true) {
#endif
        TAgentTaskRequest agent_task_req;
        TCreateTabletReq create_tablet_req;
        {
            lock_guard<MutexLock> worker_thread_lock(worker_pool_this->_worker_thread_lock);
            while (worker_pool_this->_tasks.empty()) {
                worker_pool_this->_worker_thread_condition_lock.wait();
            }

            agent_task_req = worker_pool_this->_tasks.front();
            create_tablet_req = agent_task_req.create_tablet_req;
            worker_pool_this->_tasks.pop_front();
        }

        TStatusCode::type status_code = TStatusCode::OK;
        vector<string> error_msgs;
        TStatus task_status;

        OLAPStatus create_status = worker_pool_this->_command_executor->create_table(create_tablet_req);
        if (create_status != OLAPStatus::OLAP_SUCCESS) {
            OLAP_LOG_WARNING("create table failed. status: %d, signature: %ld",
                             create_status, agent_task_req.signature);
            // TODO liutao09 distinguish the OLAPStatus
            status_code = TStatusCode::RUNTIME_ERROR;
        } else {
            ++_s_report_version;
        }

        task_status.__set_status_code(status_code);
        task_status.__set_error_msgs(error_msgs);

        TFinishTaskRequest finish_task_request;
        finish_task_request.__set_backend(worker_pool_this->_backend);
        finish_task_request.__set_report_version(_s_report_version);
        finish_task_request.__set_task_type(agent_task_req.task_type);
        finish_task_request.__set_signature(agent_task_req.signature);
        finish_task_request.__set_task_status(task_status);

        worker_pool_this->_finish_task(finish_task_request);
        worker_pool_this->_remove_task_info(agent_task_req.task_type, agent_task_req.signature, "");
#ifndef BE_TEST
    }
#endif
    return (void*)0;
}

void* TaskWorkerPool::_drop_table_worker_thread_callback(void* arg_this) {
    TaskWorkerPool* worker_pool_this = (TaskWorkerPool*)arg_this;

#ifndef BE_TEST
    while (true) {
#endif
        TAgentTaskRequest agent_task_req;
        TDropTabletReq drop_tablet_req;
        {
            lock_guard<MutexLock> worker_thread_lock(worker_pool_this->_worker_thread_lock);
            while (worker_pool_this->_tasks.empty()) {
                worker_pool_this->_worker_thread_condition_lock.wait();
            }

            agent_task_req = worker_pool_this->_tasks.front();
            drop_tablet_req = agent_task_req.drop_tablet_req;
            worker_pool_this->_tasks.pop_front();
        }

        TStatusCode::type status_code = TStatusCode::OK;
        vector<string> error_msgs;
        TStatus task_status;

        AgentStatus status = worker_pool_this->_drop_table(drop_tablet_req);
        if (status != PALO_SUCCESS) {
            OLAP_LOG_WARNING(
                "drop table failed! signature: %ld", agent_task_req.signature);
            error_msgs.push_back("drop table failed!");
            status_code = TStatusCode::RUNTIME_ERROR;
        }
        task_status.__set_status_code(status_code);
        task_status.__set_error_msgs(error_msgs);

        TFinishTaskRequest finish_task_request;
        finish_task_request.__set_backend(worker_pool_this->_backend);
        finish_task_request.__set_task_type(agent_task_req.task_type);
        finish_task_request.__set_signature(agent_task_req.signature);
        finish_task_request.__set_task_status(task_status);

        worker_pool_this->_finish_task(finish_task_request);
        worker_pool_this->_remove_task_info(agent_task_req.task_type, agent_task_req.signature, "");
#ifndef BE_TEST
    }
#endif
    return (void*)0;
}

void* TaskWorkerPool::_alter_table_worker_thread_callback(void* arg_this) {
    TaskWorkerPool* worker_pool_this = (TaskWorkerPool*)arg_this;

#ifndef BE_TEST
    while (true) {
#endif
        TAgentTaskRequest agent_task_req;
        TAlterTabletReq alter_tablet_request;
        {
            lock_guard<MutexLock> worker_thread_lock(worker_pool_this->_worker_thread_lock);
            while (worker_pool_this->_tasks.empty()) {
                worker_pool_this->_worker_thread_condition_lock.wait();
            }

            agent_task_req = worker_pool_this->_tasks.front();
            alter_tablet_request = agent_task_req.alter_tablet_req;
            worker_pool_this->_tasks.pop_front();
        }
        // Try to register to cgroups_mgr
        CgroupsMgr::apply_system_cgroup();
        int64_t signatrue = agent_task_req.signature;
        OLAP_LOG_INFO("get alter table task, signature: %ld", agent_task_req.signature);

        TFinishTaskRequest finish_task_request;
        TTaskType::type task_type = agent_task_req.task_type;
        switch (task_type) {
        case TTaskType::SCHEMA_CHANGE:
        case TTaskType::ROLLUP:
            worker_pool_this->_alter_table(alter_tablet_request,
                                           signatrue,
                                           task_type,
                                           &finish_task_request);
            break;
        default:
            // pass
            break;
        }

        worker_pool_this->_finish_task(finish_task_request);
        worker_pool_this->_remove_task_info(agent_task_req.task_type, agent_task_req.signature, "");
#ifndef BE_TEST
    }
#endif
    return (void*)0;
}

void TaskWorkerPool::_alter_table(
        const TAlterTabletReq& alter_tablet_request,
        int64_t signature,
        const TTaskType::type task_type,
        TFinishTaskRequest* finish_task_request) {
    AgentStatus status = PALO_SUCCESS;
    TStatus task_status;
    vector<string> error_msgs;

    string process_name;
    switch (task_type) {
    case TTaskType::ROLLUP:
        process_name = "roll up";
        break;
    case TTaskType::SCHEMA_CHANGE:
        process_name = "schema change";
        break;
    default:
        OLAP_LOG_WARNING("schema change type invalid. type: %d, signature: %ld.",
                         task_type, signature);
        status = PALO_TASK_REQUEST_ERROR;
        break;
    }

    TTabletId base_tablet_id = alter_tablet_request.base_tablet_id;
    TSchemaHash base_schema_hash = alter_tablet_request.base_schema_hash;

    // Check last schema change status, if failed delete tablet file
    // Do not need to adjust delete success or not
    // Because if delete failed create rollup will failed
    if (status == PALO_SUCCESS) {
        // Check lastest schema change status
        AlterTableStatus alter_table_status = _show_alter_table_status(
                base_tablet_id,
                base_schema_hash);
        OLAP_LOG_INFO("get alter table status: %d first, signature: %ld",
                      alter_table_status, signature);

        // Delete failed alter table tablet file
        if (alter_table_status == ALTER_TABLE_FAILED) {
            TDropTabletReq drop_tablet_req;
            drop_tablet_req.__set_tablet_id(alter_tablet_request.new_tablet_req.tablet_id);
            drop_tablet_req.__set_schema_hash(alter_tablet_request.new_tablet_req.tablet_schema.schema_hash);
            status = _drop_table(drop_tablet_req);

            if (status != PALO_SUCCESS) {
                OLAP_LOG_WARNING("delete failed rollup file failed, status: %d, "
                                 "signature: %ld.",
                                 status, signature);
                error_msgs.push_back("delete failed rollup file failed, "
                                     "signature: " + to_string(signature));
            }
        }

        if (status == PALO_SUCCESS) {
            if (alter_table_status == ALTER_TABLE_DONE
                    || alter_table_status == ALTER_TABLE_FAILED
                    || alter_table_status == ALTER_TABLE_WAITING) {
                // Create rollup table
                OLAPStatus ret = OLAPStatus::OLAP_SUCCESS;
                switch (task_type) {
                case TTaskType::ROLLUP:
                    ret = _command_executor->create_rollup_table(alter_tablet_request);
                    break;
                case TTaskType::SCHEMA_CHANGE:
                    ret = _command_executor->schema_change(alter_tablet_request);
                    break;
                default:
                    // pass
                    break;
                }
                if (ret != OLAPStatus::OLAP_SUCCESS) {
                    status = PALO_ERROR;
                    OLAP_LOG_WARNING("%s failed. signature: %ld, status: %d",
                                     process_name.c_str(), signature, status);
                }
            }
        }
    }

    if (status == PALO_SUCCESS) {
        ++_s_report_version;
        OLAP_LOG_INFO("%s finished. signature: %ld", process_name.c_str(), signature);
    }

    // Return result to fe
    finish_task_request->__set_backend(_backend);
    finish_task_request->__set_report_version(_s_report_version);
    finish_task_request->__set_task_type(task_type);
    finish_task_request->__set_signature(signature);

    vector<TTabletInfo> finish_tablet_infos;
    if (status == PALO_SUCCESS) {
        TTabletInfo tablet_info;
        status = _get_tablet_info(
                alter_tablet_request.new_tablet_req.tablet_id,
                alter_tablet_request.new_tablet_req.tablet_schema.schema_hash,
                signature,
                &tablet_info);

        if (status != PALO_SUCCESS) {
            OLAP_LOG_WARNING("%s success, but get new tablet info failed."
                             "tablet_id: %ld, schema_hash: %ld, signature: %ld.",
                             process_name.c_str(),
                             alter_tablet_request.new_tablet_req.tablet_id,
                             alter_tablet_request.new_tablet_req.tablet_schema.schema_hash,
                             signature);
        } else {
            finish_tablet_infos.push_back(tablet_info);
        }
    }

    if (status == PALO_SUCCESS) {
        finish_task_request->__set_finish_tablet_infos(finish_tablet_infos);
        OLAP_LOG_INFO("%s success. signature: %ld", process_name.c_str(), signature);
        error_msgs.push_back(process_name + " success");
        task_status.__set_status_code(TStatusCode::OK);
    } else if (status == PALO_TASK_REQUEST_ERROR) {
        OLAP_LOG_WARNING("alter table request task type invalid. "
                         "signature: %ld", signature);
        error_msgs.push_back("alter table request new tablet id or schema count invalid.");
        task_status.__set_status_code(TStatusCode::ANALYSIS_ERROR);
    } else {
        OLAP_LOG_WARNING("%s failed. signature: %ld", process_name.c_str(), signature);
        error_msgs.push_back(process_name + " failed");
        error_msgs.push_back("status: " + _agent_utils->print_agent_status(status));
        task_status.__set_status_code(TStatusCode::RUNTIME_ERROR);
    }

    task_status.__set_error_msgs(error_msgs);
    finish_task_request->__set_task_status(task_status);
}

void* TaskWorkerPool::_push_worker_thread_callback(void* arg_this) {
    // Try to register to cgroups_mgr
    CgroupsMgr::apply_system_cgroup();
    TaskWorkerPool* worker_pool_this = (TaskWorkerPool*)arg_this;

    // gen high priority worker thread
    TPriority::type priority = TPriority::NORMAL;
    int32_t push_worker_count_high_priority = config::push_worker_count_high_priority;
    static uint32_t s_worker_count = 0;
    {
        lock_guard<MutexLock> worker_thread_lock(worker_pool_this->_worker_thread_lock);
        if (s_worker_count < push_worker_count_high_priority) {
            ++s_worker_count;
            priority = TPriority::HIGH;
        }
    }

#ifndef BE_TEST
    while (true) {
#endif
        AgentStatus status = PALO_SUCCESS;
        TAgentTaskRequest agent_task_req;
        TPushReq push_req;
        string user;
        int32_t index = 0;
        do {
            lock_guard<MutexLock> worker_thread_lock(worker_pool_this->_worker_thread_lock);
            while (worker_pool_this->_tasks.empty()) {
                worker_pool_this->_worker_thread_condition_lock.wait();
            }

            index = worker_pool_this->_get_next_task_index(
                    config::push_worker_count_normal_priority
                            + config::push_worker_count_high_priority,
                    worker_pool_this->_tasks, priority);

            if (index < 0) {
                // there is no high priority task. notify other thread to handle normal task
                worker_pool_this->_worker_thread_condition_lock.notify();
                break;
            }

            agent_task_req = worker_pool_this->_tasks[index];
            if (agent_task_req.__isset.resource_info) {
                user = agent_task_req.resource_info.user;
            }
            push_req = agent_task_req.push_req;
            worker_pool_this->_tasks.erase(worker_pool_this->_tasks.begin() + index);
        } while (0);

#ifndef BE_TEST
        if (index < 0) {
            // there is no high priority task in queue
            sleep(1);
            continue;
        }
#endif

        OLAP_LOG_INFO("get push task. signature: %ld, user: %s, priority: %d",
                      agent_task_req.signature, user.c_str(), priority);

        vector<TTabletInfo> tablet_infos;
        if (push_req.push_type == TPushType::LOAD || push_req.push_type == TPushType::LOAD_DELETE) {
#ifndef BE_TEST
            Pusher pusher(push_req);
            status = pusher.init();
#else
            status = worker_pool_this->_pusher->init();
#endif

            if (status == PALO_SUCCESS) {
                uint32_t retry_time = 0;
                while (retry_time < PUSH_MAX_RETRY) {
#ifndef BE_TEST
                    status = pusher.process(&tablet_infos);
#else
                    status = worker_pool_this->_pusher->process(&tablet_infos);
#endif
                    // Internal error, need retry
                    if (status == PALO_ERROR) {
                        OLAP_LOG_WARNING("push internal error, need retry.signature: %ld",
                                         agent_task_req.signature);
                        retry_time += 1;
                    } else {
                        break;
                    }
                }
            }
        } else if (push_req.push_type == TPushType::DELETE) {
            OLAPStatus delete_data_status =
                     worker_pool_this->_command_executor->delete_data(push_req, &tablet_infos);
            if (delete_data_status != OLAPStatus::OLAP_SUCCESS) {
                OLAP_LOG_WARNING("delet data failed. statusta: %d, signature: %ld",
                                 delete_data_status, agent_task_req.signature);
                status = PALO_ERROR;
            }
        } else {
            status = PALO_TASK_REQUEST_ERROR;
        }

        // Return result to fe
        vector<string> error_msgs;
        TStatus task_status;

        TFinishTaskRequest finish_task_request;
        finish_task_request.__set_backend(worker_pool_this->_backend);
        finish_task_request.__set_task_type(agent_task_req.task_type);
        finish_task_request.__set_signature(agent_task_req.signature);
        if (push_req.push_type == TPushType::DELETE) {
            finish_task_request.__set_request_version(push_req.version);
            finish_task_request.__set_request_version_hash(push_req.version_hash);
        }

        if (status == PALO_SUCCESS) {
            OLAP_LOG_DEBUG("push ok.signature: %ld", agent_task_req.signature);
            error_msgs.push_back("push success");

            ++_s_report_version;

            task_status.__set_status_code(TStatusCode::OK);
            finish_task_request.__set_finish_tablet_infos(tablet_infos);
        } else if (status == PALO_TASK_REQUEST_ERROR) {
            OLAP_LOG_WARNING("push request push_type invalid. type: %d, signature: %ld",
                             push_req.push_type, agent_task_req.signature);
            error_msgs.push_back("push request push_type invalid.");
            task_status.__set_status_code(TStatusCode::ANALYSIS_ERROR);
        } else {
            OLAP_LOG_WARNING("push failed, error_code: %d, signature: %ld",
                             status, agent_task_req.signature);
            error_msgs.push_back("push failed");
            task_status.__set_status_code(TStatusCode::RUNTIME_ERROR);
        }
        task_status.__set_error_msgs(error_msgs);
        finish_task_request.__set_task_status(task_status);
        finish_task_request.__set_report_version(_s_report_version);

        worker_pool_this->_finish_task(finish_task_request);
        worker_pool_this->_remove_task_info(
                agent_task_req.task_type, agent_task_req.signature, user);
#ifndef BE_TEST
    }
#endif

    return (void*)0;
}

void* TaskWorkerPool::_clone_worker_thread_callback(void* arg_this) {
    TaskWorkerPool* worker_pool_this = (TaskWorkerPool*)arg_this;

#ifndef BE_TEST
    while (true) {
#endif
        AgentStatus status = PALO_SUCCESS;
        TAgentTaskRequest agent_task_req;
        TCloneReq clone_req;

        {
            lock_guard<MutexLock> worker_thread_lock(worker_pool_this->_worker_thread_lock);
            while (worker_pool_this->_tasks.empty()) {
                worker_pool_this->_worker_thread_condition_lock.wait();
            }

            agent_task_req = worker_pool_this->_tasks.front();
            clone_req = agent_task_req.clone_req;
            worker_pool_this->_tasks.pop_front();
        }
        // Try to register to cgroups_mgr
        CgroupsMgr::apply_system_cgroup();
        OLAP_LOG_INFO("get clone task. signature: %ld", agent_task_req.signature);

        vector<string> error_msgs;
        // Check local tablet exist or not
        SmartOLAPTable tablet =
                worker_pool_this->_command_executor->get_table(
                clone_req.tablet_id, clone_req.schema_hash);
        if (tablet.get() != NULL) {
            OLAP_LOG_INFO("clone tablet exist yet. tablet_id: %ld, schema_hash: %ld, "
                          "signature: %ld",
                          clone_req.tablet_id, clone_req.schema_hash,
                          agent_task_req.signature);
            error_msgs.push_back("clone tablet exist yet.");
            status = PALO_CREATE_TABLE_EXIST;
        }

        // Get local disk from olap
        string local_shard_root_path;
        if (status == PALO_SUCCESS) {
            OLAPStatus olap_status = worker_pool_this->_command_executor->obtain_shard_path(
                    clone_req.storage_medium, &local_shard_root_path);
            if (olap_status != OLAP_SUCCESS) {
                OLAP_LOG_WARNING("clone get local root path failed. signature: %ld",
                                 agent_task_req.signature);
                error_msgs.push_back("clone get local root path failed.");
                status = PALO_ERROR;
            }
        }

        string src_file_path;
        TBackend src_host;
        if (status == PALO_SUCCESS) {
            status = worker_pool_this->_clone_copy(
                    clone_req,
                    agent_task_req.signature,
                    local_shard_root_path,
                    &src_host,
                    &src_file_path,
                    &error_msgs);
        }

        if (status == PALO_SUCCESS) {
            OLAP_LOG_INFO("clone copy done, src_host: %s, src_file_path: %s",
                          src_host.host.c_str(), src_file_path.c_str());
            // Load header
            OLAPStatus load_header_status =
                    worker_pool_this->_command_executor->load_header(
                            local_shard_root_path,
                            clone_req.tablet_id,
                            clone_req.schema_hash);
            if (load_header_status != OLAP_SUCCESS) {
                OLAP_LOG_WARNING("load header failed. local_shard_root_path: %s, schema_hash: %d, "
                                 "status: %d, signature: %ld",
                                 local_shard_root_path.c_str(), clone_req.schema_hash,
                                 load_header_status, agent_task_req.signature);
                error_msgs.push_back("load header failed.");
                status = PALO_ERROR;
            }
        }

#ifndef BE_TEST
        // Clean useless dir, if failed, ignore it.
        if (status != PALO_SUCCESS && status != PALO_CREATE_TABLE_EXIST) {
            stringstream local_data_path_stream;
            local_data_path_stream << local_shard_root_path
                                   << "/" << clone_req.tablet_id
                                   << "/" << clone_req.schema_hash;
            string local_data_path = local_data_path_stream.str();
            OLAP_LOG_INFO("clone failed. want to delete local dir: %s, signature: %ld",
                          local_data_path.c_str(), agent_task_req.signature);
            try {
                boost::filesystem::path local_path(local_data_path);
                if (boost::filesystem::exists(local_path)) {
                    boost::filesystem::remove_all(local_path);
                }
            } catch (boost::filesystem::filesystem_error e) {
                // Ignore the error, OLAP will delete it
                OLAP_LOG_WARNING("clone delete useless dir failed. "
                                 "error: %s, local dir: %s, signature: %ld",
                                 e.what(), local_data_path.c_str(),
                                 agent_task_req.signature);
            }
        }
#endif

        // Get clone tablet info
        vector<TTabletInfo> tablet_infos;
        if (status == PALO_SUCCESS || status == PALO_CREATE_TABLE_EXIST) {
            TTabletInfo tablet_info;
            AgentStatus get_tablet_info_status = worker_pool_this->_get_tablet_info(
                    clone_req.tablet_id,
                    clone_req.schema_hash,
                    agent_task_req.signature,
                    &tablet_info);
            if (get_tablet_info_status != PALO_SUCCESS) {
                OLAP_LOG_WARNING("clone success, but get tablet info failed."
                                 "tablet id: %ld, schema hash: %ld, signature: %ld",
                                 clone_req.tablet_id, clone_req.schema_hash,
                                 agent_task_req.signature);
                error_msgs.push_back("clone success, but get tablet info failed.");
                status = PALO_ERROR;
            } else if (
                (clone_req.__isset.committed_version
                        && clone_req.__isset.committed_version_hash)
                        && (tablet_info.version < clone_req.committed_version ||
                            (tablet_info.version == clone_req.committed_version
                            && tablet_info.version_hash != clone_req.committed_version_hash))) {

                // we need to check if this cloned table's version is what we expect.
                // if not, maybe this is a stale remaining table which is waiting for drop.
                // we drop it.
                OLAP_LOG_INFO("begin to drop the stale table. "
                        "tablet id: %ld, schema hash: %ld, signature: %ld "
                        "version: %ld, version_hash %ld "
                        "expected version: %ld, version_hash: %ld",
                        clone_req.tablet_id, clone_req.schema_hash,
                        agent_task_req.signature,
                        tablet_info.version, tablet_info.version_hash,
                        clone_req.committed_version, clone_req.committed_version_hash);

                TDropTabletReq drop_req;
                drop_req.tablet_id = clone_req.tablet_id;
                drop_req.schema_hash = clone_req.schema_hash;
                AgentStatus drop_status = worker_pool_this->_drop_table(drop_req);
                if (drop_status != PALO_SUCCESS) {
                    // just log
                    OLAP_LOG_WARNING(
                        "drop stale cloned table failed! tabelt id: %ld", clone_req.tablet_id);
                }

                status = PALO_ERROR;
            } else {
                OLAP_LOG_INFO("clone get tablet info success. "
                              "tablet id: %ld, schema hash: %ld, signature: %ld "
                              "version: %ld, version_hash %ld",
                              clone_req.tablet_id, clone_req.schema_hash,
                              agent_task_req.signature,
                              tablet_info.version, tablet_info.version_hash);
                tablet_infos.push_back(tablet_info);
            }
        }

        // Return result to fe
        TStatus task_status;
        TFinishTaskRequest finish_task_request;
        finish_task_request.__set_backend(worker_pool_this->_backend);
        finish_task_request.__set_task_type(agent_task_req.task_type);
        finish_task_request.__set_signature(agent_task_req.signature);

        TStatusCode::type status_code = TStatusCode::OK;
        if (status != PALO_SUCCESS && status != PALO_CREATE_TABLE_EXIST) {
            status_code = TStatusCode::RUNTIME_ERROR;
            OLAP_LOG_WARNING("clone failed. signature: %ld",
                             agent_task_req.signature);
            error_msgs.push_back("clone failed.");
        } else {
            OLAP_LOG_INFO("clone success, set tablet infos. signature: %ld",
                          agent_task_req.signature);
            finish_task_request.__set_finish_tablet_infos(tablet_infos);
        }
        task_status.__set_status_code(status_code);
        task_status.__set_error_msgs(error_msgs);
        finish_task_request.__set_task_status(task_status);

        worker_pool_this->_finish_task(finish_task_request);
        worker_pool_this->_remove_task_info(agent_task_req.task_type, agent_task_req.signature, "");
#ifndef BE_TEST
    }
#endif

    return (void*)0;
}

AgentStatus TaskWorkerPool::_clone_copy(
        const TCloneReq& clone_req,
        int64_t signature,
        const string& local_data_path,
        TBackend* src_host,
        string* src_file_path,
        vector<string>* error_msgs) {
    AgentStatus status = PALO_SUCCESS;


    std::string token = _master_info.token;

    for (auto src_backend : clone_req.src_backends) {
        stringstream http_host_stream;
        http_host_stream << "http://" << src_backend.host << ":" << src_backend.http_port;
        string http_host = http_host_stream.str();
        // Make snapshot in remote olap engine
        *src_host = src_backend;
#ifndef BE_TEST
        AgentServerClient agent_client(*src_host);
#endif
        TAgentResult make_snapshot_result;
        status = PALO_SUCCESS;

        OLAP_LOG_INFO("pre make snapshot. backend_ip: %s", src_host->host.c_str());
        TSnapshotRequest snapshot_request;
        snapshot_request.__set_tablet_id(clone_req.tablet_id);
        snapshot_request.__set_schema_hash(clone_req.schema_hash);
#ifndef BE_TEST
        agent_client.make_snapshot(
                snapshot_request,
                &make_snapshot_result);
#else
        _agent_client->make_snapshot(
                snapshot_request,
                &make_snapshot_result);
#endif

        if (make_snapshot_result.status.status_code == TStatusCode::OK) {
            if (make_snapshot_result.__isset.snapshot_path) {
                *src_file_path = make_snapshot_result.snapshot_path;
                if (src_file_path->at(src_file_path->length() - 1) != '/') {
                    src_file_path->append("/");
                }
                OLAP_LOG_INFO("make snapshot success. backend_ip: %s, src_file_path: %s,"
                              " signature: %ld",
                              src_host->host.c_str(), src_file_path->c_str(),
                              signature);
            } else {
                OLAP_LOG_WARNING("clone make snapshot success, "
                                 "but get src file path failed. signature: %ld",
                                 signature);
                status = PALO_ERROR;
                continue;
            }
        } else {
            OLAP_LOG_WARNING("make snapshot failed. tablet_id: %ld, schema_hash: %ld, "
                             "backend_ip: %s, backend_port: %d, signature: %ld",
                             clone_req.tablet_id, clone_req.schema_hash,
                             src_host->host.c_str(), src_host->be_port,
                             signature);
            error_msgs->push_back("make snapshot failed. backend_ip: " + src_host->host);
            status = PALO_ERROR;
            continue;
        }

        // Get remote and local full path
        stringstream src_file_full_path_stream;
        stringstream local_file_full_path_stream;

        if (status == PALO_SUCCESS) {
            src_file_full_path_stream << *src_file_path
                                      << "/" << clone_req.tablet_id
                                      << "/" << clone_req.schema_hash << "/";
            local_file_full_path_stream << local_data_path
                                        << "/" << clone_req.tablet_id
                                        << "/" << clone_req.schema_hash << "/";
        }
        string src_file_full_path = src_file_full_path_stream.str();
        string local_file_full_path = local_file_full_path_stream.str();

#ifndef BE_TEST
        // Check local path exist, if exist, remove it, then create the dir
        if (status == PALO_SUCCESS) {
            boost::filesystem::path local_file_full_dir(local_file_full_path);
            if (boost::filesystem::exists(local_file_full_dir)) {
                boost::filesystem::remove_all(local_file_full_dir);
            }
            boost::filesystem::create_directories(local_file_full_dir);
        }
#endif

        // Get remove dir file list
        FileDownloader::FileDownloaderParam downloader_param;
        downloader_param.remote_file_path = http_host + HTTP_REQUEST_PREFIX
            + HTTP_REQUEST_TOKEN_PARAM + token
            + HTTP_REQUEST_FILE_PARAM + src_file_full_path;
        downloader_param.curl_opt_timeout = LIST_REMOTE_FILE_TIMEOUT;

#ifndef BE_TEST
        FileDownloader* file_downloader_ptr = new FileDownloader(downloader_param);
        if (file_downloader_ptr == NULL) {
            OLAP_LOG_WARNING("clone copy create file downloader failed. try next backend");
            status = PALO_ERROR;
        }
#endif

        string file_list_str;
        AgentStatus download_status = PALO_SUCCESS;
        uint32_t download_retry_time = 0;
        while (status == PALO_SUCCESS && download_retry_time < DOWNLOAD_FILE_MAX_RETRY) {
#ifndef BE_TEST
            download_status = file_downloader_ptr->list_file_dir(&file_list_str);
#else
            download_status = _file_downloader_ptr->list_file_dir(&file_list_str);
#endif
            if (download_status != PALO_SUCCESS) {
                OLAP_LOG_WARNING("clone get remote file list failed. backend_ip: %s, "
                                 "src_file_path: %s, signature: %ld",
                                 src_host->host.c_str(),
                                 downloader_param.remote_file_path.c_str(),
                                 signature);
                ++download_retry_time;
                sleep(download_retry_time);
            } else {
                break;
            }
        }

#ifndef BE_TEST
        if (file_downloader_ptr != NULL) {
            delete file_downloader_ptr;
            file_downloader_ptr = NULL;
        }
#endif

        vector<string> file_name_list;
        if (download_status != PALO_SUCCESS) {
            OLAP_LOG_WARNING("clone get remote file list failed over max time. backend_ip: %s, "
                             "src_file_path: %s, signature: %ld",
                             src_host->host.c_str(),
                             downloader_param.remote_file_path.c_str(),
                             signature);
            status = PALO_ERROR;
        } else {
            size_t start_position = 0;
            size_t end_position = file_list_str.find("\n");

            // Split file name from file_list_str
            while (end_position != string::npos) {
                string file_name = file_list_str.substr(
                        start_position, end_position - start_position);
                // If the header file is not exist, the table could't loaded by olap engine.
                // Avoid of data is not complete, we copy the header file at last.
                // The header file's name is end of .hdr.
                if (file_name.size() > 4 && file_name.substr(file_name.size() - 4, 4) == ".hdr") {
                    file_name_list.push_back(file_name);
                } else {
                    file_name_list.insert(file_name_list.begin(), file_name);
                }

                start_position = end_position + 1;
                end_position = file_list_str.find("\n", start_position);
            }
            if (start_position != file_list_str.size()) {
                string file_name = file_list_str.substr(
                        start_position, file_list_str.size() - start_position);
                if (file_name.size() > 4 && file_name.substr(file_name.size() - 4, 4) == ".hdr") {
                    file_name_list.push_back(file_name);
                } else {
                    file_name_list.insert(file_name_list.begin(), file_name);
                }
            }
        }

        // Get copy from remote
        for (auto file_name : file_name_list) {
            download_retry_time = 0;
            downloader_param.remote_file_path = http_host + HTTP_REQUEST_PREFIX
                + HTTP_REQUEST_TOKEN_PARAM + token
                + HTTP_REQUEST_FILE_PARAM + src_file_full_path + file_name;
            downloader_param.local_file_path = local_file_full_path + file_name;

            // Get file length
            uint64_t file_size = 0;
            uint64_t estimate_time_out = 0;

            downloader_param.curl_opt_timeout = GET_LENGTH_TIMEOUT;
#ifndef BE_TEST
            file_downloader_ptr = new FileDownloader(downloader_param);
            if (file_downloader_ptr == NULL) {
                OLAP_LOG_WARNING("clone copy create file downloader failed. try next backend");
                status = PALO_ERROR;
                break;
            }
#endif
            while (download_retry_time < DOWNLOAD_FILE_MAX_RETRY) {
#ifndef BE_TEST
                download_status = file_downloader_ptr->get_length(&file_size);
#else
                download_status = _file_downloader_ptr->get_length(&file_size);
#endif
                if (download_status != PALO_SUCCESS) {
                    OLAP_LOG_WARNING("clone copy get file length failed. backend_ip: %s, "
                                     "src_file_path: %s, signature: %ld",
                                     src_host->host.c_str(),
                                     downloader_param.remote_file_path.c_str(),
                                     signature);
                    ++download_retry_time;
                    sleep(download_retry_time);
                } else {
                    break;
                }
            }

#ifndef BE_TEST
            if (file_downloader_ptr != NULL) {
                delete file_downloader_ptr;
                file_downloader_ptr = NULL;
            }
#endif
            if (download_status != PALO_SUCCESS) {
                OLAP_LOG_WARNING("clone copy get file length failed over max time. "
                                 "backend_ip: %s, src_file_path: %s, signature: %ld",
                                 src_host->host.c_str(),
                                 downloader_param.remote_file_path.c_str(),
                                 signature);
                status = PALO_ERROR;
                break;
            }

            estimate_time_out = file_size / config::download_low_speed_limit_kbps / 1024;
            if (estimate_time_out < config::download_low_speed_time) {
                estimate_time_out = config::download_low_speed_time;
            }

            // Download the file
            download_retry_time = 0;
            downloader_param.curl_opt_timeout = estimate_time_out;
#ifndef BE_TEST
            file_downloader_ptr = new FileDownloader(downloader_param);
            if (file_downloader_ptr == NULL) {
                OLAP_LOG_WARNING("clone copy create file downloader failed. try next backend");
                status = PALO_ERROR;
                break;
            }
#endif
            while (download_retry_time < DOWNLOAD_FILE_MAX_RETRY) {
#ifndef BE_TEST
                download_status = file_downloader_ptr->download_file();
#else
                download_status = _file_downloader_ptr->download_file();
#endif
                if (download_status != PALO_SUCCESS) {
                    OLAP_LOG_WARNING("download file failed. backend_ip: %s, "
                                     "src_file_path: %s, signature: %ld",
                                     src_host->host.c_str(),
                                     downloader_param.remote_file_path.c_str(),
                                     signature);
                } else {
                    // Check file length
                    boost::filesystem::path local_file_path(downloader_param.local_file_path);
                    uint64_t local_file_size = boost::filesystem::file_size(local_file_path);
                    if (local_file_size != file_size) {
                        OLAP_LOG_WARNING("download file length error. backend_ip: %s, "
                                         "src_file_path: %s, signature: %ld,"
                                         "remote file size: %d, local file size: %d",
                                         src_host->host.c_str(),
                                         downloader_param.remote_file_path.c_str(),
                                         signature, file_size, local_file_size);
                        download_status = PALO_FILE_DOWNLOAD_FAILED;
                    } else {
                        chmod(downloader_param.local_file_path.c_str(), S_IRUSR | S_IWUSR);
                        break;
                    }
                }
                ++download_retry_time;
                sleep(download_retry_time);
            } // Try to download a file from remote backend

#ifndef BE_TEST
            if (file_downloader_ptr != NULL) {
                delete file_downloader_ptr;
                file_downloader_ptr = NULL;
            }
#endif

            if (download_status != PALO_SUCCESS) {
                OLAP_LOG_WARNING("download file failed over max retry. backend_ip: %s, "
                                 "src_file_path: %s, signature: %ld",
                                 src_host->host.c_str(),
                                 downloader_param.remote_file_path.c_str(),
                                 signature);
                status = PALO_ERROR;
                break;
            }
        } // Clone files from remote backend

        // Release snapshot, if failed, ignore it. OLAP engine will drop useless snapshot
        TAgentResult release_snapshot_result;
#ifndef BE_TEST
        agent_client.release_snapshot(
                make_snapshot_result.snapshot_path,
                &release_snapshot_result);
#else
        _agent_client->release_snapshot(
                make_snapshot_result.snapshot_path,
                &release_snapshot_result);
#endif
        if (release_snapshot_result.status.status_code != TStatusCode::OK) {
            OLAP_LOG_WARNING("release snapshot failed. src_file_path: %s, signature: %ld",
                             src_file_path->c_str(), signature);
        }

        if (status == PALO_SUCCESS) {
            break;
        }
    } // clone copy from one backend
    return status;
}

void* TaskWorkerPool::_storage_medium_migrate_worker_thread_callback(void* arg_this) {
    TaskWorkerPool* worker_pool_this = (TaskWorkerPool*)arg_this;

#ifndef BE_TEST
    while (true) {
#endif
        // Try to register to cgroups_mgr
        CgroupsMgr::apply_system_cgroup();
        TAgentTaskRequest agent_task_req;
        TStorageMediumMigrateReq storage_medium_migrate_req;
        {
            lock_guard<MutexLock> worker_thread_lock(worker_pool_this->_worker_thread_lock);
            while (worker_pool_this->_tasks.empty()) {
                worker_pool_this->_worker_thread_condition_lock.wait();
            }

            agent_task_req = worker_pool_this->_tasks.front();
            storage_medium_migrate_req = agent_task_req.storage_medium_migrate_req;
            worker_pool_this->_tasks.pop_front();
        }

        TStatusCode::type status_code = TStatusCode::OK;
        vector<string> error_msgs;
        TStatus task_status;

        OLAPStatus res = OLAPStatus::OLAP_SUCCESS;
        res = worker_pool_this->_command_executor->storage_medium_migrate(storage_medium_migrate_req);
        if (res != OLAPStatus::OLAP_SUCCESS) {
            OLAP_LOG_WARNING("storage media migrate failed. status: %d, signature: %ld",
                             res, agent_task_req.signature);
            status_code = TStatusCode::RUNTIME_ERROR;
        } else {
            OLAP_LOG_INFO("storage media migrate success. status: %d, signature: %ld",
                          res, agent_task_req.signature);
        }

        task_status.__set_status_code(status_code);
        task_status.__set_error_msgs(error_msgs);

        TFinishTaskRequest finish_task_request;
        finish_task_request.__set_backend(worker_pool_this->_backend);
        finish_task_request.__set_task_type(agent_task_req.task_type);
        finish_task_request.__set_signature(agent_task_req.signature);
        finish_task_request.__set_task_status(task_status);

        worker_pool_this->_finish_task(finish_task_request);
        worker_pool_this->_remove_task_info(agent_task_req.task_type, agent_task_req.signature, "");
#ifndef BE_TEST
    }
#endif
    return (void*)0;
}

void* TaskWorkerPool::_cancel_delete_data_worker_thread_callback(void* arg_this) {
    TaskWorkerPool* worker_pool_this = (TaskWorkerPool*)arg_this;

#ifndef BE_TEST
    while (true) {
#endif
        TAgentTaskRequest agent_task_req;
        TCancelDeleteDataReq cancel_delete_data_req;
        {
            lock_guard<MutexLock> worker_thread_lock(worker_pool_this->_worker_thread_lock);
            while (worker_pool_this->_tasks.empty()) {
                worker_pool_this->_worker_thread_condition_lock.wait();
            }

            agent_task_req = worker_pool_this->_tasks.front();
            cancel_delete_data_req = agent_task_req.cancel_delete_data_req;
            worker_pool_this->_tasks.pop_front();
        }

        OLAP_LOG_INFO("get cancel delete data task. signature: %ld",
                      agent_task_req.signature);
        TStatusCode::type status_code = TStatusCode::OK;
        vector<string> error_msgs;
        TStatus task_status;

        OLAPStatus cancel_delete_data_status = OLAPStatus::OLAP_SUCCESS;
        cancel_delete_data_status =
                worker_pool_this->_command_executor->cancel_delete(cancel_delete_data_req);
        if (cancel_delete_data_status != OLAPStatus::OLAP_SUCCESS) {
            OLAP_LOG_WARNING("cancel delete data failed. statusta: %d, signature: %ld",
                             cancel_delete_data_status, agent_task_req.signature);
            status_code = TStatusCode::RUNTIME_ERROR;
        } else {
            OLAP_LOG_INFO("cancel delete data success. statusta: %d, signature: %ld",
                          cancel_delete_data_status, agent_task_req.signature);
        }

        task_status.__set_status_code(status_code);
        task_status.__set_error_msgs(error_msgs);

        TFinishTaskRequest finish_task_request;
        finish_task_request.__set_backend(worker_pool_this->_backend);
        finish_task_request.__set_task_type(agent_task_req.task_type);
        finish_task_request.__set_signature(agent_task_req.signature);
        finish_task_request.__set_task_status(task_status);

        worker_pool_this->_finish_task(finish_task_request);
        worker_pool_this->_remove_task_info(
                agent_task_req.task_type, agent_task_req.signature, "");
#ifndef BE_TEST
    }
#endif
    return (void*)0;
}

void* TaskWorkerPool::_check_consistency_worker_thread_callback(void* arg_this) {
    TaskWorkerPool* worker_pool_this = (TaskWorkerPool*)arg_this;

#ifndef BE_TEST
    while (true) {
#endif
        // Try to register to cgroups_mgr
        CgroupsMgr::apply_system_cgroup();
        TAgentTaskRequest agent_task_req;
        TCheckConsistencyReq check_consistency_req;
        {
            lock_guard<MutexLock> worker_thread_lock(worker_pool_this->_worker_thread_lock);
            while (worker_pool_this->_tasks.empty()) {
                worker_pool_this->_worker_thread_condition_lock.wait();
            }

            agent_task_req = worker_pool_this->_tasks.front();
            check_consistency_req = agent_task_req.check_consistency_req;
            worker_pool_this->_tasks.pop_front();
        }

        TStatusCode::type status_code = TStatusCode::OK;
        vector<string> error_msgs;
        TStatus task_status;

        OLAPStatus res = OLAPStatus::OLAP_SUCCESS;
        uint32_t checksum = 0;
        res = worker_pool_this->_command_executor->compute_checksum(
                check_consistency_req.tablet_id,
                check_consistency_req.schema_hash,
                check_consistency_req.version,
                check_consistency_req.version_hash,
                &checksum);
        if (res != OLAPStatus::OLAP_SUCCESS) {
            OLAP_LOG_WARNING("check consistency failed. status: %d, signature: %ld",
                             res, agent_task_req.signature);
            status_code = TStatusCode::RUNTIME_ERROR;
        } else {
            OLAP_LOG_INFO("check consistency success. status: %d, signature: %ld. checksum: %ud",
                          res, agent_task_req.signature, checksum);
        }

        task_status.__set_status_code(status_code);
        task_status.__set_error_msgs(error_msgs);

        TFinishTaskRequest finish_task_request;
        finish_task_request.__set_backend(worker_pool_this->_backend);
        finish_task_request.__set_task_type(agent_task_req.task_type);
        finish_task_request.__set_signature(agent_task_req.signature);
        finish_task_request.__set_task_status(task_status);
        finish_task_request.__set_tablet_checksum(static_cast<int64_t>(checksum));
        finish_task_request.__set_request_version(check_consistency_req.version);
        finish_task_request.__set_request_version_hash(check_consistency_req.version_hash);

        worker_pool_this->_finish_task(finish_task_request);
        worker_pool_this->_remove_task_info(agent_task_req.task_type, agent_task_req.signature, "");
#ifndef BE_TEST
    }
#endif
    return (void*)0;
}

void* TaskWorkerPool::_report_task_worker_thread_callback(void* arg_this) {
    TaskWorkerPool* worker_pool_this = (TaskWorkerPool*)arg_this;

    TReportRequest request;
    request.__set_backend(worker_pool_this->_backend);

#ifndef BE_TEST
    while (true) {
#endif
        {
            lock_guard<MutexLock> task_signatures_lock(_s_task_signatures_lock);
            request.__set_tasks(_s_task_signatures);
        }
        OLAP_LOG_INFO("master host: %s, port: %d",
                      worker_pool_this->_master_info.network_address.hostname.c_str(),
                      worker_pool_this->_master_info.network_address.port);
        TMasterResult result;
        AgentStatus status = worker_pool_this->_master_client->report(request, &result);

        if (status == PALO_SUCCESS) {
            OLAP_LOG_INFO("finish report task success. return code: %d",
                          result.status.status_code);
        } else {
            OLAP_LOG_WARNING("finish report task failed. status: %d", status);
        }

#ifndef BE_TEST
        sleep(config::report_task_interval_seconds);
    }
#endif

    return (void*)0;
}

void* TaskWorkerPool::_report_disk_state_worker_thread_callback(void* arg_this) {
    TaskWorkerPool* worker_pool_this = (TaskWorkerPool*)arg_this;

    TReportRequest request;
    request.__set_backend(worker_pool_this->_backend);

#ifndef BE_TEST
    while (true) {
#endif
        if (worker_pool_this->_master_info.network_address.port == 0) {
            // port == 0 means not received heartbeat yet
            // sleep a short time and try again
            OLAP_LOG_INFO("waiting to receive first heartbeat from frontend");
            sleep(config::sleep_one_second);
            continue;
        }

        vector<OLAPRootPathStat> root_paths_stat;

        worker_pool_this->_command_executor->get_all_root_path_stat(&root_paths_stat);

        map<string, TDisk> disks;
        for (auto root_path_state : root_paths_stat) {
            TDisk disk;
            disk.__set_root_path(root_path_state.root_path);
            disk.__set_disk_total_capacity(static_cast<double>(root_path_state.disk_total_capacity));
            disk.__set_data_used_capacity(static_cast<double>(root_path_state.data_used_capacity));
            disk.__set_disk_available_capacity(static_cast<double>(root_path_state.disk_available_capacity));
            disk.__set_used(root_path_state.is_used);
            disks[root_path_state.root_path] = disk;
        }
        request.__set_disks(disks);

        TMasterResult result;
        AgentStatus status = worker_pool_this->_master_client->report(request, &result);

        if (status == PALO_SUCCESS) {
            OLAP_LOG_INFO("finish report disk state success. return code: %d",
                          result.status.status_code);
        } else {
            OLAP_LOG_WARNING("finish report disk state failed. status: %d", status);
        }

#ifndef BE_TEST
        {
            // wait disk_broken_cv awaken
            // if awaken, set is_report_disk_state_already to true, it will not notify again
            // if overtime, while will go to next cycle
            boost::unique_lock<boost::mutex> lk(_disk_broken_lock);
            if (OLAPRootPath::get_instance()->disk_broken_cv.timed_wait(lk, _wait_duration)) {
                OLAPRootPath::get_instance()->is_report_disk_state_already = true;
            }
        }
    }
#endif

    return (void*)0;
}

void* TaskWorkerPool::_report_olap_table_worker_thread_callback(void* arg_this) {
    TaskWorkerPool* worker_pool_this = (TaskWorkerPool*)arg_this;

    TReportRequest request;
    request.__set_backend(worker_pool_this->_backend);
    request.__isset.tablets = true;
    AgentStatus status = PALO_SUCCESS;

#ifndef BE_TEST
    while (true) {
#endif
        if (worker_pool_this->_master_info.network_address.port == 0) {
            // port == 0 means not received heartbeat yet
            // sleep a short time and try again
            OLAP_LOG_INFO("waiting to receive first heartbeat from frontend");
            sleep(config::sleep_one_second);
            continue;
        }

        request.tablets.clear();

        request.__set_report_version(_s_report_version);
        OLAPStatus report_all_tablets_info_status =
                worker_pool_this->_command_executor->report_all_tablets_info(&request.tablets);
        if (report_all_tablets_info_status != OLAP_SUCCESS) {
            OLAP_LOG_WARNING("report get all tablets info failed. status: %d",
                             report_all_tablets_info_status);
#ifndef BE_TEST
            // wait disk_broken_cv awaken
            // if awaken, set is_report_olap_table_already to true, it will not notify again
            // if overtime, while will go to next cycle
            boost::unique_lock<boost::mutex> lk(_disk_broken_lock);
            if (OLAPRootPath::get_instance()->disk_broken_cv.timed_wait(lk, _wait_duration)) {
                OLAPRootPath::get_instance()->is_report_olap_table_already =  true;
            }
            continue;
#else
            return (void*)0;
#endif
        }

        TMasterResult result;
        status = worker_pool_this->_master_client->report(request, &result);

        if (status == PALO_SUCCESS) {
            OLAP_LOG_INFO("finish report olap table success. return code: %d",
                          result.status.status_code);
        } else {
            OLAP_LOG_WARNING("finish report olap table failed. status: %d", status);
        }

#ifndef BE_TEST
        // wait disk_broken_cv awaken
        // if awaken, set is_report_olap_table_already to true, it will not notify again
        // if overtime, while will go to next cycle
        boost::unique_lock<boost::mutex> lk(_disk_broken_lock);
        if (OLAPRootPath::get_instance()->disk_broken_cv.timed_wait(lk, _wait_duration)) {
            OLAPRootPath::get_instance()->is_report_olap_table_already =  true;
        }
    }
#endif

    return (void*)0;
}

void* TaskWorkerPool::_upload_worker_thread_callback(void* arg_this) {
    TaskWorkerPool* worker_pool_this = (TaskWorkerPool*)arg_this;

#ifndef BE_TEST
    while (true) {
#endif
        TAgentTaskRequest agent_task_req;
        TUploadReq upload_request;
        {
            lock_guard<MutexLock> worker_thread_lock(worker_pool_this->_worker_thread_lock);
            while (worker_pool_this->_tasks.empty()) {
                worker_pool_this->_worker_thread_condition_lock.wait();
            }

            agent_task_req = worker_pool_this->_tasks.front();
            upload_request = agent_task_req.upload_req;
            worker_pool_this->_tasks.pop_front();
        }
        // Try to register to cgroups_mgr
        CgroupsMgr::apply_system_cgroup();
        OLAP_LOG_INFO("get upload task, signature: %ld", agent_task_req.signature);

        TStatusCode::type status_code = TStatusCode::OK;
        vector<string> error_msgs;
        TStatus task_status;

        // Write remote source info into file by json format
        pthread_t tid = pthread_self();
        time_t now = time(NULL);
        stringstream label_stream;
        label_stream << tid << "_" << now;
        string label(label_stream.str());
        string info_file_path(config::agent_tmp_dir + "/" + label);
        bool ret = worker_pool_this->_agent_utils->write_json_to_file(
                upload_request.remote_source_properties,
                info_file_path);
        if (!ret) {
            status_code = TStatusCode::RUNTIME_ERROR;
            error_msgs.push_back("Write remote source info to file failed. Path:" +
                                 info_file_path);
            OLAP_LOG_WARNING("Write remote source info to file failed. Path: %s",
                             info_file_path.c_str());
        }

        // Upload files to remote source
        stringstream local_file_path_stream;
        local_file_path_stream << upload_request.local_file_path;
        if (upload_request.__isset.tablet_id) {
            local_file_path_stream << "/" << upload_request.tablet_id;
        }
        if (status_code == TStatusCode::OK) {
            string command = "sh " + config::trans_file_tool_path + " " + label + " upload " +
                         local_file_path_stream.str() + " " + upload_request.remote_file_path +
                         " " + info_file_path + " " + "file_list";
            OLAP_LOG_INFO("Upload cmd: %s", command.c_str());
            string errmsg;
            ret = worker_pool_this->_agent_utils->exec_cmd(command, &errmsg);
            if (!ret) {
                status_code = TStatusCode::RUNTIME_ERROR;
                error_msgs.push_back(errmsg);
                OLAP_LOG_WARNING("Upload file failed. Error: %s", errmsg.c_str());
            }
        }

        // Delete tmp file
        boost::filesystem::path file_path(info_file_path);
        if (boost::filesystem::exists(file_path)) {
            boost::filesystem::remove_all(file_path);
        }

        task_status.__set_status_code(status_code);
        task_status.__set_error_msgs(error_msgs);

        TFinishTaskRequest finish_task_request;
        finish_task_request.__set_backend(worker_pool_this->_backend);
        finish_task_request.__set_task_type(agent_task_req.task_type);
        finish_task_request.__set_signature(agent_task_req.signature);
        finish_task_request.__set_task_status(task_status);

        worker_pool_this->_finish_task(finish_task_request);
        worker_pool_this->_remove_task_info(agent_task_req.task_type, agent_task_req.signature, "");
#ifndef BE_TEST
    }
#endif
    return (void*)0;
}

void* TaskWorkerPool::_restore_worker_thread_callback(void* arg_this) {
    TaskWorkerPool* worker_pool_this = (TaskWorkerPool*)arg_this;

#ifndef BE_TEST
    while (true) {
#endif
        TAgentTaskRequest agent_task_req;
        TRestoreReq restore_request;
        {
            lock_guard<MutexLock> worker_thread_lock(worker_pool_this->_worker_thread_lock);
            while (worker_pool_this->_tasks.empty()) {
                worker_pool_this->_worker_thread_condition_lock.wait();
            }

            agent_task_req = worker_pool_this->_tasks.front();
            restore_request = agent_task_req.restore_req;
            worker_pool_this->_tasks.pop_front();
        }
        // Try to register to cgroups_mgr
        CgroupsMgr::apply_system_cgroup();
        OLAP_LOG_INFO("get restore task, signature: %ld", agent_task_req.signature);

        TStatusCode::type status_code = TStatusCode::OK;
        vector<string> error_msgs;
        TStatus task_status;

        // Write remote source info into file by json format
        pthread_t tid = pthread_self();
        time_t now = time(NULL);
        stringstream label_stream;
        label_stream << tid << "_" << now << "_" << restore_request.tablet_id;
        string label(label_stream.str());
        string info_file_path(config::agent_tmp_dir + "/" + label);
        bool ret = worker_pool_this->_agent_utils->write_json_to_file(
                restore_request.remote_source_properties,
                info_file_path);
        if (!ret) {
            status_code = TStatusCode::RUNTIME_ERROR;
            error_msgs.push_back("Write remote source info to file failed. Path:" +
                                 info_file_path);
            OLAP_LOG_WARNING("Write remote source info to file failed. Path: %s",
                             info_file_path.c_str());
        }

        // Get local disk to restore from olap
        string local_shard_root_path;
        if (status_code == TStatusCode::OK) {
            OLAPStatus olap_status = worker_pool_this->_command_executor->obtain_shard_path(
                    TStorageMedium::HDD, &local_shard_root_path);
            if (olap_status != OLAP_SUCCESS) {
                OLAP_LOG_WARNING("clone get local root path failed. signature: %ld",
                                 agent_task_req.signature);
                error_msgs.push_back("clone get local root path failed.");
                status_code = TStatusCode::RUNTIME_ERROR;
            }
        }

        stringstream local_file_path_stream;
        local_file_path_stream << local_shard_root_path << "/" << restore_request.tablet_id << "/";
        string local_file_path(local_file_path_stream.str());

        // Download files from remote source
        if (status_code == TStatusCode::OK) {
            string command = "sh " + config::trans_file_tool_path + " " + label + " download " +
                         local_file_path + " " + restore_request.remote_file_path +
                         " " + info_file_path;
            OLAP_LOG_INFO("Download cmd: %s", command.c_str());
            string errmsg;
            ret = worker_pool_this->_agent_utils->exec_cmd(command, &errmsg);
            if (!ret) {
                status_code = TStatusCode::RUNTIME_ERROR;
                error_msgs.push_back(errmsg);
                OLAP_LOG_WARNING("Download file failed. Error: %s", errmsg.c_str());
            }
        }

        // Delete tmp file
        boost::filesystem::path file_path(info_file_path);
        if (boost::filesystem::exists(file_path)) {
            boost::filesystem::remove_all(file_path);
        }

        // Change file name
        boost::filesystem::path blocal_file_path(local_file_path);
        if (status_code == TStatusCode::OK && boost::filesystem::exists(blocal_file_path)) {
            boost::filesystem::recursive_directory_iterator end_iter;
            for (boost::filesystem::recursive_directory_iterator file_path(blocal_file_path);
                    file_path != end_iter; ++file_path) {
                if (boost::filesystem::is_directory(*file_path)) {
                    continue;
                }

                // Check file name
                string file_path_str = file_path->path().string();
                string file_name = file_path_str;
                uint32_t slash_pos = file_path_str.rfind('/');
                if (slash_pos != -1) {
                    file_name = file_path_str.substr(slash_pos + 1);
                }
                uint32_t file_path_str_len = file_name.size();
                if (file_path_str_len <= 4) {
                    continue;
                }
                string file_path_suffix = file_name.substr(file_path_str_len - 4);
                if (file_path_suffix != ".hdr" && file_path_suffix != ".idx" &&
                        file_path_suffix != ".dat") {
                    continue;
                }

                // Get new file name
                stringstream new_file_name_stream;
                char sperator = '_';
                if (file_path_suffix == ".hdr") {
                    sperator = '.';
                }
                uint32_t sperator_pos = file_name.find(sperator);
                new_file_name_stream << restore_request.tablet_id << file_name.substr(sperator_pos);
                string new_file_path_str = file_path_str.substr(0, slash_pos) + "/" +
                                           new_file_name_stream.str();

                OLAP_LOG_INFO("change file name %s to %s",
                        file_path_str.c_str(), new_file_path_str.c_str());
                // Change file name to new one
                boost::filesystem::path new_file_path(new_file_path_str);
                boost::filesystem::rename(*file_path, new_file_path);
            }
        }

        // Load olap
        if (status_code == TStatusCode::OK) {
            OLAPStatus load_header_status =
                    worker_pool_this->_command_executor->load_header(
                            local_shard_root_path,
                            restore_request.tablet_id,
                            restore_request.schema_hash);
            if (load_header_status != OLAP_SUCCESS) {
                OLAP_LOG_WARNING("load header failed. local_shard_root_path: %s, tablet_id: %d "
                                 "schema_hash: %d, status: %d, signature: %ld",
                                 local_shard_root_path.c_str(), restore_request.tablet_id,
                                 restore_request.schema_hash, load_header_status,
                                 agent_task_req.signature);
                error_msgs.push_back("load header failed.");
                status_code = TStatusCode::RUNTIME_ERROR;;
            }
        }

        // Get tablets info
        vector<TTabletInfo> finish_tablet_infos;
        if (status_code == TStatusCode::OK) {
            TTabletInfo tablet_info;
            AgentStatus get_tablet_info_status = worker_pool_this->_get_tablet_info(
                    restore_request.tablet_id,
                    restore_request.schema_hash,
                    agent_task_req.signature,
                    &tablet_info);

            if (get_tablet_info_status != PALO_SUCCESS) {
                OLAP_LOG_WARNING("Restore success, but get new tablet info failed."
                                 "tablet_id: %ld, schema_hash: %ld, signature: %ld.",
                                 restore_request.tablet_id, restore_request.schema_hash,
                                 agent_task_req.signature);
            } else {
                finish_tablet_infos.push_back(tablet_info);
            }
        }

        task_status.__set_status_code(status_code);
        task_status.__set_error_msgs(error_msgs);

        TFinishTaskRequest finish_task_request;
        finish_task_request.__set_finish_tablet_infos(finish_tablet_infos);
        finish_task_request.__set_backend(worker_pool_this->_backend);
        finish_task_request.__set_task_type(agent_task_req.task_type);
        finish_task_request.__set_signature(agent_task_req.signature);
        finish_task_request.__set_task_status(task_status);

        worker_pool_this->_finish_task(finish_task_request);
        worker_pool_this->_remove_task_info(agent_task_req.task_type, agent_task_req.signature, "");
#ifndef BE_TEST
    }
#endif
    return (void*)0;
}

void* TaskWorkerPool::_make_snapshot_thread_callback(void* arg_this) {
    TaskWorkerPool* worker_pool_this = (TaskWorkerPool*)arg_this;

#ifndef BE_TEST
    while (true) {
#endif
        TAgentTaskRequest agent_task_req;
        TSnapshotRequest snapshot_request;
        {
            lock_guard<MutexLock> worker_thread_lock(worker_pool_this->_worker_thread_lock);
            while (worker_pool_this->_tasks.empty()) {
                 worker_pool_this->_worker_thread_condition_lock.wait();
            }

            agent_task_req = worker_pool_this->_tasks.front();
            snapshot_request = agent_task_req.snapshot_req;
            worker_pool_this->_tasks.pop_front();
        }
        // Try to register to cgroups_mgr
        CgroupsMgr::apply_system_cgroup();
        OLAP_LOG_INFO("get snapshot task, signature: %ld", agent_task_req.signature);

        TStatusCode::type status_code = TStatusCode::OK;
        vector<string> error_msgs;
        TStatus task_status;

        string snapshot_path;
        OLAPStatus make_snapshot_status = worker_pool_this->_command_executor->make_snapshot(
                snapshot_request, &snapshot_path);
        if (make_snapshot_status != OLAP_SUCCESS) {
            status_code = TStatusCode::RUNTIME_ERROR;
            OLAP_LOG_WARNING("make_snapshot failed. tablet_id: %ld, schema_hash: %ld, version: %d,"
                             "version_hash: %ld, status: %d",
                             snapshot_request.tablet_id, snapshot_request.schema_hash,
                             snapshot_request.version, snapshot_request.version_hash,
                             make_snapshot_status);
            error_msgs.push_back("make_snapshot failed. status: " +
                                 boost::lexical_cast<string>(make_snapshot_status));
        } else {
            OLAP_LOG_INFO("make_snapshot success. tablet_id: %ld, schema_hash: %ld, version: %d,"
                          "version_hash: %ld, snapshot_path: %s",
                          snapshot_request.tablet_id, snapshot_request.schema_hash,
                          snapshot_request.version, snapshot_request.version_hash,
                          snapshot_path.c_str());
        }

        task_status.__set_status_code(status_code);
        task_status.__set_error_msgs(error_msgs);

        TFinishTaskRequest finish_task_request;
        finish_task_request.__set_backend(worker_pool_this->_backend);
        finish_task_request.__set_task_type(agent_task_req.task_type);
        finish_task_request.__set_signature(agent_task_req.signature);
        finish_task_request.__set_snapshot_path(snapshot_path);
        finish_task_request.__set_task_status(task_status);

        worker_pool_this->_finish_task(finish_task_request);
        worker_pool_this->_remove_task_info(agent_task_req.task_type, agent_task_req.signature, "");
#ifndef BE_TEST
    }
#endif
    return (void*)0;
}

void* TaskWorkerPool::_release_snapshot_thread_callback(void* arg_this) {
    TaskWorkerPool* worker_pool_this = (TaskWorkerPool*)arg_this;

#ifndef BE_TEST
    while (true) {
#endif
        TAgentTaskRequest agent_task_req;
        TReleaseSnapshotRequest release_snapshot_request;
        {
            lock_guard<MutexLock> worker_thread_lock(worker_pool_this->_worker_thread_lock);
            while (worker_pool_this->_tasks.empty()) {
                worker_pool_this->_worker_thread_condition_lock.wait();
            }

            agent_task_req = worker_pool_this->_tasks.front();
            release_snapshot_request = agent_task_req.release_snapshot_req;
            worker_pool_this->_tasks.pop_front();
        }
        // Try to register to cgroups_mgr
        CgroupsMgr::apply_system_cgroup();
        OLAP_LOG_INFO("get release snapshot task, signature: %ld", agent_task_req.signature);

        TStatusCode::type status_code = TStatusCode::OK;
        vector<string> error_msgs;
        TStatus task_status;

        string& snapshot_path = release_snapshot_request.snapshot_path;
        OLAPStatus release_snapshot_status =
                worker_pool_this->_command_executor->release_snapshot(snapshot_path);
        if (release_snapshot_status != OLAP_SUCCESS) {
            status_code = TStatusCode::RUNTIME_ERROR;
            OLAP_LOG_WARNING("release_snapshot failed. snapshot_path: %s, status: %d",
                             snapshot_path.c_str(), release_snapshot_status);
            error_msgs.push_back("release_snapshot failed. status: " +
                                 boost::lexical_cast<string>(release_snapshot_status));
        } else {
            OLAP_LOG_INFO("release_snapshot success. snapshot_path: %s, status: %d",
                          snapshot_path.c_str(), release_snapshot_status);
        }

        task_status.__set_status_code(status_code);
        task_status.__set_error_msgs(error_msgs);

        TFinishTaskRequest finish_task_request;
        finish_task_request.__set_backend(worker_pool_this->_backend);
        finish_task_request.__set_task_type(agent_task_req.task_type);
        finish_task_request.__set_signature(agent_task_req.signature);
        finish_task_request.__set_task_status(task_status);

        worker_pool_this->_finish_task(finish_task_request);
        worker_pool_this->_remove_task_info(agent_task_req.task_type, agent_task_req.signature, "");
#ifndef BE_TEST
    }
#endif
    return (void*)0;
}

AlterTableStatus TaskWorkerPool::_show_alter_table_status(
        TTabletId tablet_id,
        TSchemaHash schema_hash) {
    AlterTableStatus alter_table_status =
            _command_executor->show_alter_table_status(tablet_id, schema_hash);
    return alter_table_status;
}

AgentStatus TaskWorkerPool::_drop_table(const TDropTabletReq drop_tablet_req) {
    AgentStatus status = PALO_SUCCESS;
    OLAPStatus drop_status = _command_executor->drop_table(drop_tablet_req);
    if (drop_status != OLAPStatus::OLAP_SUCCESS) {
        status = PALO_ERROR;
    }
    return status;
}

AgentStatus TaskWorkerPool::_get_tablet_info(
        const TTabletId tablet_id,
        const TSchemaHash schema_hash,
        int64_t signature,
        TTabletInfo* tablet_info) {
    AgentStatus status = PALO_SUCCESS;

    tablet_info->__set_tablet_id(tablet_id);
    tablet_info->__set_schema_hash(schema_hash);
    OLAPStatus olap_status =
            _command_executor->report_tablet_info(tablet_info);
    if (olap_status != OLAP_SUCCESS) {
        OLAP_LOG_WARNING("get tablet info failed. status: %d, signature: %ld",
                         olap_status, signature);
        status = PALO_ERROR;
    }
    return status;
}
}  // namespace palo
