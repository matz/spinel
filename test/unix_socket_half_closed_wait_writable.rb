# A peer that stops sending does not make a wait for room to write answer
# while the send buffer is still full.
require "socket"
ENV["SPINEL_WORKERS"] = "1"

a, b = UNIXSocket.pair
t = Thread.new { b.read }
sleep 0.05
a.close_write
t.join
chunk = "x" * 65536
loop do
  b.write_nonblock(chunk)
rescue IO::WaitWritable
  break
end
p b.wait_writable(0.5)
