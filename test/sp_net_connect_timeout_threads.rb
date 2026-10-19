# spinel: not-cruby -- the spinel-native FFI DSL; the .expected is authored
# spinel: linux -- a full accept queue drops the SYN on Linux, so the connect hangs; macOS refuses it
# sp_net_connect_timeout under threads: the connect parks its green thread
# with the deadline (sp_sched_wait_io_timeout), so other threads run while it
# waits, and a connect nothing answers ends at the deadline.

module Net
  ffi_func :sp_net_listen_host,          [:str, :int, :int],    :int
  ffi_func :sp_net_local_port,           [:int],                :int
  ffi_func :sp_net_connect_timeout,      [:str, :int, :double], :int
  ffi_func :sp_net_take_connect_timeout, [],                    :int
  ffi_func :sp_net_close,                [:int],                :int
end

lfd = Net.sp_net_listen_host("127.0.0.1", 0, 1)
port = Net.sp_net_local_port(lfd)

# Starting a thread first starts the scheduler's monitor, so the connects
# below park rather than fall back to a plain blocking poll.
Thread.new { 1 }.join

# Fill the accept queue, so the next connect hangs.
held = []
fd = 0
tries = 0
while fd >= 0 && tries < 6
  fd = Net.sp_net_connect_timeout("127.0.0.1", port, 0.3)
  held.push(fd) if fd >= 0
  tries += 1
end
Net.sp_net_take_connect_timeout

hung = Thread.new do
  got = Net.sp_net_connect_timeout("127.0.0.1", port, 0.5)
  [got, Net.sp_net_take_connect_timeout]
end

# The main thread keeps running while the connect waits.
ticks = 0
while hung.alive?
  ticks += 1
  sleep 0.05
end
r = hung.value
puts "hung: #{r[0]} timed out: #{r[1]}"
puts "main ran meanwhile: #{ticks >= 3}"

held.each { |h| Net.sp_net_close(h) }
Net.sp_net_close(lfd)
