# spinel: gc-stress
# A child gets its redirected descriptors blocking. Writing to a pipe made its
# write end O_NONBLOCK, the flag lives on the description the child shares,
# and a child that filled the pipe failed with EAGAIN ("Resource temporarily
# unavailable") instead of waiting for the parent to read.
MB = 1_000_000

def drain_later(r)
  sleep 0.3   # the pipe fills while nobody reads
  r.read.bytesize - 1
end

# stdout
r, w = IO.pipe
w.write("p")
pid = spawn("head -c #{MB} /dev/zero", out: w)
w.close
got = drain_later(r)
Process.wait(pid)
puts "out: exit=#{$?.exitstatus} got=#{got}"

# stderr
r, w = IO.pipe
w.write("p")
pid = spawn("head -c #{MB} /dev/zero 1>&2", err: w)
w.close
got = drain_later(r)
Process.wait(pid)
puts "err: exit=#{$?.exitstatus} got=#{got}"

# system with a redirect
r, w = IO.pipe
w.write("p")
t = Thread.new { drain_later(r) }
ok = system("head -c #{MB} /dev/zero", out: w)
w.close
puts "system: ok=#{ok} got=#{t.value}"
