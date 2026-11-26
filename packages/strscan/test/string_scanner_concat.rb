# StringScanner#concat and #<< append to the String the scanner reads and
# return the scanner; the position and the last match stay (Rack's multipart
# parser feeds each read into its scanner with concat)
require "strscan"

ss = StringScanner.new(+"ab")
p ss.scan(/a/)
p ss.concat("cd").equal?(ss)
p ss.pos
p ss.matched
p ss.rest
p ss.string
ss << "ef" << "g"
p ss.rest
p ss.scan(/\w+/)
p ss.eos?
ss.concat("é")
p ss.getch
p ss.charpos

t = StringScanner.new(+"x")
t.terminate
t << "yz"
p t.eos?
p t.scan(/y/)
p t.rest

b = StringScanner.new((+"\xFFa").b)
b.concat((+"\xFE").b)
p b.string.encoding
p b.rest_size
p b.get_byte
p b.scan(/a/)
p b.rest.bytes

[[+"a", "\xFF".b], ["".b, "é"], ["a".b, "é"], [+"é", "a".b]].each do |src, more|
  m = StringScanner.new(src)
  m.concat(more)
  p [m.string.encoding, m.getch, m.charpos]
end

f = StringScanner.new("ab".freeze)
begin
  f.concat("c")
rescue FrozenError => e
  p e.message
end
p f.string
