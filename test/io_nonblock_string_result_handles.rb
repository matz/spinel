reader, writer = IO.pipe
p reader.read_nonblock(1, exception: false)
writer.write("a\0bc")
empty = reader.read_nonblock(0, exception: false)
value = reader.read_nonblock(3, exception: false)
held = [value, empty, 19]
p [value.bytes, value.encoding.name, empty.encoding.name]
value << "x" * 105
empty << "y" * 85
p [held[0].bytesize, held[1].bytesize, value.bytesize, empty.bytesize]
writer.close
p [reader.read_nonblock(50, exception: false), reader.read_nonblock(3, exception: false)]
reader.close
