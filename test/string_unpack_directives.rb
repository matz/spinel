# String#unpack checks its format as CRuby does: a byte that is no directive,
# a NUL among them included, is ArgumentError naming it (it was skipped, and a
# NUL ended the format), `x` past the end and `X*` past the start raise, `#`
# comments run to the end of the line, and i/I/j/J/p/P read their native sizes.
def t
  p yield
rescue ArgumentError => e
  p [e.class, e.message]
end
t { "abcd".unpack("CX*C") }
t { "\x01\x02\x03\x04".unpack("C2x3C") }
t { "abc".unpack("H\x00H") }
t { "abc".unpack("b\x00b") }
t { "abc".unpack("w\x00w") }
t { "abc".unpack("C\x00C") }
t { "abc".unpack("CX2C") }
t { "abc".unpack("CX4C") }
t { "abc".unpack("Cx5") }
t { "abc".unpack("C x C") }
t { "abc".unpack("Cy") }
t { "abc".unpack1("x9") }
t { "abc".unpack("@5C") }
t { "abc".unpack("X") }
b = "\x01\x00\x00\x00\xfe\xff\xff\xff\x02\x00\x00\x00\x00\x00\x00\x00\xff\xff\xff\xff\xff\xff\xff\xff".b
t { b.unpack("iI") }
t { b.unpack("i!I_") }
t { b.unpack("x8" + (["x"].pack("p").bytesize == 8 ? "jJ" : "qQ")) }   # j/J are the pointer size: q/Q where it is 4 bytes
t { b.unpack("i>") }
t { b.unpack("C # a comment\nC") }
t { b.unpack("C\vC\fC\rC") }
t { ("\x00" * 8).unpack("p") }
t { ("\x00" * 8).unpack("P") }
t { b.unpack("x8p") }
t { b.unpack1("x*") }
t { b.unpack("x24") }
t { b.unpack("x25") }
t { b.unpack("C3X*C") }
t { "".unpack("X*") }
t { "ab".unpack("CX*") }
t { [1, 2].pack("CC").unpack("CC") }
t { "abc".unpack("a\x00") }
t { "abc".unpack("C\x01C") }
t { "abc".unpack("C\x7fC") }
t { "abc".unpack("C\x1bC\ty") }
t { "abc".unpack("Cé") }
t { "abc".unpack("C\xffC".b) }
t { "abc".unpack("C\u0080y") }
t { "abc".unpack("C y") }
t { "abc".unpack("C\u0085y") }
