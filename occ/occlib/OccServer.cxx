/*
 * === This file is part of ALICE O² ===
 *
 * Copyright 2018 CERN and copyright holders of ALICE O².
 * Author: Teo Mrnjavac <teo.mrnjavac@cern.ch>
 *         Sylvain Chapeland <sylvain.chapeland@cern.ch>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * In applying this license CERN does not waive the privileges and
 * immunities granted to it by virtue of its status as an
 * Intergovernmental Organization or submit itself to any jurisdiction.
 */


#include "OccServer.h"

#include "util/Defer.h"
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/algorithm/string.hpp>

#include "RuntimeControlledObject.h"

using namespace std::chrono_literals;

OccServer::OccServer(RuntimeControlledObject* rco)
    : Service()
    , m_rco(rco)
    , m_destroying(false)
    , m_machineDone(false)
{
    m_checkerThread = std::thread(&OccServer::runChecker, this);
    m_rco->setState(t_State::standby);
}

OccServer::~OccServer()
{
    m_destroying = true;
    m_checkerThread.join();
}

grpc::Status OccServer::StateStream(grpc::ServerContext* context,
                                    const pb::StateStreamRequest* request,
                                    grpc::ServerWriter<pb::StateStreamReply>* writer)
{
    std::cout << "[request StateStream] handler BEGIN" << std::endl;

    (void) context;
    (void) request;

    boost::uuids::basic_random_generator<boost::mt19937> gen;
    std::string id = boost::uuids::to_string(gen());

    boost::lockfree::queue<t_State> stateQueue;
    m_stateQueues[id] = &stateQueue;
    DEFER({
        m_stateQueues.erase(id);
    });

    bool isStreamOpen = true;
    while (!m_destroying && isStreamOpen) {
        t_State newState;
        bool ok = stateQueue.pop(newState);
        if (!ok) {  // queue empty, sleep and retry
            std::this_thread::sleep_for(2ms);
            continue;
        }

        pb::StateStreamReply response;
        response.set_type(pb::STATE_STABLE);
        response.set_state(getStringFromState(newState));

        if (newState != t_State::done) {
            isStreamOpen = writer->Write(response);
        } else { // we're about to shut down, better close the StateStream writer
            writer->WriteLast(response, grpc::WriteOptions());
            isStreamOpen = false;
        }
    }

    std::cout << "[request StateStream] handler END" << std::endl;
    return grpc::Status::OK;
}

grpc::Status OccServer::GetState(grpc::ServerContext* context,
                                 const pb::GetStateRequest* request,
                                 pb::GetStateReply* response)
{
    std::lock_guard<std::mutex> lock(m_mu);
    std::cout << "[request GetState] handler BEGIN" << std::endl;

    (void) context;
    (void) request;

    auto state = getStringFromState(m_rco->getState());
    response->set_state(state);

    std::cout << "[request GetState] handler END" << std::endl;
    return grpc::Status::OK;
}

/**
 * Transition requests a state transition from the controllable object, and blocks until success or failure.
 *
 * @param context server context
 * @param request the request, as generated and wrapped by Protobuf
 * @param response the response, as generated and wrapped by Protobuf
 * @return the status, either grpc::Status::OK or an error status
 */
grpc::Status OccServer::Transition(grpc::ServerContext* context,
                                   const pb::TransitionRequest* request,
                                   pb::TransitionReply* response)
{
    std::lock_guard<std::mutex> lock(m_mu);
    std::cout << "[request Transition] handler BEGIN" << std::endl;

    (void) context;
    if (!request) {
        return grpc::Status(grpc::INVALID_ARGUMENT, "null request received");
    }

    std::string srcStateStr = request->srcstate();
    std::string event = request->event();
    auto arguments = request->arguments();
    const std::string finalState = EXPECTED_FINAL_STATE.at(request->event());

    t_State currentState = m_rco->getState();
    std::string currentStateStr = getStringFromState(currentState);
    if (srcStateStr != currentStateStr) {
        return grpc::Status(grpc::INVALID_ARGUMENT,
                            "transition not possible: state mismatch: source: " + srcStateStr + " current: " +
                            currentStateStr);
    }
    if (currentState == t_State::done) {
        return grpc::Status(grpc::FAILED_PRECONDITION,
                            "transition not possible: current state: " + currentStateStr);
    }

    std::cout << "[request Transition] src: " << srcStateStr
              << " currentState: " << currentStateStr
              << " event: " << event << std::endl;

    PropertyMap properties;
    for (auto item : arguments) {
        properties[item.key()] = item.value();
    }

    t_State newState        = processStateTransition(event, properties);
    std::string newStateStr = getStringFromState(newState);

    response->set_state(newStateStr);
    response->set_event(request->event());
    response->set_ok(newStateStr == finalState);
    if (newState == error) {                   // ERROR state
        response->set_trigger(pb::DEVICE_ERROR);
    } else if (newStateStr == finalState) {    // correct destination state
        response->set_trigger(pb::EXECUTOR);
    } else {                                   // some other state, for whatever reason - we assume DEVICE_INTENTIONAL
        response->set_trigger(pb::DEVICE_INTENTIONAL);
    }

    std::cout << "[request Transition] handler END, new state: " << newStateStr << std::endl;
    return grpc::Status::OK;
}

t_State OccServer::processStateTransition(const std::string& event, const PropertyMap& properties)
{
    int err = 0;
    int invalidEvent = 0;

    t_State currentState = m_rco->getState();
    t_State newState = currentState;

    std::string evt = boost::algorithm::to_lower_copy(event);

    printf("Object: %s - processing event %s in state %s\n",
        m_rco->getName().c_str(),
        evt.c_str(),
        getStringFromState(currentState).c_str());

    // STANDBY
    if (currentState==t_State::standby) {
        if (evt=="configure") {
            err = m_rco->executeConfigure(properties);
            if (!err) {
                newState = t_State::configured;
            } else {
                newState = t_State::error;
            }
        } else if (evt=="exit") {
            err = m_rco->executeExit();
            if (!err) {
                newState = t_State::done;
            } else {
                newState = t_State::error;
            }
        } else {
            invalidEvent=1;
        }

    // CONFIGURED
    } else if (currentState==t_State::configured) {
        if (evt=="start") {
            err=m_rco->executeStart();
            if (!err) {
                newState=t_State::running;
            } else {
                newState=t_State::error;
            }
        } else if (evt=="reset") {
            err=m_rco->executeReset();
            if (!err) {
                newState=t_State::standby;
            } else {
                newState=t_State::error;
            }
        } else if (evt=="exit") {
            err = m_rco->executeExit();
            if (!err) {
                newState = t_State::done;
            } else {
                newState = t_State::error;
            }
        } else {
            invalidEvent=1;
        }

    // RUNNING
    } else if (currentState==t_State::running) {
        if (evt=="stop") {
            err=m_rco->executeStop();
            if (!err) {
                newState=t_State::configured;
            } else {
                newState=t_State::error;
            }
        } else if (evt=="pause") {
            err=m_rco->executePause();
            if (!err) {
                newState=t_State::paused;
            } else {
                newState=t_State::error;
            }
        } else {
            invalidEvent=1;
        }

    // PAUSED
    } else if (currentState==t_State::paused) {
        if (evt=="resume") {
            err=m_rco->executeResume();
            if (!err) {
                newState=t_State::running;
            } else {
                newState=t_State::error;
            }
        } else if (evt=="stop") {
            err=m_rco->executeStop();
            if (!err) {
                newState=t_State::configured;
            } else {
                newState=t_State::error;
            }
        } else {
            invalidEvent=1;
        }

    // ERROR
    } else if (currentState==t_State::error) {
        if (evt=="recover") {
            err=m_rco->executeRecover();
            if (!err) {
                newState=t_State::standby;
            } else {
                newState=t_State::error;
            }
        } else {
            invalidEvent=1;
        }

    // other
    } else {
        invalidEvent=1;
    }

    if (invalidEvent) {
        printf("Object: %s - invalid event %s received in state %s\n",
            m_rco->getName().c_str(),
            evt.c_str(),
            getStringFromState(currentState).c_str());
    } else {
        printf("Object: %s - event %s processed in state %s. New state: %s\n",
            m_rco->getName().c_str(),
            evt.c_str(),
            getStringFromState(currentState).c_str(),
            getStringFromState(newState).c_str());
        updateState(newState);
    }

    return newState;
}

void OccServer::updateState(t_State s)
{
    publishState(s);
    // todo: check if error
    m_rco->setState(s);
    printf("Object: %s - updating state = %s\n",
        m_rco->getName().c_str(),
        getStringFromState(s).c_str());
}

void OccServer::publishState(t_State s)
{
    for (auto item : m_stateQueues) {
        item.second->push(s);
    }
}

bool OccServer::checkMachineDone()
{
    std::lock_guard<std::mutex> lock(m_mu);
    return m_machineDone;
}

void OccServer::runChecker()
{
    while (!m_destroying) {
        m_mu.lock();

        t_State currentState = m_rco->getState();
        // check for final state reached
        if (currentState == t_State::done)
        {
            m_machineDone = true;
        }

        // execute periodic actions, as defined for t_State::running
        if (currentState == t_State::running) {
            int err = m_rco->iterateRunning();
            if (err) {
                updateState(t_State::error);
            }
        }

        // execute periodic check, in any state
        int err = m_rco->iterateCheck();
        if (err) {
            updateState(t_State::error);
        }

        m_mu.unlock();

        if (!m_destroying) {
            std::this_thread::sleep_for(1ms);
        }
    }
}