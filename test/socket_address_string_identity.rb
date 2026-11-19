# spinel: gc-minor
# A numeric address can be shared by two entries in the same socket row.
require "socket"

def check_address(row)
  host = row[2]
  address = row[3]
  p host.equal?(address)
  host << "!" * 256
  p [host.bytesize, row[2].bytesize, row[3].bytesize]
end

rows = Socket.getaddrinfo("127.0.0.1", 80, Socket::AF_INET, Socket::SOCK_STREAM)
check_address(rows.first)
p Socket.getaddrinfo("127.0.0.1", 80, Socket::AF_INET, Socket::SOCK_STREAM).first[3]
unpacked = Socket.unpack_sockaddr_in(Socket.pack_sockaddr_in(80, "127.0.0.1"))
host = unpacked[1]
host << "!"
p [unpacked[0], unpacked[1] == host]
server = TCPServer.new("127.0.0.1", 0)
check_address(server.addr(false))
p server.addr(false)[3]
server.close
