# TCPServer.new on a port in use raises Errno::EADDRINUSE, the errno of its
# bind, not a refused connection; a refused connect stays ECONNREFUSED (#11351)
require "socket"
a = TCPServer.new("127.0.0.1", 0)
port = a.addr[1]
begin
  TCPServer.new("127.0.0.1", port)
rescue SystemCallError => e
  p e.class
  puts e.message.sub(/ - .*/, "")
end
a.close
begin
  TCPSocket.new("127.0.0.1", port)
rescue SystemCallError => e
  p e.class
end
