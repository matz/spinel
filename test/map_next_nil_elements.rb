# A map whose block can `next nil` (or a bare `next`) answers an Integer or
# Float Array that holds nil, and the elements read out of it may be nil:
# select / reject / filter_map test them by Ruby's truthiness.

def nn(a) = a.map { |x| next nil if x == 1; x }
def bare(a) = a.map { |x| next if x == 2; x * 2 }
def fl(a) = a.map { |x| next nil if x == 1; x * 1.5 }
def wi(a) = a.map.with_index { |x, i| next nil if i == 0; x + i }
def wif(a) = a.map.with_index(1) { |x, i| next nil if i == 1; x * 0.5 }
def wit(a) = a.map.with_index { |x, i| x > 2 ? nil : x }

a = [1, 2, 3]
p nn(a).select { |v| v }, nn(a).reject { |v| v.nil? }, nn(a).select(&:itself)
p bare(a).reject(&:nil?), fl(a).select { |v| v }, fl(a).compact.sum
p wi(a).select { |v| v }, wif(a).reject { |v| v.nil? }, wit(a).select { |v| v }
p nn(a).count { |v| v }, wi(a).map { |v| v ? v * 2 : 0 }
# a next that is never nil keeps the plain elements
p a.map { |x| next 0 if x == 1; x }.select { |v| v > 0 }
