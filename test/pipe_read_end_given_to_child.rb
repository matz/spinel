# spinel: gc-stress
# A pipe whose read end went to a child (a spawn redirect) turns blocking for
# the parent too, which shares its open file. The parent's read(n) still
# parks for bytes that come later, asking the kernel first whether any are
# there, while another thread allocates.
r, w = IO.pipe
pid = spawn("sleep 0.3", in: r)
reader = Thread.new { r.read(6) }
w.write("abc")
allocator = Thread.new do
  junk = []
  20_000.times { |i| junk << "garbage #{i}" * 4; junk.clear if junk.size > 1000 }
  :allocated
end
sleep 0.2
w.write("def")
p reader.value
p allocator.value
Process.wait(pid)
p $?.success?
