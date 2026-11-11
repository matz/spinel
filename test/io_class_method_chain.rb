# IO's own methods that answer the same for any IO class are File's too:
# File.copy_stream, File.sysopen and File.select reach IO's, as CRuby looks
# a singleton method up through the superclass's singleton.
require "tmpdir"

src = File.join(Dir.tmpdir, "spinel_io_class_method_chain_src.txt")
dst = File.join(Dir.tmpdir, "spinel_io_class_method_chain_dst.txt")
File.write(src, "abc\n")
p File.copy_stream(src, dst)
p File.read(dst)
fd = File.sysopen(src)
p fd.is_a?(Integer)
File.delete(src)
File.delete(dst)

r, w = IO.pipe
w.write "x"
ready = File.select([r], nil, nil, 1)
p ready.nil?
p ready[0].size
w.close
r.close

# A path beside a stream: the stream is read or written, the path opened
require "stringio"
src = File.join(Dir.tmpdir, "spinel_io_class_method_chain_mixed_src.txt")
dst = File.join(Dir.tmpdir, "spinel_io_class_method_chain_mixed_dst.txt")
File.write(src, "hello")
p(File.open(dst, "w") { |w| File.copy_stream(src, w) })
p File.read(dst)
p(File.open(src) { |r| File.copy_stream(r, dst) })
p File.read(dst)
p(File.open(dst, "w") { |w| IO.copy_stream(src, w) })
p File.read(dst)
p IO.copy_stream(StringIO.new("sio"), dst)
p File.read(dst)
out = StringIO.new
p File.copy_stream(src, out)
p out.string
File.delete(src)
File.delete(dst)
