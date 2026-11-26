# A join of a general Array whose first byte is a NUL keeps the BINARY
# encoding a binary part gives it, as a join of any other first byte does.
a = [0.chr, 200.chr, 1]
a.pop
j = a.join
p j.encoding, j.bytesize
p (j + "\xC8".b).encoding
