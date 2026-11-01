/* GPL-3.0-or-later; Copyright (C) 2026 alambe94. */
#include <config.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#endif
#include <string.h>
#include <glib.h>
#include "network_discovery.h"

#define xSDK_xLOGIC_MDNS_INTERFACES 32U
#define xSDK_xLOGIC_MDNS_DURATION   1200000
#ifdef _WIN32
typedef SOCKET mdns_socket;
#define xSDK_xLOGIC_MDNS_INVALID INVALID_SOCKET
#else
typedef int mdns_socket;
#define xSDK_xLOGIC_MDNS_INVALID (-1)
#endif

struct mdns_interface
{
    mdns_socket socket;
    uint32_t address, mask;
};
// Explicit per-call heap workspace. No DNS state survives Scan.
struct mdns_workspace
{
    struct xsdk_xlogic_discovery_cache cache;
    struct mdns_interface interfaces[xSDK_xLOGIC_MDNS_INTERFACES];
    size_t count;
    gint64 deadline;
    uint16_t id;
    uint8_t packet[1500];
    struct
    {
        xNET_DNS_Wire_Name_t name;
        uint16_t interface_index, type;
    } queries[xSDK_xLOGIC_DISCOVERY_RECORDS];
    size_t query_count;
};

static void socket_close(mdns_socket socket)
{
#ifdef _WIN32
    if (closesocket(socket) != 0)
#else
    if (close(socket) != 0)
#endif
    {
        g_warning("xLOGIC discovery socket release failed.");
    }
}

static int interface_multicast(mdns_socket socket, uint32_t address)
{
    struct in_addr multicast;
    int ttl = 255;
    int result;
    multicast.s_addr = address;
    result = setsockopt(socket, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&multicast, sizeof(multicast));
    if (result != 0)
    {
        return result;
    }
    return setsockopt(socket, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof(ttl));
}

static void interface_add(struct mdns_workspace *workspace, uint32_t address, uint32_t mask)
{
    struct mdns_interface *nic;
    struct sockaddr_in local;
    size_t index;
    int result;
#ifdef _WIN32
    u_long nonblocking = 1;
#endif
    if (!address || !mask || g_get_monotonic_time() >= workspace->deadline)
    {
        return;
    }
    for (index = 0; index < workspace->count; index++)
    {
        if (workspace->interfaces[index].address == address)
        {
            return;
        }
    }
    if (workspace->count == xSDK_xLOGIC_MDNS_INTERFACES)
    {
        g_warning("xLOGIC discovery interface capacity reached.");
        return;
    }
    nic = &workspace->interfaces[workspace->count];
    nic->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (nic->socket == xSDK_xLOGIC_MDNS_INVALID)
    {
        return;
    }
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = address;
    result = bind(nic->socket, (struct sockaddr *)&local, sizeof(local));
    if (result != 0)
    {
        goto failed;
    }
    result = interface_multicast(nic->socket, address);
    if (result != 0)
    {
        goto failed;
    }
#ifdef _WIN32
    result = ioctlsocket(nic->socket, FIONBIO, &nonblocking);
#else
    result = fcntl(nic->socket, F_SETFL, O_NONBLOCK);
#endif
    if (result != 0)
    {
        goto failed;
    }
    nic->address = address;
    nic->mask = mask;
    workspace->count++;
    return;
failed:
    socket_close(nic->socket);
    g_warning("xLOGIC discovery cannot initialize an IPv4 interface.");
}

static void interfaces_find(struct mdns_workspace *workspace)
{
#ifdef _WIN32
    INTERFACE_INFO interfaces[xSDK_xLOGIC_MDNS_INTERFACES];
    DWORD bytes = 0;
    size_t index;
    int result;
    u_long nonblocking = 1;
    mdns_socket probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe == xSDK_xLOGIC_MDNS_INVALID)
    {
        return;
    }
    result = ioctlsocket(probe, FIONBIO, &nonblocking);
    if (result != 0)
    {
        socket_close(probe);
        g_warning("xLOGIC discovery cannot initialize interface enumeration.");
        return;
    }
    result = WSAIoctl(probe, SIO_GET_INTERFACE_LIST, NULL, 0, interfaces, sizeof(interfaces), &bytes, NULL, NULL);
    socket_close(probe);
    if (result != 0 || bytes > sizeof(interfaces) || bytes % sizeof(interfaces[0]) != 0)
    {
        g_warning("xLOGIC discovery IPv4 interface enumeration failed or exceeded capacity.");
        return;
    }
    for (index = 0; index < bytes / sizeof(interfaces[0]); index++)
    {
        if ((interfaces[index].iiFlags & (IFF_UP | IFF_MULTICAST | IFF_LOOPBACK)) == (IFF_UP | IFF_MULTICAST))
        {
            interface_add(workspace, interfaces[index].iiAddress.AddressIn.sin_addr.s_addr,
                          interfaces[index].iiNetmask.AddressIn.sin_addr.s_addr);
        }
    }
#else
    struct ifaddrs *interfaces, *entry;
    int result = getifaddrs(&interfaces);
    if (result != 0)
    {
        g_warning("xLOGIC discovery IPv4 interface enumeration failed.");
        return;
    }
    for (entry = interfaces; entry; entry = entry->ifa_next)
    {
        if (!entry->ifa_addr || !entry->ifa_netmask || entry->ifa_addr->sa_family != AF_INET)
        {
            continue;
        }
        if ((entry->ifa_flags & (IFF_UP | IFF_MULTICAST | IFF_LOOPBACK)) == (IFF_UP | IFF_MULTICAST))
        {
            interface_add(workspace, ((struct sockaddr_in *)entry->ifa_addr)->sin_addr.s_addr,
                          ((struct sockaddr_in *)entry->ifa_netmask)->sin_addr.s_addr);
        }
    }
    freeifaddrs(interfaces);
#endif
}

static void packet_send(struct mdns_workspace *workspace, size_t length, size_t index)
{
    struct sockaddr_in destination;
    int sent;
    if (!length || g_get_monotonic_time() >= workspace->deadline)
    {
        return;
    }
    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_port = htons(5353);
    destination.sin_addr.s_addr = htonl(0xe00000fbU);
    sent = sendto(workspace->interfaces[index].socket, (const char *)workspace->packet, (int)length, 0, (struct sockaddr *)&destination,
                  sizeof(destination));
    if (sent != (int)length)
    {
        g_warning("xLOGIC discovery query send failed.");
    }
}

static void query_send(struct mdns_workspace *workspace, const char *name, uint16_t type, size_t index)
{
    size_t length = xsdk_xlogic_discovery_query(workspace->packet, sizeof(workspace->packet), workspace->id, name, type);
    packet_send(workspace, length, index);
}

static void query_followup(struct mdns_workspace *workspace, const xNET_DNS_Wire_Name_t *name, uint16_t type, uint16_t index)
{
    size_t previous, offset;
    for (previous = 0; previous < workspace->query_count; previous++)
    {
        if (workspace->queries[previous].interface_index == index && workspace->queries[previous].type == type &&
            xNET_DNS_Wire_Name_Equal(&workspace->queries[previous].name, name))
        {
            return;
        }
    }
    if (workspace->query_count == xSDK_xLOGIC_DISCOVERY_RECORDS)
    {
        return;
    }
    workspace->queries[previous].name = *name;
    workspace->queries[previous].type = type;
    workspace->queries[previous].interface_index = index;
    workspace->query_count++;
    memset(workspace->packet, 0, 12);
    workspace->packet[0] = (uint8_t)(workspace->id >> 8U);
    workspace->packet[1] = (uint8_t)workspace->id;
    workspace->packet[5] = 1;
    memcpy(workspace->packet + 12, name->wire, name->length);
    offset = 12 + name->length;
    workspace->packet[offset++] = (uint8_t)(type >> 8U);
    workspace->packet[offset++] = (uint8_t)type;
    workspace->packet[offset++] = 0;
    workspace->packet[offset++] = 1;
    packet_send(workspace, offset, index);
}

static void followups_send(struct mdns_workspace *workspace, uint64_t now)
{
    size_t index;
    for (index = 0; index < xSDK_xLOGIC_DISCOVERY_RECORDS; index++)
    {
        const struct xsdk_xlogic_discovery_record *record = &workspace->cache.records[index];
        if (record->expires <= now)
        {
            continue;
        }
        if (record->type == 12U)
        {
            query_followup(workspace, &record->target, 255, record->interface_index);
        }
        else if (record->type == 33U)
        {
            query_followup(workspace, &record->target, 1, record->interface_index);
        }
    }
}

static void receive_error(void)
{
#ifdef _WIN32
    int error = WSAGetLastError();
    if (error != WSAEWOULDBLOCK && error != WSAEMSGSIZE)
#else
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
#endif
    {
        g_warning("xLOGIC discovery socket receive failed.");
    }
}

static int packet_receive(struct mdns_workspace *workspace, mdns_socket socket, struct sockaddr_in *source)
{
#ifdef _WIN32
    int source_length = sizeof(*source);
    int length = recvfrom(socket, (char *)workspace->packet, sizeof(workspace->packet), 0, (struct sockaddr *)source, &source_length);
    if (source_length != sizeof(*source))
    {
        return 0;
    }
    return length;
#else
    struct iovec vector;
    struct msghdr message;
    ssize_t length;
    memset(&message, 0, sizeof(message));
    vector.iov_base = workspace->packet;
    vector.iov_len = sizeof(workspace->packet);
    message.msg_name = source;
    message.msg_namelen = sizeof(*source);
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    length = recvmsg(socket, &message, 0);
    if ((message.msg_flags & MSG_TRUNC) || message.msg_namelen != sizeof(*source))
    {
        return 0;
    }
    return (int)length;
#endif
}

static void receive_interface(struct mdns_workspace *workspace, size_t index, uint64_t now)
{
    const struct mdns_interface *nic = &workspace->interfaces[index];
    struct sockaddr_in source;
    size_t budget;
    for (budget = 0; budget < 8U; budget++)
    {
        if (g_get_monotonic_time() >= workspace->deadline)
        {
            return;
        }
        int length = packet_receive(workspace, nic->socket, &source);
        if (length <= 0)
        {
            if (length < 0)
            {
                receive_error();
            }
            return;
        }
        if (source.sin_family != AF_INET || source.sin_port != htons(5353))
        {
            continue;
        }
        if ((source.sin_addr.s_addr & nic->mask) != (nic->address & nic->mask))
        {
            continue;
        }
        // Matching NIC is part of every cache key; an A record on another link
        // cannot complete this service graph.
        if (!xsdk_xlogic_discovery_receive(&workspace->cache, workspace->packet, (size_t)length, workspace->id, (uint16_t)index, now))
        {
            continue;
        }
    }
}

static bool address_route(uint32_t address, uint32_t local)
{
    struct sockaddr_in remote, source;
    mdns_socket probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int result;
    bool matches = false;
#ifdef _WIN32
    int source_length = sizeof(source);
#else
    socklen_t source_length = sizeof(source);
#endif
    if (probe == xSDK_xLOGIC_MDNS_INVALID)
    {
        return false;
    }
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port = htons(9);
    remote.sin_addr.s_addr = address;
    // UDP connect only asks the kernel to choose a route; it emits no packet.
    result = connect(probe, (struct sockaddr *)&remote, sizeof(remote));
    if (result == 0)
    {
        result = getsockname(probe, (struct sockaddr *)&source, &source_length);
        if (result == 0 && source_length == sizeof(source) && source.sin_family == AF_INET)
        {
            matches = source.sin_addr.s_addr == local;
        }
    }
    socket_close(probe);
    return matches;
}

static void cache_onlink(struct mdns_workspace *workspace)
{
    size_t index;
    for (index = 0; index < xSDK_xLOGIC_DISCOVERY_RECORDS; index++)
    {
        struct xsdk_xlogic_discovery_record *record = &workspace->cache.records[index];
        const struct mdns_interface *nic = &workspace->interfaces[record->interface_index];
        const uint8_t *address = (const uint8_t *)&record->address;
        if (record->type != 1U)
        {
            continue;
        }
        if ((record->address & nic->mask) != (nic->address & nic->mask) || address[0] == 0U || address[0] == 127U || address[0] >= 224U ||
            record->address == nic->address)
        {
            record->expires = 0;
        }
        else if (!address_route(record->address, nic->address))
        {
            record->expires = 0;
        }
    }
}

static void browse_poll(struct mdns_workspace *workspace, const char *host, gint64 started)
{
    gint64 now, retry = started + 400000;
    size_t index;
    for (index = 0; index < workspace->count; index++)
    {
        query_send(workspace, host ? host : xSDK_xLOGIC_DISCOVERY_SERVICE, host ? 1U : 12U, index);
    }
    while (workspace->count)
    {
        now = g_get_monotonic_time();
        if (now - started >= xSDK_xLOGIC_MDNS_DURATION)
        {
            break;
        }
        for (index = 0; index < workspace->count; index++)
        {
            receive_interface(workspace, index, (uint64_t)now);
        }
        if (!host)
        {
            followups_send(workspace, (uint64_t)now);
        }
        if (now >= retry)
        {
            for (index = 0; index < workspace->count; index++)
            {
                query_send(workspace, host ? host : xSDK_xLOGIC_DISCOVERY_SERVICE, host ? 1U : 12U, index);
            }
            retry += 400000;
        }
        now = g_get_monotonic_time();
        if (now < started + xSDK_xLOGIC_MDNS_DURATION)
        {
            g_usleep((gulong)MIN(10000, started + xSDK_xLOGIC_MDNS_DURATION - now));
        }
    }
}

static void browse_close(struct mdns_workspace *workspace)
{
    size_t index;
    for (index = 0; index < workspace->count; index++)
    {
        socket_close(workspace->interfaces[index].socket);
    }
#ifdef _WIN32
    if (WSACleanup() != 0)
    {
        g_warning("xLOGIC discovery Winsock release failed.");
    }
#endif
}

static struct mdns_workspace *browse(const char *host)
{
    gint64 started = g_get_monotonic_time();
    struct mdns_workspace *workspace = g_try_new0(struct mdns_workspace, 1);
#ifdef _WIN32
    WSADATA startup;
    int result;
#endif
    if (!workspace)
    {
        return NULL;
    }
    workspace->deadline = started + xSDK_xLOGIC_MDNS_DURATION;
#ifdef _WIN32
    result = WSAStartup(MAKEWORD(2, 2), &startup);
    if (result != 0)
    {
        g_free(workspace);
        return NULL;
    }
#endif
    workspace->id = (uint16_t)g_random_int_range(1, 65536);
    interfaces_find(workspace);
    browse_poll(workspace, host, started);
    cache_onlink(workspace);
    browse_close(workspace);
    return workspace;
}

size_t xsdk_xlogic_mdns_browse(struct xsdk_xlogic_discovery_device *devices, size_t capacity)
{
    struct mdns_workspace *workspace = browse(NULL);
    size_t count;
    if (!workspace)
    {
        return 0;
    }
    count = xsdk_xlogic_discovery_devices(&workspace->cache, (uint64_t)g_get_monotonic_time(), devices, capacity);
    g_free(workspace);
    return count;
}

bool xsdk_xlogic_mdns_resolve(const char *host, char address[16])
{
    struct mdns_workspace *workspace = browse(host);
    uint32_t ipv4;
    const uint8_t *bytes = (const uint8_t *)&ipv4;
    bool found;
    if (!workspace)
    {
        return false;
    }
    found = xsdk_xlogic_discovery_address(&workspace->cache, host, (uint64_t)g_get_monotonic_time(), &ipv4);
    g_free(workspace);
    if (found)
    {
        g_snprintf(address, 16, "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
    }
    return found;
}
