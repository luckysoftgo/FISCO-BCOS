/**
 * @CopyRight:
 * FISCO-BCOS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * FISCO-BCOS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with FISCO-BCOS.  If not, see <http://www.gnu.org/licenses/>
 * (c) 2016-2018 fisco-dev contributors.
 *
 * @file ModularServer.h
 * @author: caryliao
 * @date 2018-10-26
 */

#pragma once
#include "StatisticProtocolServer.h"
#include <jsonrpccpp/common/exception.h>
#include <jsonrpccpp/common/procedure.h>
#include <jsonrpccpp/server/abstractserverconnector.h>
#include <jsonrpccpp/server/iprocedureinvokationhandler.h>
#include <jsonrpccpp/server/requesthandlerfactory.h>
#include <libstat/ChannelNetworkStatHandler.h>
#include <boost/throw_exception.hpp>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace dev
{
template <class I>
using AbstractMethodPointer = void (I::*)(Json::Value const& _parameter, Json::Value& _result);
template <class I>
using AbstractNotificationPointer = void (I::*)(Json::Value const& _parameter);

template <class I>
class ServerInterface
{
public:
    using MethodPointer = AbstractMethodPointer<I>;
    using NotificationPointer = AbstractNotificationPointer<I>;

    using MethodBinding = std::tuple<jsonrpc::Procedure, AbstractMethodPointer<I>>;
    using NotificationBinding = std::tuple<jsonrpc::Procedure, AbstractNotificationPointer<I>>;
    using Methods = std::vector<MethodBinding>;
    using Notifications = std::vector<NotificationBinding>;
    struct RPCModule
    {
        std::string name;
        std::string version;
    };
    using RPCModules = std::vector<RPCModule>;

    virtual ~ServerInterface() {}
    Methods const& methods() const { return m_methods; }
    Notifications const& notifications() const { return m_notifications; }
    /// @returns which interfaces (eth, admin, db, ...) this class implements in which version.
    virtual RPCModules implementedModules() const = 0;

protected:
    void bindAndAddMethod(jsonrpc::Procedure const& _proc, MethodPointer _pointer)
    {
        m_methods.emplace_back(_proc, _pointer);
    }
    void bindAndAddNotification(jsonrpc::Procedure const& _proc, NotificationPointer _pointer)
    {
        m_notifications.emplace_back(_proc, _pointer);
    }

private:
    Methods m_methods;
    Notifications m_notifications;
};

template <class... Is>
class ModularServer : public jsonrpc::IProcedureInvokationHandler
{
public:
    ModularServer() : m_handler(new StatisticProtocolServer(*this))
    {
        m_handler->AddProcedure(jsonrpc::Procedure(
            "rpc_modules", jsonrpc::PARAMS_BY_POSITION, jsonrpc::JSON_OBJECT, NULL));
        m_implementedModules = Json::objectValue;
    }
    inline virtual void modules(const Json::Value& request, Json::Value& response)
    {
        (void)request;
        response = m_implementedModules;
    }

    virtual ~ModularServer() { StopListening(); }

    virtual bool StartListening()
    {
        for (auto const& connector : m_connectors)
        {
            if (false == connector->StartListening())
                return false;  // start failed
        }

        return true;
    }

    virtual void StopListening()
    {
        for (auto const& connector : m_connectors)
            connector->StopListening();
    }

    virtual void HandleMethodCall(
        jsonrpc::Procedure& _proc, Json::Value const& _input, Json::Value& _output) override
    {
        if (_proc.GetProcedureName() == "rpc_modules")
            modules(_input, _output);
    }

    virtual void HandleNotificationCall(
        jsonrpc::Procedure& _proc, Json::Value const& _input) override
    {
        (void)_proc;
        (void)_input;
    }

    /// server takes ownership of the connector
    unsigned addConnector(jsonrpc::AbstractServerConnector* _connector)
    {
        m_connectors.emplace_back(_connector);
        _connector->SetHandler(m_handler.get());
        return m_connectors.size() - 1;
    }

    jsonrpc::AbstractServerConnector* connector(unsigned _i) const
    {
        return m_connectors.at(_i).get();
    }

protected:
    std::vector<std::unique_ptr<jsonrpc::AbstractServerConnector>> m_connectors;
    std::unique_ptr<StatisticProtocolServer> m_handler;
    /// Mapping for implemented modules, to be filled by subclasses during construction.
    Json::Value m_implementedModules;
};

template <class I, class... Is>
class ModularServer<I, Is...> : public ModularServer<Is...>
{
public:
    using MethodPointer = AbstractMethodPointer<I>;
    using NotificationPointer = AbstractNotificationPointer<I>;

    ModularServer<I, Is...>(I* _i, Is*... _is) : ModularServer<Is...>(_is...), m_interface(_i)
    {
        if (!m_interface)
            return;
        for (auto const& method : m_interface->methods())
        {
            m_methods[std::get<0>(method).GetProcedureName()] = std::get<1>(method);
            this->m_handler->AddProcedure(std::get<0>(method));
        }

        for (auto const& notification : m_interface->notifications())
        {
            m_notifications[std::get<0>(notification).GetProcedureName()] =
                std::get<1>(notification);
            this->m_handler->AddProcedure(std::get<0>(notification));
        }
        // Store module with version.
        for (auto const& module : m_interface->implementedModules())
            this->m_implementedModules[module.name] = module.version;
    }

    virtual void HandleMethodCall(
        jsonrpc::Procedure& _proc, Json::Value const& _input, Json::Value& _output) override
    {
        auto procedureName = _proc.GetProcedureName();
        auto pointer = m_methods.find(procedureName);
        if (pointer != m_methods.end())
        {
            try
            {
                (m_interface.get()->*(pointer->second))(_input, _output);
            }
            catch (jsonrpc::JsonRpcException& e)
            {
                BOOST_THROW_EXCEPTION(jsonrpc::JsonRpcException(e.GetCode(), e.GetMessage()));
            }
            catch (std::exception& e)
            {
                BOOST_THROW_EXCEPTION(
                    jsonrpc::JsonRpcException(jsonrpc::Errors::ERROR_RPC_INVALID_PARAMS));
            }
        }
        else
        {
            ModularServer<Is...>::HandleMethodCall(_proc, _input, _output);
        }
    }

    virtual void HandleNotificationCall(
        jsonrpc::Procedure& _proc, Json::Value const& _input) override
    {
        auto pointer = m_notifications.find(_proc.GetProcedureName());
        if (pointer != m_notifications.end())
            (m_interface.get()->*(pointer->second))(_input);
        else
            ModularServer<Is...>::HandleNotificationCall(_proc, _input);
    }

    void setNetworkStatHandler(dev::stat::ChannelNetworkStatHandler::Ptr _handler)
    {
        this->m_handler->setNetworkStatHandler(_handler);
    }

    void setQPSLimiter(dev::flowlimit::RPCQPSLimiter::Ptr _qpsLimiter)
    {
        this->m_handler->setQPSLimiter(_qpsLimiter);
    }

private:
    std::unique_ptr<I> m_interface;
    std::map<std::string, MethodPointer> m_methods;
    std::map<std::string, NotificationPointer> m_notifications;
};
}  // namespace dev
