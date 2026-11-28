# String#crypt: a receiver with a NUL inside is an ArgumentError, as CRuby
# raises it (crypt(3) would hash the bytes before the NUL), and the hash is
# a BINARY String, as CRuby answers it; a NUL in the salt past its first two
# bytes is read as crypt(3) reads it.
p "ab".crypt("ab").encoding
p "x".crypt("ab")
p(("x\0y".crypt("ab") rescue $!))
p(("x".crypt("a\0b") rescue $!))
p "a".crypt("ab").encoding == Encoding::BINARY
