require "socket"
path = "/tmp/spinel_socket_test_#{Process.pid}.sock"
begin
  File.unlink(path) if File.exist?(path)
rescue
end
srv = UNIXServer.new(path)
c = UNIXSocket.new(path)
s = srv.accept
s.sendmsg("unix hi")
buf = c.recv(1024)
p buf
p c.path
s.close
c.close
srv.close
begin
  File.unlink(path)
rescue
end
