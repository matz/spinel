# An identity Hash keyed by what a Struct's own deconstruct_keys answers
# (#11078; was refused).
Sig = Struct.new(:name)
S = Struct.new(:a) do
  def deconstruct_keys(_) = { a: "str" }
end
o = {}.compare_by_identity
case S.new(Sig.new("x"))
in S[a:]
  o[a] = 1
end
p o.size
