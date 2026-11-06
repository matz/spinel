# TCPSocket.new(host, port, connect_timeout: seconds) reads its keyword: the
# seconds are evaluated once, after host and port, and nil is no limit. A
# connect that is answered or refused, or a name that does not resolve, is
# what it is without the keyword. The timeout itself, on a connect nothing
# answers, is in tcp_socket_connect_timeout_unanswered.
require "socket"

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]

# An answered connect: the seconds an Integer, a Float or nil, which the
# call learns only when it runs.
[5, 2.5, nil].each do |seconds|
  client = TCPSocket.new("127.0.0.1", port, connect_timeout: seconds)
  peer = server.accept
  client.puts "connect_timeout: #{seconds.inspect}"
  puts peer.gets
  peer.close
  client.close
end

# nil written in the call, and open, which without a block is new.
client = TCPSocket.new("127.0.0.1", port, connect_timeout: nil)
puts "nil: #{client.class}"
server.accept.close
client.close
client = TCPSocket.open("127.0.0.1", port, connect_timeout: 5)
puts "open: #{client.class}"
server.accept.close
client.close

# Host, port and the seconds are each evaluated once, in that order.
order = []
client = TCPSocket.new((order << "host"; "127.0.0.1"), (order << "port"; port),
                       connect_timeout: (order << "connect_timeout"; 2.5))
puts order.join(", ")
server.accept.close
client.close

# Nothing listens on the port now: the refusal is the one a connect without
# the keyword gets, and so is a name that does not resolve (an empty label,
# which the resolver rejects without a query).
server.close
begin
  TCPSocket.new("127.0.0.1", port, connect_timeout: 5)
  puts "connected"
rescue SystemCallError => e
  puts "refused: #{e.class}"
end
begin
  TCPSocket.new("a..b", port, connect_timeout: 5)
  puts "connected"
rescue SocketError => e
  puts "unresolved: #{e.class}"
end
