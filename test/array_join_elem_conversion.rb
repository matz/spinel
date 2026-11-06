# Array#join and Array#* convert a program object element the way CRuby
# does: its #to_str first, then its #to_ary (joined like a nested Array),
# then #to_s.
class K; def to_ary = ["x", "y"]; end
class S; def to_str = "str"; def to_s = "tos"; end
class B; def to_str = "bstr"; def to_ary = ["no"]; end
class N; def to_ary = nil; def to_s = "nn"; end
class D; def to_ary = [1, [2, 3]]; end
class T; def to_ary = 5; end
P = Struct.new(:a, :b) do
  def to_ary = [a, b]
end

k = K.new
puts [k].join("-")
puts [k, 1].join("-")
puts [k] * "+"
puts [S.new, 1].join(",")
puts [B.new].join(",")
puts [N.new].join(",")
puts [D.new, "z"].join("|")
puts ["a", D.new] * ":"
puts [[K.new, 1], 2].join("-")
puts [P.new(1, 2), 3].join(",")
def ks = [K.new, K.new]
puts ks.join(",")
begin
  [T.new].join
rescue TypeError => e
  puts e.message
end
