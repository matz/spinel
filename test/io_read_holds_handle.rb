# spinel: gc-stress
# A handle that nothing else holds stays open while its read or pread runs:
# a collection during the read does not close it
require "tmpdir"
pa = File.join(Dir.tmpdir, "spinel_io_read_holds_handle_#{Process.pid}.txt")
File.write(pa, "abc")
3.times do
  fd = IO.sysopen(pa)
  h = {a: [1, 2]}.dup
  p IO.for_fd(fd, "r").read
end
3.times do
  h = {a: [1, 2]}.dup
  p File.new(pa).read
  p File.open(pa).read(2)
  p File.new(pa).pread(2, 1)
  p File.new(pa).pread(2, 0, +"")
end
File.delete(pa)
