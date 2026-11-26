# The Base64 decoders answer a BINARY String, as CRuby's do: bytes past
# 0x7F compare equal to the same bytes as a BINARY String, and an ASCII
# result still equals a UTF-8 literal and finds the same Hash entry
require "base64"
raw = "\xFF\x80".b
enc = Base64.strict_encode64(raw)
a = Base64.strict_decode64(enc)
b = Base64.decode64(enc)
c = Base64.urlsafe_decode64(Base64.urlsafe_encode64(raw))
d = Base64.urlsafe_decode64(Base64.urlsafe_encode64(raw, padding: false))
[a, b, c, d].each { |s| p [s.encoding, s == raw, s.valid_encoding?, s.bytesize] }

ascii = Base64.decode64("YWJj")
p ascii.encoding, ascii == "abc", "abc" == ascii
table = { "abc" => 1 }
p table[ascii], { ascii => 2 }["abc"]
p (ascii + "!"), ascii.upcase, ascii.size
