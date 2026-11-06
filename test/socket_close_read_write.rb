# close_write on a connected socket shuts its writing side down and keeps
# it readable: the peer reads to the end, and the socket still reads what
# the peer sends. A write after it raises; calling it again answers nil.
# With both sides down the socket closes. close_read alone leaves the
# socket writable.
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
s.write("answer")
try("close_write") { s.close_write }
try("peer reads to the end") { c.read }
c.write("late bytes")
c.close_write
try("still reads") { s.read }
try("write after close_write") { s.write("x") }
try("close_write again") { s.close_write }
try("closed? with one side down") { s.closed? }
try("close_read") { s.close_read }
try("closed? with both down") { s.closed? }
try("close_write once closed") { s.close_write }

c2 = TCPSocket.new("127.0.0.1", server.addr[1])
s2 = server.accept
try("close_read first") { s2.close_read }
try("write after close_read") { s2.write("y") }
try("peer reads it") { c2.readpartial(1) }
try("closed? after close_read") { s2.closed? }
s2.close
c.close
c2.close
server.close
