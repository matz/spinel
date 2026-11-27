# spinel: not-cruby -- which thread waits on which worker is Spinel's own
# A worker makes its readiness set (kqueue / epoll) when a thread there
# first waits on I/O. With no descriptor left for one, that wait goes on
# the monitor's poll list. Here a helper's set is up first, then the
# descriptors run out, and main waits on a pipe for the first time: the
# monitor must still poll it, or main sleeps forever.
ENV["SPINEL_WORKERS"] = "2"   # main and one helper, under any outside setting
r1, w1 = IO.pipe
t = Thread.new { r1.read(1) }   # brings the helper's set up
sleep 0.05
w1.write("a")
p t.value

r, w = IO.pipe
soft, max = Process.getrlimit(:NOFILE)
Process.setrlimit(:NOFILE, 64, max)
held = []
def grab(held)
  while true
    a, b = IO.pipe
    held << a << b
  end
rescue IOError, SystemCallError
end
grab(held)
writer = Thread.new { sleep 0.05; w.write("b") }
p r.read(1)                     # main's first I/O wait: no descriptor for its set
writer.join
Process.setrlimit(:NOFILE, soft, max)   # a close wants a descriptor of its own
held.each(&:close)
p :done
