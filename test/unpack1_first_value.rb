# unpack1 stops at the first value, so a directive after it is never read:
# an unknown directive ("\x00", "y"), "%", "X" past the start or invalid
# base64 there raised ArgumentError. A nil for short input is no value for
# unpack1 ("a".unpack1("NC") is 97), as CRuby counts them.

def t
  p yield
rescue ArgumentError => e
  puts "ArgumentError: #{e.message}"
end

t { "abc".unpack1("C\x00C") }
t { "abc".unpack1("a\x00C") }
t { "abc".unpack1("CyC") }
t { "abc".unpack1("C%") }
t { "abc".unpack1("CX9") }
t { "abc".unpack1("Cm0") }
t { "a\xffb".b.unpack1("U*") }
t { "abc".unpack1("Cy", offset: 1) }
t { "a".unpack1("NC") }
t { "\x01".unpack1("SC") }
t { "\x01".unpack("SC") }
t { "".unpack1("CC") }
t { "".unpack1("Ca") }

# a directive before the first value raises as before
t { "abc".unpack1("yC") }
t { "abc".unpack1("\x00C") }
t { "".unpack1("C\x00C") }

s = ["abc", 1][ARGV.size]
t { s.unpack1("C\x00C") }
t { s.unpack1("Cy") }
t { s.unpack1("Cy", offset: 2) }
t { s.unpack1("NC") }
f = ["Cy", 1][ARGV.size]
t { s.unpack1(f) }

# a program class owns unpack1: the String case is the dispatch's arm
class Rec
  def unpack1(fmt) = "rec #{fmt}"
end
[Rec.new, "abc"].each { |v| t { v.unpack1("Cy") } }
