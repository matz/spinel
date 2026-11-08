# StringIO#read with a length answers a BINARY String, and read and gets
# with none answer the buffer's own encoding (#11344)
require "stringio"
SOI = "\xFF\xD8".b
data = "\xFF\xD8\xFF\xE0".b
io = StringIO.new(data)
head = io.read(2)
p head.encoding
p head == SOI
p StringIO.new(data).read.encoding
u = StringIO.new("abc")
p u.read(0).encoding
p u.read(2).encoding
p u.read.encoding
b = StringIO.new("\xFF\xD8\n\xFF".b)
p b.gets.encoding
p b.read.encoding
p b.read(0).encoding
p b.read.encoding
x = StringIO.new("ab".b)
x.read
p x.read(1)
p StringIO.new("héllo").read.encoding
