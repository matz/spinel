# A socket with its writing side shut down, reopened onto another socket,
# takes that socket's state and writes again.
require "socket"

def try(label)
  r = yield
  puts "#{label}: #{r.inspect}"
rescue => e
  puts "#{label}: #{e.class}: #{e.message}"
end

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
c1 = TCPSocket.new("127.0.0.1", port)
s1 = server.accept
c2 = TCPSocket.new("127.0.0.1", port)
s2 = server.accept
s1.close_write
try("write before reopen") { s1.write("x") }
s1.reopen(s2)
try("write after reopen, read by the peer") { s1.write("y"); c2.readpartial(10) }
s1.close
s2.close
c1.close
c2.close
server.close
