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
