# A peer that stops sending does not answer a wait for room to write while
# the send buffer is full: wait_writable times out, alone and with a reader
# parked on the same socket, which the peer's end of stream wakes. A peer
# that closes the socket entirely still wakes the writer.
require "socket"
ENV["SPINEL_WORKERS"] = "1"

def full(io)
  chunk = "x" * 65536
  loop do
    io.write_nonblock(chunk)
  rescue IO::WaitWritable
    break
  end
end

a, b = UNIXSocket.pair
t = Thread.new { b.read }
sleep 0.05
a.close_write
t.join
full(b)
p b.wait_writable(0.5)

a, b = UNIXSocket.pair
full(b)
reader = Thread.new { b.wait_readable(2) ? :readable : :timeout }
writer = Thread.new { b.wait_writable(0.5) ? :writable : :timeout }
sleep 0.1
a.close_write
p reader.value, writer.value

a, b = UNIXSocket.pair
full(b)
writer = Thread.new { b.wait_writable(2) ? :writable : :timeout }
sleep 0.1
a.close
p writer.value
