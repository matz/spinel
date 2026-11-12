# A call of IO's own copy_stream or new made before the program redefines
# it runs IO's method, one after the definition the program's.
require "tmpdir"

src = File.join(Dir.tmpdir, "spinel_io_class_method_before_def_src.txt")
dst = File.join(Dir.tmpdir, "spinel_io_class_method_before_def_dst.txt")
File.write(src, "abc\n")
p IO.copy_stream(src, dst)
p IO.new(1, autoclose: false).fileno
class IO
  def self.copy_stream(*a) = :program_copy_stream
  def self.new(*a, **k) = :program_new
end
p IO.copy_stream(src, dst)
p IO.new(1, autoclose: false)
File.delete(src)
File.delete(dst)
