# After a spawn hands a pipe to a child, the parent's write end is blocking
# again: the spawn cleared O_NONBLOCK on the description it shares with the
# child. A large write from the parent still parks instead of sleeping in the
# kernel, so the reader thread on the same worker can drain the pipe.
#
# One worker is the point of the test: with two, the kernel does the hand-off
# without the scheduler.
ENV["SPINEL_WORKERS"] = "1"

r, w = IO.pipe
w.write("p")
pid = spawn("true", out: w)
Process.wait(pid)
t = Thread.new { r.read.bytesize }
w.write("x" * 300_000)
w.close
puts "got=#{t.value}"
