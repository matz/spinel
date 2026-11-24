# TCPServer.for_fd on a read-only File's descriptor raises, and the File
# can still read from the descriptor afterward.
require "socket"
require "tmpdir"

path = File.join(Dir.tmpdir, "sp_socket_for_fd_ro_#{Process.pid}")
File.write(path, "kept")
f = File.open(path, "r")
begin
  TCPServer.for_fd(f.fileno)
  puts "no error"
rescue IOError, SystemCallError
  puts "raised"
end
p f.read
f.close
File.delete(path)
