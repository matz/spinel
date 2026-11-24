# TCPServer.new(nil, port) binds the wildcard address, and a port in use
# raises EADDRINUSE with "bind(2) for nil port N".
require "socket"

srv = TCPServer.new(nil, 0)
port = srv.addr[1]
p ["AF_INET", "AF_INET6"].include?(srv.addr[0])
c = TCPSocket.new("127.0.0.1", port)
a = srv.accept
c.write("ab")
p a.read(2)
c.close
a.close

[nil, [nil, "x"].first, [nil, "x", 1].first].each do |h|
  TCPServer.new(h, port)
rescue SystemCallError => e
  p e.class
  puts e.message.split(" - ", 2).last.sub(port.to_s, "PORT")
end
srv.close

begin
  TCPServer.new([true, 1].first, 0)
rescue TypeError => e
  p e.message
end
