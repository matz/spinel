# A Struct's own deconstruct_keys answers what the member does not hold.
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
