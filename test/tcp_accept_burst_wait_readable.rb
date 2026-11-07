# Connections that queue up before anyone accepts are all accepted, one
# wait_readable before each accept.
#
# Fifty clients connect before the acceptor runs, so the listening socket
# turns readable once for all of them. A wait_readable after the first
# accept has no new readiness to wait for and must still return, since
# the queue is not empty.
require "socket"
require "io/wait"

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
clients = Array.new(50) { TCPSocket.new("127.0.0.1", port) }

accepted = []
acceptor = Thread.new do
  50.times do
    server.wait_readable
    accepted << server.accept
  end
end

if acceptor.join(10)
  puts "accepted #{accepted.size}"
else
  puts "accept stalled after #{accepted.size}"
end
accepted.each(&:close)
clients.each(&:close)
server.close
