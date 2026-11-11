# A push reads the length once and stores it plus one: the element lands
# after the last one through every grow path -- a full buffer, a buffer a
# shift left room at the front of, a frozen array -- for Integer, Float and
# mixed arrays, and in the results map, select and filter_map build.

ints = []
i = 0
while i < 100
  ints << i * 3
  i += 1
end
p ints.length, ints[0], ints[99], ints.sum

q = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]
5.times { q.shift }
40.times { |k| q << 100 + k }
p q.length, q.first, q.last, q.sum

fl = []
50.times { |k| fl << k * 0.5 }
p fl.length, fl[0], fl[49], fl.sum

fq = [0.25, 0.5, 0.75, 1.0]
3.times { fq.shift }
20.times { |k| fq << k + 0.125 }
p fq.length, fq.first, fq.last

mix = []
30.times { |k| mix << (k.even? ? k : "s#{k}") }
p mix.length, mix[0], mix[1], mix[29]

mq = [1, "a", 2.5, :b]
2.times { mq.shift }
20.times { |k| mq << (k.odd? ? k : k.to_s) }
p mq.length, mq.first, mq.last

src = (1..40).to_a
p src.map { |x| x * 2 }.last
p src.map { |x| x * 0.5 }.last
p src.select { |x| x % 3 == 0 }.length
p src.filter_map { |x| x.even? ? x.to_s : nil }.last

fr = [1, 2].freeze
begin
  fr << 3
rescue FrozenError => e
  puts "FrozenError #{fr.length}"
end
frf = [1.5].freeze
begin
  frf << 2.5
rescue FrozenError => e
  puts "FrozenError #{frf.length}"
end
