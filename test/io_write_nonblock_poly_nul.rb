# write_nonblock with a poly operand writes each byte of a String holding a
# NUL: the write stopped at the first NUL and answered 0 for a frame that
# starts with a length prefix from pack("N").
frame = [3].pack("N") + "abc"
values = [frame, 42, :sym]
r, w = IO.pipe

values.each do |v|
  p w.write_nonblock(v)
  p r.read_nonblock(100)
end

values.each do |v|
  p w.write_nonblock(v, exception: false)
  p r.read_nonblock(100)
end

flag = ARGV.size == 0
values.each do |v|
  p w.write_nonblock(v, exception: flag)
  p r.read_nonblock(100)
end

io = [w, "not an IO"][0]
values.each do |v|
  p io.write_nonblock(v)
  p r.read_nonblock(100)
end
values.each do |v|
  p io.write_nonblock(v, exception: flag)
  p r.read_nonblock(100)
end

p w.write_nonblock(frame)
p r.read_nonblock(100)
p w.write_nonblock(frame, exception: flag)
p r.read_nonblock(100)
r.close
w.close
