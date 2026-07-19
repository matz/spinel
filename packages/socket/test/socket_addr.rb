require "socket"
# sockaddr_in packs a 16-byte AF_INET sockaddr; unpack recovers host+port.
sa = Socket.sockaddr_in(8080, "127.0.0.1")
p sa.bytes.length
p Socket.unpack_sockaddr_in_port(sa)
p Socket.unpack_sockaddr_in(sa)
# Addrinfo from a packed sockaddr exposes family/socktype/protocol.
ai = Addrinfo.tcp("127.0.0.1", 8080)
p ai.family
p ai.socktype
p ai.ip_port
p ai.ip_address
p ai.ipv4?
# getaddrinfo returns binary sockaddrs we can unpack.
res = Socket.getaddrinfo("127.0.0.1", 8080, Socket::AF_INET, :STREAM)
p res.length
p Socket.unpack_sockaddr_in(res[0])
p Socket.unpack_sockaddr_in_port(res[0])
