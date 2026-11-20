# spinel: int64
# String#append_as_bytes appends an Integer's low byte, as CRuby masks it
# with 0xff (256 is "\x00", -1 is "\xff", a Bignum the last byte of its two's
# complement), whether the Integer is typed or boxed (read out of a mixed
# Array, or a parameter that also takes Strings). A boxed Integer raised
# "no implicit conversion of Integer into String", and a typed one outside
# 0..255 RangeError. An argument that is neither a String nor an Integer is
# CRuby's TypeError, raised before any byte is appended.
def hex(s) = s.unpack1("H*")

# boxed: read out of a mixed Array
log = "".b
["Seongsu", 0xa1, "Kondae", 0x1b2, -3].each do |v|
  log.append_as_bytes(v)
end
p hex(log), log.bytesize

# boxed and narrowed by is_a?
ticket = "".b
[0xa1, "Jayang-dong"].each do |v|
  ticket.append_as_bytes(v) if v.is_a?(Integer)
end
p hex(ticket)

# typed, outside 0..255
line = "".b
line.append_as_bytes(256, 0x1a1, -1, -256, -257, 0x1_0000_0041)
p hex(line)

# Bignums, typed and boxed
big = "".b
big.append_as_bytes(2**70 + 0x41, -(2**70) - 1)
[2**64 + 0x42, "ICN"].each { |v| big.append_as_bytes(v) }
p hex(big)

# several arguments, typed and boxed mixed
fares = [1450, "GMP", 0x2c1]
trip = "".b
trip.append_as_bytes("Guui", fares[0], fares[1], 300, fares[2])
p trip.bytes

# a String the block appends to in place
tap = proc { |card, v| card.append_as_bytes(v, 0x10a) }
card = "".b
[0x1ff, "Achasan"].each { |v| tap.call(card, v) }
p hex(card)

# an instance variable receiver
class Gate
  attr_reader :log
  def initialize = @log = "".b
  def pass(v) = @log.append_as_bytes(v, -2)
end
gate = Gate.new
[0x341, "Jayang"].each { |v| gate.pass(v) }
p hex(gate.log)

# a UTF-8 receiver keeps its encoding
river = +"Han River "
river.append_as_bytes(0x1e9)
p river, river.encoding, river.valid_encoding?

# neither a String nor an Integer: TypeError, and nothing appended
park = +"Children's Grand Park"
[:zoo, nil, 1.5].each do |v|
  begin
    park.append_as_bytes("!", v)
  rescue TypeError => e
    p e.message
  end
end
begin
  park.append_as_bytes("!", [1])
rescue TypeError => e
  p e.message
end
p park
begin
  (+"Seongsu").append_as_bytes(:station)
rescue TypeError => e
  p e.message
end
