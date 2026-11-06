# spinel: gc-stress
# IO#read(n) on a pipe that holds fewer than n bytes parks until the rest
# arrives, so a collection another thread starts meanwhile does not wait on
# the reader's worker (the process froze at 0% CPU).
r, w = IO.pipe
reader = Thread.new { r.read(3000).bytesize }
w.write("a" * 1000)
allocator = Thread.new do
  junk = []
  20_000.times { |i| junk << "garbage #{i}" * 4; junk.clear if junk.size > 1000 }
  :allocated
end
sleep 0.2
w.write("b" * 2000)
p reader.value
p allocator.value
