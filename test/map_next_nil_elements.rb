# A map whose block can `next nil` (or a bare `next`) answers an Integer or
# Float Array that holds nil, and the elements read out of it may be nil:
# select / reject / filter_map test them by Ruby's truthiness.

def nn(a) = a.map { |x| next nil if x == 1; x }
def bare(a) = a.map { |x| next if x == 2; x * 2 }
def fl(a) = a.map { |x| next nil if x == 1; x * 1.5 }
def wi(a) = a.map.with_index { |x, i| next nil if i == 0; x + i }
def wif(a) = a.map.with_index(1) { |x, i| next nil if i == 1; x * 0.5 }
def wit(a) = a.map.with_index { |x, i| x > 2 ? nil : x }

# a nil `next` after the first 32 a block holds still marks the elements
def many(a)
  a.map do |x|
    next 0 if x == 100
    next 1 if x == 101
    next 2 if x == 102
    next 3 if x == 103
    next 4 if x == 104
    next 5 if x == 105
    next 6 if x == 106
    next 7 if x == 107
    next 8 if x == 108
    next 9 if x == 109
    next 10 if x == 110
    next 11 if x == 111
    next 12 if x == 112
    next 13 if x == 113
    next 14 if x == 114
    next 15 if x == 115
    next 16 if x == 116
    next 17 if x == 117
    next 18 if x == 118
    next 19 if x == 119
    next 20 if x == 120
    next 21 if x == 121
    next 22 if x == 122
    next 23 if x == 123
    next 24 if x == 124
    next 25 if x == 125
    next 26 if x == 126
    next 27 if x == 127
    next 28 if x == 128
    next 29 if x == 129
    next 30 if x == 130
    next 31 if x == 131
    next 32 if x == 132
    next nil if x == 1
    x
  end
end

a = [1, 2, 3]
p nn(a).select { |v| v }, nn(a).reject { |v| v.nil? }, nn(a).select(&:itself)
p bare(a).reject(&:nil?), fl(a).select { |v| v }, fl(a).compact.sum
p wi(a).select { |v| v }, wif(a).reject { |v| v.nil? }, wit(a).select { |v| v }
p nn(a).count { |v| v }, wi(a).map { |v| v ? v * 2 : 0 }
# a next that is never nil keeps the plain elements
p a.map { |x| next 0 if x == 1; x }.select { |v| v > 0 }
p many(a).select { |v| v }, many(a).reject(&:nil?)
