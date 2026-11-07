# spinel: not-cruby -- the spinel-native FFI DSL; the .expected is authored
# sp_net_unix_connect under threads: the descriptor comes back non-blocking,
# as sp_net_connect's does, so a recv with nothing to read parks its green
# thread and the one worker runs the others.
require "tmpdir"

ENV["SPINEL_WORKERS"] = "1"

module Net
  ffi_func :sp_net_unix_listen,  [:str, :int],       :int
  ffi_func :sp_net_unix_connect, [:str],             :int
  ffi_func :sp_net_accept,       [:int],             :int
  ffi_func :sp_net_recv_some,    [:int, :int],       :binstr
  ffi_func :sp_net_write_str,    [:int, :str],       :int
  ffi_func :sp_net_close,        [:int],             :int
end

path = File.join(Dir.tmpdir, "sp_net_unix_connect_parks_#{Process.pid}.sock")
File.delete(path) if File.exist?(path)
lfd = Net.sp_net_unix_listen(path, 1)
cfd = Net.sp_net_unix_connect(path)
sfd = Net.sp_net_accept(lfd)

reader = Thread.new { Net.sp_net_recv_some(cfd, 16) }

# The reader waits for data while main sleeps; main can only wake up and
# answer if the wait parked the reader instead of holding the worker.
sleep 0.05
Net.sp_net_write_str(sfd, "answered")
puts reader.value

Net.sp_net_close(cfd)
Net.sp_net_close(sfd)
Net.sp_net_close(lfd)
File.delete(path)
