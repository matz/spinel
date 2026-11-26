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

text = "café 日本\n"
reads = {
  "gets" => peer_read(text) { |cl| cl.gets },
  "readline" => peer_read(text) { |cl| cl.readline },
  "readpartial" => peer_read(text) { |cl| cl.readpartial(64) },
  "read(n)" => peer_read(text) { |cl| cl.read(text.bytesize) },
  "read" => peer_read(text) { |cl| cl.read },
  "recv" => peer_read(text) { |cl| cl.recv(64) },
  "sysread" => peer_read(text) { |cl| cl.sysread(64) },
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

try("ascii << bytes") { v = +"id="; v << buf }
try("bytes << ascii") { v = buf.dup; v << "ok" }
try("empty << text") { v = String.new; v << "é" }
try("bytes << text") { v = buf.dup; v << "é" }
try("a + b + bytes") { "a" + "é" + buf }
ascii = peer_read("hello\n") { |cl| cl.readpartial(64) }
try("text << ascii bytes") { v = +"é"; v << ascii }
try("text + ascii bytes") { "é" + ascii }
