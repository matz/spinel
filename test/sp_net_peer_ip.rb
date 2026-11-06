# spinel: not-cruby -- the spinel-native FFI DSL; the .expected is authored
# sp_net_peer_ip: the IP a server's accepted connection came from, as the
# `REMOTE_ADDR` a Rack server sets (Rack::Request#ip, rate limits keyed by
# it). A UNIX-domain connection has none and answers "".
require "tmpdir"

module Net
  ffi_func :sp_net_listen_host,  [:str, :int, :int], :int
  ffi_func :sp_net_local_port,   [:int],             :int
  ffi_func :sp_net_connect,      [:str, :int],       :int
  ffi_func :sp_net_unix_listen,  [:str, :int],       :int
  ffi_func :sp_net_unix_connect, [:str],             :int
  ffi_func :sp_net_accept,       [:int],             :int
  ffi_func :sp_net_peer_ip,      [:int],             :str
  ffi_func :sp_net_close,        [:int],             :int
end

lfd = Net.sp_net_listen_host("127.0.0.1", 0, 4)
cfd = Net.sp_net_connect("127.0.0.1", Net.sp_net_local_port(lfd))
afd = Net.sp_net_accept(lfd)
p Net.sp_net_peer_ip(afd)
Net.sp_net_close(afd)
Net.sp_net_close(cfd)
Net.sp_net_close(lfd)

path = File.join(Dir.tmpdir, "sp_net_pi_#{Process.pid}.sock")
File.unlink(path) if File.exist?(path)
ufd = Net.sp_net_unix_listen(path, 4)
ucfd = Net.sp_net_unix_connect(path)
uafd = Net.sp_net_accept(ufd)
p Net.sp_net_peer_ip(uafd)
p Net.sp_net_peer_ip(-1)
Net.sp_net_close(uafd)
Net.sp_net_close(ucfd)
Net.sp_net_close(ufd)
File.unlink(path)
