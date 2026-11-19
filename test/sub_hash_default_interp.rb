# sub/gsub with a replacement Hash of any values: a match the Hash lacks
# reads its default (as to_s), and an interpolated pattern takes the Hash as
# a literal one does.
h = Hash.new("?")
h["e"] = 3
p "hello".sub(/l/, h)
p "hello".gsub(/[el]/, h)
g = Hash.new(0)
g["e"] = 3
p "hello".gsub(/[el]/, g)
k = Hash.new(:x)
k["e"] = nil
p "hello".gsub(/[el]/, k)
x = "l"
p "hello".sub(/#{x}/, {"e" => 3})
p "hello".gsub(/#{x}/, {"l" => "L"})
p "hello".gsub(/[#{x}e]/, {"l" => 1, "e" => :E})
p "hello".gsub(/#{x}/, h)
p "hello".gsub(/#{x}/, "*")
p "hello".gsub(/l/, {"l" => 1})

# A new default String must stay live while the converted Hash is built.
poly = Hash.new(424242)
poly["e"] = :v
p "hello".gsub(/[el]/, poly)
p "hello".sub("e".dup, poly)
p "hello".gsub("e".dup, g)
p "hello".sub("l".dup, {"l" => nil})
