# spinel: gc-stress
# A connected TCP socket given to a spawned child is blocking, for the child
# and for the parent, which share its open file. The child fills it and
# waits for the reader. The parent's handle still waits in the scheduler,
# not in the kernel: on one worker, a thread reading it and a large write to
# it both finish while another thread allocates.
ENV["SPINEL_WORKERS"] = "1"
require "socket"
MB = 1_000_000
server = TCPServer.new("127.0.0.1", 0)
left = TCPSocket.new("127.0.0.1", server.addr[1])
right = server.accept

pid = spawn("head -c #{MB} /dev/zero", out: left)
sleep 0.3   # the socket fills while nobody reads
got = right.read(MB).bytesize
Process.wait(pid)
puts "child: exit=#{$?.exitstatus} got=#{got}"

allocator = Thread.new do
  junk = []
  20_000.times { |i| junk << "garbage #{i}" * 4; junk.clear if junk.size > 1000 }
  :allocated
end
reader = Thread.new { left.read(5) }
sleep 0.1
right.write("hello")
puts "parent read: #{reader.value}"
drain = Thread.new { right.read(4 * MB).bytesize }
left.write("x" * (4 * MB))
puts "parent write: got=#{drain.value}"
p allocator.value
