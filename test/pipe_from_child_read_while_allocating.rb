# spinel: gc-stress
# A pipe whose write end went to a child: the parent's read(n) of what the
# child writes in two parts parks for the second, while another thread
# allocates. The read end keeps its own buffer; only the write end's flag
# was cleared for the child.
r, w = IO.pipe
pid = spawn("printf aaaa; sleep 0.3; printf bbbbbb", out: w)
w.close
reader = Thread.new { r.read(10) }
allocator = Thread.new do
  junk = []
  20_000.times { |i| junk << "garbage #{i}" * 4; junk.clear if junk.size > 1000 }
  :allocated
end
p reader.value
p allocator.value
Process.wait(pid)
p $?.success?
