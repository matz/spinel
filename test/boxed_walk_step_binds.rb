# A walk over a boxed receiver binds each step's values by the proc
# distribution. How many values a step yields is the run time's: one for an
# Array element or a Hash entry's pair, two for the key and value
# Hash#select and Hash#reject yield, as many as an Enumerator's step
# yielded. A rest, an optional, a post or a keyword therefore binds from
# the values gathered at run time: `select { |*r| }` bound nil, `sum` never
# ran such a block, `uniq` raised NoMethodError, and `count { |*r| }` over
# each_with_index bound [[3, 0]].

rows = [[[3, 1], [4, 2]], 1][ARGV.size]
hash = [{ 3 => 1, 4 => 2 }, 1][ARGV.size]
ewi = [[5, 6].each_with_index, 1][ARGV.size]

[rows, hash, ewi].each do |a|
  out = []
  a.each { |*r| out << r }
  a.each { |q, *r| out << [q, r] }
  a.each { |q, s = 7| out << [q, s] }
  a.each { |q, k: 1| out << [q, k] }
  a.each { |q, *r, z| out << [q, r, z] }
  p out
  p(a.map { |q, s = 7, *r| [q, s, r] })
  p(a.map { |*r, k: 1| [r, k] })
  p(a.select { |*r| out << r; true }.size, a.reject { |q, *r| r == [1] }.size)
  p(a.sort_by { |q, *r| -q })
  p(a.sum { |*r| r.size }, a.sum { |q, s = 0| s })
  p(a.uniq { |*r| r.size }.size, a.uniq { |q, k: 1| k }.size)
  p(a.filter_map { |*r| r }, a.flat_map { |q, *r| r })
  p(a.count { |*r| r.size == 2 }, a.any? { |q, *r, z| z == 0 })
  p out.last(2)
end

# a lone |x| takes the key of Hash#select, the first value of a step uniq is
# handed, and a block that binds nothing still runs
p(hash.select { |k| k > 3 }, hash.reject { |k| k > 3 })
p(ewi.uniq { |x| x })
p(rows.uniq { 1 })

# keywords, a `**` and an unused leading parameter
p(rows.map { |q, **kw| [q, kw] })
p(hash.sort_by { |q, s = 9, *r| r.size - s })
p(ewi.map { |q, *r| r })

# each_with_index, each_value and String / Float elements
named = [[["a", 1.5], ["b", 2.5]], 1][ARGV.size]
named.each_with_index { |*r| p r }
named.each_with_index { |q, i = 9, k: 1| p [q, i, k] }
nested = [{ "a" => [1, 2] }, 1][ARGV.size]
nested.each_value { |*r| p r }
nested.each_value { |q, *r| p [q, r] }
p(named.map { |q, s = "d"| q + s.to_s })
p(named.select { |q, *r| r[0] > 2.0 })
