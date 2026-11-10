# A count CRuby refuses on IO's own methods raises its ArgumentError, also
# when the call goes through File's singleton chain (File.sysopen is IO's).
# A subclass's `new` keeps its own counts: UDPSocket.new is not IO.new, and
# the socket classes' for_fd is BasicSocket's own, not IO's.
require "tmpdir"
require "socket"

src = File.join(Dir.tmpdir, "spinel_io_class_method_counts_src.txt")
File.write(src, "abc\n")
begin
  File.sysopen(src, 0, 0644, 1)
rescue ArgumentError => e
  p e.message
end
begin
  File.select
rescue ArgumentError => e
  p e.message
end
begin
  File.copy_stream(src)
rescue ArgumentError => e
  p e.message
end
begin
  IO.for_fd(1, "w", 3)
rescue ArgumentError => e
  p e.message
end
begin
  File.pipe(1, 2, 3)
rescue ArgumentError => e
  p e.message
end
u = UDPSocket.new
p u.class
begin
  BasicSocket.for_fd(u.fileno, 2, 3)
rescue ArgumentError => e
  p e.message
end
begin
  UDPSocket.for_fd(u.fileno, 2)
rescue ArgumentError => e
  p e.message
end
u.close
# the rows are the classes' own methods, as reflection lists them
p BasicSocket.singleton_methods(false).sort
p UDPSocket.singleton_methods(false).sort
File.delete(src)
