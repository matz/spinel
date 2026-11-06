# A dup of a socket whose writing side is shut down shares that state: a
# write through the copy raises, and the copy still reads.
require "socket"

def try(label)
  r = yield
  puts "#{label}: #{r.inspect}"
rescue => e
  puts "#{label}: #{e.class}: #{e.message}"
end

server = TCPServer.new("127.0.0.1", 0)
c = TCPSocket.new("127.0.0.1", server.addr[1])
s = server.accept
s.close_write
d = s.dup
try("write through the dup") { d.write("x") }
try("syswrite through the dup") { d.syswrite("x") }
c.write("hi")
try("the dup reads") { d.readpartial(10) }
d.close
s.close
c.close
server.close
