# An Errno from a socket call with an address names the address as CRuby
# does ("Address already in use - bind(2) for \"127.0.0.1\" port 80"): the
# message read "Address already in use @ bind - 127.0.0.1".
require "socket"
require "tmpdir"

def attempt(port = nil)
  yield
  :ok
rescue SystemCallError => e
  m = "#{e.class}: #{e.message}"
  port ? m.sub(port.to_s, "PORT") : m
end

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
closed = TCPServer.new("127.0.0.1", 0)
closed_port = closed.addr[1]
closed.close

udp = UDPSocket.new
udp.bind("127.0.0.1", 0)
udp_port = udp.addr[1]
p attempt(udp_port) { UDPSocket.new.bind("127.0.0.1", udp_port) }
p attempt { udp.send("x" * 100_000, 0, "127.0.0.1", 9) }

s = Socket.new(Socket::AF_INET, Socket::SOCK_STREAM)
p attempt(port) { s.bind(Socket.sockaddr_in(port, "127.0.0.1")) }
s.bind(Socket.sockaddr_in(0, "127.0.0.1"))
p attempt { s.bind(Socket.sockaddr_in(0, "127.0.0.1")) }
p attempt { Socket.new(Socket::AF_INET, Socket::SOCK_STREAM).bind("") }
p attempt(closed_port) { Socket.new(Socket::AF_INET, Socket::SOCK_STREAM).connect(Socket.sockaddr_in(closed_port, "127.0.0.1")) }

p attempt(port) { TCPServer.new("127.0.0.1", port) }
any = TCPServer.new(0)
p attempt(any.addr[1]) { TCPServer.new(any.addr[1]) }
any.close
p attempt(closed_port) { TCPSocket.new("127.0.0.1", closed_port) }
p attempt(closed_port) { TCPSocket.new("127.0.0.1", closed_port, connect_timeout: 5) }
p attempt(closed_port) { Socket.tcp("127.0.0.1", closed_port) }
p attempt { Socket.new(Socket::AF_INET, Socket::SOCK_STREAM).connect_nonblock("") }

p attempt { UNIXSocket.new("/nonexistent/sock") }
p attempt { Socket.new(Socket::AF_UNIX, Socket::SOCK_STREAM).connect(Socket.sockaddr_un("/nonexistent dir/s")) }
p attempt { Socket.new(Socket::AF_UNIX, Socket::SOCK_STREAM).connect(Socket.sockaddr_un("nonexistent_rel/s")) }
path = File.join(Dir.tmpdir, "spinel_errno_addr_#{Process.pid}")
File.write(path, "")
p attempt { UNIXServer.new(path) }.sub(path, "PATH")
File.delete(path)

s.close
udp.close
server.close
