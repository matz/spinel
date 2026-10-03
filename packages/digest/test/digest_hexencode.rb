# Digest.hexencode is a module function of CRuby's digest extension: a
# String's bytes as lowercase hex, embedded NULs and multibyte characters
# included (#7241). aws-sigv4 calls it on a raw signature.
require "digest"

puts Digest.hexencode("")
puts Digest.hexencode("abc")
puts Digest.hexencode([0, 255, 0].pack("C*"))
puts Digest.hexencode("é")
puts Digest.hexencode(Digest::SHA256.digest("abc")) == Digest::SHA256.hexdigest("abc")
