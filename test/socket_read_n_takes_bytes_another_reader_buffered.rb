# spinel: not-cruby -- CRuby's read(n), once it waits on the descriptor,
# does not look at the bytes another thread's gets pulled into the buffer
# meanwhile: it answers "zz", the bytes after them.
#
# A read(n) waiting on the socket while another thread's gets brings more
# than the line into the socket's buffer takes those bytes next, in order,
# once it wakes; it does not wait past them for new ones.
ENV["SPINEL_WORKERS"] = "1"
require "socket"

srv = TCPServer.new("127.0.0.1", 0)
left = TCPSocket.new("127.0.0.1", srv.addr[1])
right = srv.accept
reader = Thread.new { right.read(10) }
sleep 0.2                      # the reader waits on the descriptor
left.write("x\n" + "y" * 10)   # one segment: gets takes it all into the buffer
p right.gets
left.write("zz")               # wakes the reader
sleep 0.2
p reader.value
p right.read(2)
left.close
right.close
srv.close
