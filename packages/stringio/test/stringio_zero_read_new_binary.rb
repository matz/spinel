# readpartial(0), sysread(0) and read_nonblock(0) answer a new BINARY String
# each, which the caller may append to.
require "stringio"
io = StringIO.new(+"abc")
s = io.readpartial(0)
p s.frozen?, s.encoding
s2 = io.sysread(0)
p s2.frozen?, s2.encoding
s3 = io.read_nonblock(0)
p s3.frozen?, s3.encoding
s << "x"
p s, s2.equal?(s3)
