// Copyright (C) 2022 Intel Corporation
// SPDX-License-Identifier: MIT
#include "ProcessSpawnSink.h"
#include <Core/source/gfx/base/ComPtr.h>
#include <Core/source/infra/log/Logging.h>
#include <Core/source/infra/log/v/ProcWatch.h>
#include <format>

using p2c::infra::log::v::procwatch;

namespace p2c::win::com
{
    ProcessSpawnSink::ProcessSpawnSink(EventQueue& queue, float delayToleranceSeconds)
        :
        delayToleranceSeconds_{ delayToleranceSeconds },
        eventQueue_{ queue }
    {}
    HRESULT STDMETHODCALLTYPE ProcessSpawnSink::Indicate(LONG count,
        IWbemClassObject __RPC_FAR* __RPC_FAR* pObjArr)
    {
        using gfx::ComPtr;
        for (int i = 0; i < count; i++)
        {
            // get wbem interface for process info
            ComPtr<IWbemClassObject> pProcInfo;
            {
                // read out pointer to process information
                VARIANT varTgtObj;
                if (auto hr = pObjArr[i]->Get(
                    L"TargetInstance",
                    0,
                    &varTgtObj,
                    nullptr,
                    nullptr
                ); FAILED(hr))
                {
                    p2clog.warn(L"Failed to read wbem event").commit();
                    continue;
                }
                // query for actual wbem inteface
                if (auto hr = varTgtObj.punkVal->QueryInterface<IWbemClassObject>(&pProcInfo);
                    FAILED(hr))
                {
                    p2clog.warn(L"Failed to query interface for process info").commit();
                    continue;
                }
                // release original interface reference
                varTgtObj.punkVal->Release();
            }
            // read process parent id
            DWORD parentPid;
            {
                VARIANT varParentPid;
                if (auto hr = pProcInfo->Get(
                    L"ParentProcessId",
                    0,
                    &varParentPid,
                    nullptr,
                    nullptr
                ); FAILED(hr))
                {
                    p2clog.warn(L"Failed to read wbem parent pid").commit();
                    continue;
                }
                parentPid = varParentPid.uintVal;
            }
            // read process id
            DWORD pid;
            {
                VARIANT varPid;
                if (auto hr = pProcInfo->Get(
                    L"ProcessId",
                    0,
                    &varPid,
                    nullptr,
                    nullptr
                ); FAILED(hr))
                {
                    p2clog.warn(L"Failed to read wbem pid").commit();
                    continue;
                }
                pid = varPid.uintVal;
            }
            // read process name
            std::wstring name;
            {
                VARIANT varName;
                if (auto hr = pProcInfo->Get(
                    L"Name",
                    0,
                    &varName,
                    nullptr,
                    nullptr
                ); FAILED(hr))
                {
                    p2clog.warn(L"Failed to read wbem proc name").commit();
                }
                else if (varName.bstrVal) {
                    name = varName.bstrVal;
                }
            }

            p2cvlog(procwatch).note(std::format(L"proc-spawn event | pid:{:5} par:{:5} nam:{}", pid, parentPid, name)).commit();

            // queue notification for handling on kernel thread
            eventQueue_.Push({
                .pid = pid,
                .parentId = parentPid,
                .name = std::move(name),
            });
        }
        if (count > 0) {
            eventQueue_.Notify();
        }
        return WBEM_S_NO_ERROR;
    }
    std::string ProcessSpawnSink::GetQueryString() const
    {
        return std::format(
            "SELECT * "
            "FROM __InstanceCreationEvent WITHIN {:.2f} "
            "WHERE TargetInstance ISA 'Win32_Process'",
            delayToleranceSeconds_
        );
    }



    // event queue functions

    ProcessSpawnSink::EventQueue::EventQueue(std::function<void()> notification)
        :
        notificationFunction_{ std::move(notification) }
    {}

    std::optional<Process> ProcessSpawnSink::EventQueue::Pop()
    {
        std::lock_guard lk{ mtx_ };
        if (!processSpawnEvents_.empty()) {
            auto spawn = std::make_optional(processSpawnEvents_.front());
            processSpawnEvents_.pop_front();
            return spawn;
        }
        else {
            return {};
        }
    }

    void ProcessSpawnSink::EventQueue::Push(Process proc)
    {
        std::lock_guard lk{ mtx_ };
        processSpawnEvents_.push_back(std::move(proc));
    }

    void ProcessSpawnSink::EventQueue::Notify() const
    {
        if (notificationFunction_) {
            notificationFunction_();
        }
    }
}