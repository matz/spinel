# Encoding.default_external= and default_internal= set what the getters and
# Encoding.find("external") return, and a read follows the external default
# between UTF-8 and ASCII-8BIT. The setters were dropped.
require "tmpdir"

def try
  p yield
rescue => e
  puts "#{e.class}: #{e.message}"
end

p Encoding.default_external
p Encoding.default_internal

dir = Dir.mktmpdir("encdef")
path = File.join(dir, "a.txt")
File.write(path, "one\ntwo\n")

def reads(path, dir)
  p File.read(path).encoding
  p File.readlines(path)[0].encoding
  p File.readlines(path, chomp: true)[0].encoding
  File.foreach(path) { |l| p l.encoding; break }
  File.open(path) do |f|
    p f.gets.encoding
    p f.getc.encoding
    p f.read(2).encoding
    p f.read.encoding
  end
  r, w = IO.pipe
  w.write("x\ny")
  w.close
  p r.gets.encoding
  p r.read.encoding
  r.close
  p `echo hi`.encoding
  p Dir.children(dir)[0].encoding
  p Dir.pwd.encoding
  # an encoding the call or the handle is given stays
  p File.binread(path).encoding
  p File.read(path, encoding: "utf-8").encoding
  p File.read(path, 2).encoding
  p File.read(path, 2, 1).encoding
  p File.read(path, 2, encoding: "utf-8").encoding
  p File.read(path, 2, 1, encoding: "utf-8").encoding
  p File.read(path, nil, 1, encoding: "utf-8").encoding
  p File.read(path, internal_encoding: "utf-8").encoding
  File.open(path, "r:utf-8") { |f| p f.read.encoding }
  File.open(path, "rb") { |f| p f.read.encoding }
  File.open(path, encoding: Encoding::UTF_8) { |f| p f.gets.encoding }
  File.open(path, encoding: "binary") { |f| p f.read.encoding }
  utf8 = "utf-8"
  bin = "binary"
  File.open(path, encoding: utf8) { |f| p f.read.encoding }
  File.open(path, encoding: bin) { |f| p f.read.encoding }
  p Dir.children(dir, encoding: "utf-8")[0].encoding
  p Dir.children(dir, encoding: "binary")[0].encoding
  Dir.each_child(dir, encoding: "utf-8") { |n| p n.encoding }
  File.open(path) { |f| f.set_encoding("utf-8"); p f.read.encoding }
  File.open(path) { |f| f.set_encoding(nil); p f.read.encoding }
  p "lit".encoding
end

reads(path, dir)

p(Encoding.default_external = Encoding::ASCII_8BIT)
p(Encoding.default_internal = Encoding::ASCII_8BIT)
p Encoding.default_external
p Encoding.default_internal
p Encoding.default_external == Encoding::BINARY
p Encoding.find("external")
p Encoding.find("filesystem")
p Encoding.find("internal")
p Encoding.find("locale")

Encoding.default_internal = nil
p Encoding.default_internal
p Encoding.find("internal")
reads(path, dir)

# a handle opened before the assignment follows it
f = File.open(path)
Encoding.default_external = "utf-8"
p Encoding.default_external
p f.gets.encoding
Encoding.default_external = "BINARY"
p f.gets.encoding
f.close

# a limit counts bytes on an ASCII-8BIT read
File.binwrite(path, "\xC3\xA9x\n")
File.open(path) { |g| p g.gets(1).bytes }
File.open(path, "r:utf-8") { |g| p g.gets(1).bytes }
File.binwrite(path, "A\x80B")
File.open(path) { |g| p g.gets("\x80".b).bytes }
# the path is read before an encoding: expression that assigns it
other = path + ".other"
q = path
File.open(q, "w", encoding: (q = other; "utf-8")) { |g| g.write("w1") }
p File.read(path), File.exist?(other)
q = path
g = File.new(q, "w", encoding: (q = other; "utf-8"))
g.write("w2")
g.close
p File.read(path), File.exist?(other)
File.binwrite(path, "\xC3\xA9x\n")
p File.read(path, nil, encoding: "utf-8").encoding
p File.read(path, nil, 1, encoding: "utf-8").encoding
p File.read(path, nil).encoding
File.write(path, "one\ntwo\n")

# a name, and the names of the defaults themselves
try { Encoding.default_external = "utf-8" }
try { Encoding.default_external }
try { Encoding.default_internal = "external" }
try { Encoding.default_internal }
try { Encoding.default_external = "internal" }
try { Encoding.default_external.name }
try { Encoding.default_internal = "internal" }
try { Encoding.default_internal }
try { Encoding.default_internal ||= Encoding::ASCII_8BIT }
Encoding.default_internal = nil
try { Encoding.default_internal ||= Encoding::ASCII_8BIT }
try { Encoding.default_internal }

# an encoding a String here does not have is kept for the getters
try { Encoding.default_external = Encoding::EUC_JP }
try { Encoding.default_external }
try { Encoding.find("external") }
try { Encoding.default_internal = "Shift_JIS" }
try { Encoding.default_internal }

try { Encoding.default_external = nil }
try { Encoding.default_external = 1 }
try { Encoding.default_external = :utf8 }
try { Encoding.default_external = "nope" }
try { Encoding.default_internal = 1 }
try { Encoding.default_internal = "nope" }
try { [Encoding.default_external, Encoding.default_internal] }

Encoding.default_internal = nil
Encoding.default_external = Encoding::UTF_8
reads(path, dir)

File.delete(path)
Dir.rmdir(dir)
