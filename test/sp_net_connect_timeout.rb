# spinel: not-cruby -- the spinel-native FFI DSL; the .expected is authored
# spinel: linux -- a full accept queue drops the SYN on Linux, so the connect hangs; macOS refuses it
# sp_net_connect_timeout: a connect with a deadline. sp_net_connect waits as
# long as the kernel does, which for an address that never answers is over a
# minute. The timed connect gives up after timeout_s seconds, and
# sp_net_take_connect_timeout tells that apart from a refusal, which also
# answers -1.
#
# A listener whose accept queue is full drops new SYNs, so a connect to it
# hangs as one to a dead host does, without needing a host that is down.

module Net
  ffi_func :sp_net_listen_host,          [:str, :int, :int],    :int
  ffi_func :sp_net_local_port,           [:int],                :int
  ffi_func :sp_net_connect_timeout,      [:str, :int, :double], :int
  ffi_func :sp_net_take_connect_timeout, [],                    :int
  ffi_func :sp_net_close,                [:int],                :int
end

lfd = Net.sp_net_listen_host("127.0.0.1", 0, 1)
port = Net.sp_net_local_port(lfd)
puts "listening: #{lfd > 0 && port > 0}"

# A listener with room answers at once, and that is not a timeout.
fd = Net.sp_net_connect_timeout("127.0.0.1", port, 5.0)
puts "connected: #{fd > 0} timed out: #{Net.sp_net_take_connect_timeout}"
held = [fd]

# A negative timeout connects as sp_net_connect does.
fd = Net.sp_net_connect_timeout("127.0.0.1", port, -1.0)
puts "no deadline: #{fd > 0} timed out: #{Net.sp_net_take_connect_timeout}"
held.push(fd)

# Nothing accepts, so the queue fills and the next connects hang until the
# deadline. The kernel may take one more first; at most a few are needed.
fd = 0
waited = 0.0
tries = 0
while fd >= 0 && tries < 6
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  fd = Net.sp_net_connect_timeout("127.0.0.1", port, 0.3)
  waited = Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0
  held.push(fd) if fd >= 0
  tries += 1
end
puts "full queue: #{fd} timed out: #{Net.sp_net_take_connect_timeout}"
puts "waited the deadline: #{waited >= 0.25 && waited < 2.0}"
# Reading the flag clears it.
puts "cleared: #{Net.sp_net_take_connect_timeout}"

held.each { |h| Net.sp_net_close(h) }
Net.sp_net_close(lfd)

# Nothing listens on the port now: a refusal is -1 but not a timeout.
puts "refused: #{Net.sp_net_connect_timeout("127.0.0.1", port, 5.0)} timed out: #{Net.sp_net_take_connect_timeout}"

# A name that does not resolve is not a timeout either.
puts "unresolved: #{Net.sp_net_connect_timeout("no-such-host.invalid", port, 5.0)} timed out: #{Net.sp_net_take_connect_timeout}"
