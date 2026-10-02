s = +"a"
[s].each.with_index { |q, i| q << i.to_s }
p s
