# #reopen of a TCP socket onto a file reads the file from its start: the
# bytes the socket's buffer still held were the socket's, and go with it.
require "socket"
require "tmpdir"

path = File.join(Dir.tmpdir, "spinel_socket_reopen_#{Process.pid}.txt")
File.write(path, "from-file\n")
server = TCPServer.new("127.0.0.1", 0)
a = TCPSocket.new("127.0.0.1", server.addr[1])
b = server.accept
b.write("line1\nline2\n")
sleep 0.1
p a.gets
a.reopen(path)
p a.gets
a.close
b.close
server.close
File.delete(path)
