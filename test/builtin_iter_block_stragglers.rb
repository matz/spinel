# Blocks bound by the call emitters outside the iterator loops: catch's
# block takes the tag it was given; Hash#fetch_values and Struct#to_h run a
# block's leading statements; Hash.new's default block names its hash and
# key `_1, _2` as well as `|h, k|`.

catch(:done) { |t| p t }
p(catch(:x) { |t| throw t, 5 })
k = "k"
catch(k) { |t| p t }

h = { a: 1 }
p(h.fetch_values(:a, :b) { |key| puts "miss #{key}"; key.to_s })
S = Struct.new(:a, :b)
p(S.new(1, 2).to_h { |key, v| puts key; [key, v * 2] })

p(Hash.new { _1[_2] = _2 * 2 }[3])
d = Hash.new { |hh, key| hh[key] = key.to_s * 2 }
p d[4]
