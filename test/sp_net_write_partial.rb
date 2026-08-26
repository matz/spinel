# Non-blocking write: hand back what the socket took, leave the policy for the
# rest to the caller. sp_net_write_str / _bytes keep writing until everything
# is gone, so one peer that cannot keep up delays delivery to every other one,
# and nothing at the Ruby level can see it happening. (matz/spinel#4103)
#
# A UNIX-domain pair gives two live fds without binding a port, so this is
# deterministic on any POSIX target.
module Net
  ffi_func :sp_net_unix_listen,   [:str, :int],        :int
  ffi_func :sp_net_unix_connect,  [:str],              :int
  ffi_func :sp_net_accept,        [:int],              :int
  ffi_func :sp_net_set_nonblock,  [:int],              :int
  ffi_func :sp_net_write_partial, [:int, :str, :int],  :int
  ffi_func :sp_net_recv_some,     [:int, :int],        :str
  ffi_func :sp_net_close,         [:int],              :int
  ffi_func :sp_net_getpid,        [],                  :int
end

path = "/tmp/sp_net_wp_#{Net.sp_net_getpid}.sock"
File.unlink(path) if File.exist?(path)
lfd = Net.sp_net_unix_listen(path, 4)
cfd = Net.sp_net_unix_connect(path)
afd = Net.sp_net_accept(lfd)
puts "paired: #{lfd > 0 && cfd > 0 && afd > 0}"

Net.sp_net_set_nonblock(cfd)

# An empty write is not an error and not a partial write.
puts "empty: #{Net.sp_net_write_partial(cfd, '', 0)}"

# A small write fits, and arrives whole.
puts "small: #{Net.sp_net_write_partial(cfd, 'hello', 5)}"
puts "read: #{Net.sp_net_recv_some(afd, 64)}"

# Backpressure is visible rather than blocking: with nothing draining the far
# end, the socket buffer fills and the write reports 0 accepted. The old
# blocking write would still be parked here.
chunk = "x" * 65536
sent  = 0
hit   = false
200.times do
  n = Net.sp_net_write_partial(cfd, chunk, 65536)
  if n == 0
    hit = true
    break
  end
  break if n < 0
  sent += n
end
puts "backpressure: #{hit}"
puts "accepted some first: #{sent > 0}"

# Draining the far end makes room, and the write resumes -- the caller keeps
# the remainder and retries, which is the whole point of the contract.
Net.sp_net_recv_some(afd, 65536)
puts "resumes: #{Net.sp_net_write_partial(cfd, 'more', 4) > 0}"

Net.sp_net_close(afd)
Net.sp_net_close(cfd)
Net.sp_net_close(lfd)
File.unlink(path)
