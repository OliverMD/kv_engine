/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "testapp_client_test.h"
#include <protocol/connection/client_greenstack_connection.h>
#include <protocol/connection/client_mcbp_connection.h>

std::ostream& operator<<(std::ostream& os, const TransportProtocols& t) {
    os << to_string(t);
    return os;
}

std::string to_string(const TransportProtocols& transport) {
#ifdef JETBRAINS_CLION_IDE
    // CLion don't properly parse the output when the
    // output gets written as the string instead of the
    // number. This makes it harder to debug the tests
    // so let's just disable it while we're waiting
    // for them to supply a fix.
    // See https://youtrack.jetbrains.com/issue/CPP-6039
    return std::to_string(static_cast<int>(transport));
#else
    switch (transport) {
    case TransportProtocols::McbpPlain:
        return "Mcbp";
    case TransportProtocols::GreenstackPlain:
        return "Greenstack";
    case TransportProtocols::McbpIpv6Plain:
        return "McbpIpv6";
    case TransportProtocols::GreenstackIpv6Plain:
        return "GreenstackIpv6";
    case TransportProtocols::McbpSsl:
        return "McbpSsl";
    case TransportProtocols::GreenstackSsl:
        return "GreenstackSsl";
    case TransportProtocols::McbpIpv6Ssl:
        return "McbpIpv6Ssl";
    case TransportProtocols::GreenstackIpv6Ssl:
        return "GreenstackIpv6Ssl";
    }
    throw std::logic_error("Unknown transport");
#endif
}

MemcachedConnection& TestappClientTest::prepare(MemcachedConnection& connection) {
    connection.reconnect();
    if (connection.getProtocol() == Protocol::Memcached) {
        auto& c = dynamic_cast<MemcachedBinprotConnection&>(connection);
        c.setDatatypeSupport(true);
        c.setMutationSeqnoSupport(true);
    } else {
        auto& c = dynamic_cast<MemcachedGreenstackConnection&>(connection);
        c.hello("memcached_testapp", "1,0", "BucketTest");
    }
    return connection;
}

MemcachedConnection& TestappClientTest::getConnection() {
    switch (GetParam()) {
    case TransportProtocols::McbpPlain:
        return prepare(connectionMap.getConnection(Protocol::Memcached,
                                                   false, AF_INET));
    case TransportProtocols::GreenstackPlain:
        return prepare(connectionMap.getConnection(Protocol::Greenstack,
                                                   false, AF_INET));
    case TransportProtocols::McbpIpv6Plain:
        return prepare(connectionMap.getConnection(Protocol::Memcached,
                                                   false, AF_INET6));
    case TransportProtocols::GreenstackIpv6Plain:
        return prepare(connectionMap.getConnection(Protocol::Greenstack,
                                                   false, AF_INET6));
    case TransportProtocols::McbpSsl:
        return prepare(connectionMap.getConnection(Protocol::Memcached,
                                                   true, AF_INET));
    case TransportProtocols::GreenstackSsl:
        return prepare(connectionMap.getConnection(Protocol::Greenstack,
                                                   true, AF_INET));
    case TransportProtocols::McbpIpv6Ssl:
        return prepare(connectionMap.getConnection(Protocol::Memcached,
                                                   true, AF_INET6));
    case TransportProtocols::GreenstackIpv6Ssl:
        return prepare(connectionMap.getConnection(Protocol::Greenstack,
                                                   true, AF_INET6));
    }
    throw std::logic_error("Unknown transport");
}