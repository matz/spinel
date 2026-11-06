# File.write and File.binwrite naming the encoding the text already has
# (encoding:, external_encoding:, internal_encoding:; UTF-8 or BINARY) write
# the same bytes as the plain call, alone or with mode: / perm: / an offset,
# and answer the byte count
# cwd-relative: Windows MinGW has no /tmp
File.umask(022)
path = "spinel_write_encoding_#{Process.pid}.tmp"

p File.write(path, "kept", encoding: "UTF-8")
puts File.read(path, encoding: "UTF-8")
p File.write(path, "caf\u00e9\n", encoding: Encoding::UTF_8)
p File.read(path)
p File.write(path, "more\n", mode: "a", encoding: "UTF-8")
p File.read(path)
p File.write(path, "XY", 1, encoding: "UTF-8")
p File.read(path)
p File.write(path, "ext\n", external_encoding: Encoding::UTF_8, internal_encoding: Encoding::UTF_8)
p File.read(path)
p File.binwrite(path, "\x00\x01".b, encoding: Encoding::BINARY)
p File.binread(path)
File.delete(path)

p File.write(path, "secret\n", perm: 0600, encoding: "UTF-8")
p format("%o", File.stat(path).mode & 0777), File.read(path)
File.delete(path)
p File.exist?(path)

# a nil offset is no offset: a truncating write, and the options still apply
File.write(path, "hello world")
p File.write(path, "ab", nil, encoding: "UTF-8")
p File.read(path)
p File.write(path, "cd", nil, mode: "a", encoding: "UTF-8")
p File.read(path)
p File.write(path, "ef", nil, perm: 0600, encoding: "UTF-8")
p File.read(path)
File.delete(path)
p File.exist?(path)
