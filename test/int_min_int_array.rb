# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# -2**63 as an element of a typed Integer array: stored, read, printed,
# searched, summed, sorted, and never read as nil.
n = ARGV.size
m = -9223372036854775807 - (n + 1)
a = Array.new(3, 0)
a[1] = m
p a
p a[1]
p a[1].nil?
p(a[1] ? 1 : 0)
puts "#{a[1]}"
puts a.join(",")
puts a.to_s
a.each { |e| p e.nil? }
p a.map { |e| e & 1 }
p a.map { |e| e.nil? }
p a.find { |e| e < 0 }
p a.select { |e| e < 0 }
p a.count { |e| e != 0 }
p a.count(m)
p a.include?(m)
p a.index(m)
p a.min, a.max
p a.sum
p a.sort
p a.sort.reverse
p a.first, a.last
p a.dup
p a.reverse
b = a.dup
p b.pop, b.shift, b
p [m].first
p [m].last
p [m].pop
p a.minmax
p a.sum { |e| e }
p a.inject(0) { |s, e| s | e }
p a.any? { |e| e < 0 }
p a.all? { |e| e <= 0 }
p a.take_while { |e| e >= 0 }
p a.each_with_index.map { |e, i| [i, e] }
p a.zip(a)
p a.uniq
p a.tally
p a.group_by { |e| e < 0 }
p a.partition { |e| e < 0 }
