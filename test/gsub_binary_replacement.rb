# The result of sub and gsub takes the encoding the receiver and the
# replacement share, as a concatenation does: an ASCII-only receiver with a
# BINARY replacement is BINARY, a BINARY receiver stays BINARY, as CRuby
# has it; with a String or a Regexp pattern, a String or a Hash replacement.
bin = 157.chr
p bin.encoding
p "a-b".gsub("-", bin).encoding, "a-b".sub("-", bin).encoding
p "a-b".gsub(/-/, bin).encoding, "a-b".sub(/-/, bin).encoding
p "a-b".gsub(/-/, bin).bytes, "a-b".gsub(/-/, bin) == "a\x9Db".b
s = "a-b-c".dup
s.gsub!(/-/, bin)
p s.encoding, s.bytes
t = "a-b".dup
t.sub!("-", bin)
p t.encoding, t == "a\x9Db".b
p "a-b".gsub(/-/, "x").encoding, "日-b".gsub(/-/, "x").encoding, "a-b".gsub(/-/, "日").encoding
p "abc".b.gsub(/b/, "x").encoding, "abc".b.gsub(/b/, "日").encoding, "abc".b.sub("b", "x").encoding
p "a-b".gsub(/-/, "-" => bin).encoding, "a-b".gsub("-", "-" => bin).encoding, "a-b".gsub(/-/, "-" => "x").encoding
p "a-b".gsub(/-/, "").encoding, "a-b".gsub(/z/, bin).encoding, "a-b".sub(/z/, bin).encoding
