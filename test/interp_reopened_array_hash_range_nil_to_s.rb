# An interpolated Array, Hash, Range or nil converts by its class's own to_s
# when the program reopens that class with one, as CRuby's does.
class Array; def to_s = "ARR(#{size})"; end
class Hash; def to_s = "HSH(#{size})"; end
class NilClass; def to_s = "NIL"; end
class Range; def to_s = "RNG(#{first})"; end
x = [2, 3]
h = {a: 1}
n = nil
r = 1..2
puts "#{[1]} #{x} #{h} #{{}} #{n} #{nil} #{r} #{(3..4)}"
puts "#{["a"]} #{[1.5]} #{{"k" => :v}}"
s = "s"
puts "#{s} #{:sym} #{7}"
puts "#{x.map { |v| v * 2 }}-#{h.select { |k, v| v > 0 }}"
