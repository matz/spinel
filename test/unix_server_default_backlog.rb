# spinel: linux -- macOS caps every backlog at kern.ipc.somaxconn (128), so only Linux shows a longer queue
# UNIXServer.new listens with SOMAXCONN, as CRuby's does, so the kernel's
# own limit (net.core.somaxconn) decides how many connects wait for accept.
# A fixed 128 filled the queue under a burst: a nginx in front got EAGAIN
# on its connect and answered 502 while the server was still accepting.
#
# Nothing accepts here, so each connect that the queue takes succeeds at
# once and the first one past it fails: EAGAIN on Linux, ECONNREFUSED on
# macOS.
require "socket"
require "tmpdir"

path = File.join(Dir.tmpdir, "spinel_unix_backlog_#{Process.pid}.sock")
File.unlink(path) if File.exist?(path)
server = UNIXServer.new(path)

want = 300
# the kernel's limit; where /proc has none (macOS), 128, the default of
# macOS's kern.ipc.somaxconn
limit = "/proc/sys/net/core/somaxconn"
cap = File.exist?(limit) ? File.read(limit).to_i : 128
clients = []
queued = 0
full = false
while !full && clients.size < want
  s = Socket.new(Socket::AF_UNIX, Socket::SOCK_STREAM, 0)
  clients << s
  begin
    s.connect_nonblock(Socket.sockaddr_un(path))
    queued += 1
  rescue Errno::EAGAIN, Errno::ECONNREFUSED
    full = true
  end
end

# CRuby passes the SOMAXCONN its own build saw, so a Ruby built against an
# older C library listens with 128 where the kernel would take more
puts "queue holds SOMAXCONN or what the kernel allows: #{queued >= [want, cap, Socket::SOMAXCONN].min}"

clients.each { |c| c.close }
server.close
File.unlink(path)
