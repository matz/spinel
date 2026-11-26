require "socket"

# What a socket reads is ASCII-8BIT, by every read path. Its non-ASCII
# bytes cannot join a UTF-8 String holding non-ASCII text: <<, concat
# and + raise Encoding::CompatibilityError (and leave the receiver as it
# was), as interpolation already did. ASCII on either side still joins.

def peer_read(text)
  srv = TCPServer.new("127.0.0.1", 0)
  t = Thread.new do
    c = srv.accept
    c.write(text)
    c.close
  end
  cl = TCPSocket.new("127.0.0.1", srv.addr[1])
  got = yield cl
  t.join
  cl.close
  srv.close
  got
end

def try(name)
  r = yield
  puts "#{name}: #{r.encoding} #{r.bytesize}"
rescue Encoding::CompatibilityError => e
  puts "#{name}: #{e.class}: #{e.message}"
end

# A partial read may answer fewer bytes than the peer wrote: read with the
# same call until all n are in (binary onto binary stays binary).
def read_all(n)
  buf = yield
  buf << yield while buf.bytesize < n
  buf
end

text = "café 日本\n"
n = text.bytesize
reads = {
  "gets" => peer_read(text) { |cl| cl.gets },
  "readline" => peer_read(text) { |cl| cl.readline },
  "readpartial" => peer_read(text) { |cl| read_all(n) { cl.readpartial(64) } },
  "read(n)" => peer_read(text) { |cl| cl.read(n) },
  "read" => peer_read(text) { |cl| cl.read },
  "recv" => peer_read(text) { |cl| read_all(n) { cl.recv(64) } },
  "sysread" => peer_read(text) { |cl| read_all(n) { cl.sysread(64) } },
}

reads.each do |name, buf|
  puts "#{name}: #{buf.encoding} #{buf.size} #{buf.bytesize}"
  try("  <<") { u = +"é"; u << buf }
  try("  concat") { u = +"é"; u.concat(buf) }
  try("  +") { "é" + buf }
  try("  reverse +") { buf + "é" }
  try("  interpolation") { "é#{buf}" }
end

buf = reads["readpartial"]
u = +"é"
begin
  u << buf
rescue Encoding::CompatibilityError
end
puts "kept: #{u.encoding} #{u.bytesize}"

# concat with several arguments checks them all before it appends any:
# the receiver is left as it was, even when it is one of the arguments.
def concat_kept(name, s)
  3.times do
    begin
      yield s
    rescue Encoding::CompatibilityError
    end
  end
  puts "concat #{name}: #{s.encoding} #{s.bytes.inspect}"
end
concat_kept("self, bytes", +"é") { |s| s.concat(s, buf) }
concat_kept("ascii, bytes", +"é") { |s| s.concat("x", buf) }
concat_kept("self, self, bytes", +"é") { |s| s.concat(s, s, buf) }
concat_kept("bytes, text", +"a") { |s| s.concat(buf, "é") }
concat_kept("self, bytes, text", +"a") { |s| s.concat(s, buf, "é") }
args = [buf, "é"]
concat_kept("splat bytes, text", +"a") { |s| s.concat(*args) }
try("concat self, ascii") { s = +"é"; s.concat(s, "x") }
try("concat ascii, bytes onto ascii") { s = +"a"; s.concat("b", buf) }

try("ascii << bytes") { v = +"id="; v << buf }
try("bytes << ascii") { v = buf.dup; v << "ok" }
try("empty << text") { v = String.new; v << "é" }
try("bytes << text") { v = buf.dup; v << "é" }
try("a + b + bytes") { "a" + "é" + buf }

# Three-operand + chains, every order of ASCII text, non-ASCII text,
# ASCII bytes and non-ASCII bytes: high bytes in both encodings anywhere
# in the chain raise, whichever operand brings them.
ops = { "a" => "a", "e" => "é", "A" => "a".b, "B" => buf }
ops.each do |n1, x|
  ops.each do |n2, y|
    ops.each do |n3, z|
      try("#{n1} + #{n2} + #{n3}") { x + y + z }
    end
  end
end
try("lit e + a + bytes") { "é" + "a" + buf }
try("bytes + lit a + e") { buf + "a" + "é" }
try("lit a + bytes + e") { "a" + buf + "é" }

ascii = peer_read("hello\n") { |cl| read_all(6) { cl.readpartial(64) } }
try("text << ascii bytes") { v = +"é"; v << ascii }
try("text + ascii bytes") { "é" + ascii }
