# String#inspect and String#dump take only a valid UTF-8 character as a
# character, as CRuby's precise length does. A sequence whose continuation
# bytes are there but that encodes no character -- a surrogate half, an
# overlong form, a code point past U+10FFFF -- escapes each of its bytes as
# \xNN. It was decoded as a code point: the surrogate half JSON.parse answers
# for "\uDC00" printed as "\uDC00", and "\xC0\x80" as "\u0000".

def str(*bytes) = bytes.pack("C*").force_encoding("UTF-8")

[
  # surrogate halves (ED A0..BF), and the last character before them
  str(0xED, 0xB0, 0x80), str(0xED, 0xA0, 0x80), str(0x61, 0xED, 0xBF, 0xBF, 0x62),
  str(0xED, 0x9F, 0xBF),
  # overlong forms, and the smallest character each lead byte encodes
  str(0xC0, 0x80), str(0xC1, 0xBF), str(0xC2, 0x80),
  str(0xE0, 0x80, 0x80), str(0xE0, 0x9F, 0xBF), str(0xE0, 0xA0, 0x80),
  str(0xF0, 0x80, 0x80, 0x80), str(0xF0, 0x8F, 0xBF, 0xBF), str(0xF0, 0x90, 0x80, 0x80),
  # past U+10FFFF, and U+10FFFF itself
  str(0xF4, 0x90, 0x80, 0x80), str(0xF5, 0x80, 0x80, 0x80), str(0xF4, 0x8F, 0xBF, 0xBF),
  # a valid character after an invalid one is still a character
  str(0xED, 0xB0, 0x80, 0xE3, 0x81, 0x82),
].each do |s|
  puts s.inspect
  puts s.dump
  puts [s].inspect
  p s.valid_encoding?
end

# a BINARY String's high bytes stay bytes
s = [0xED, 0xB0, 0x80].pack("C*")
puts s.inspect
puts s.dump

# DEL is a control byte: #dump writes it as \x7F, #inspect of a UTF-8 String
# as \u007F, as CRuby does
s = str(0x61, 0x7F, 0x62)
puts s.inspect
puts s.dump
puts s.b.inspect
puts s.b.dump
