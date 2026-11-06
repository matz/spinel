# The edges of a Range of Times, as CRuby answers them: first and last of an
# open end, include? and === of a Range, iteration of an endless one, dup
# and clone, a comparator block, a nil when-pattern.
a = Time.at(0).utc; b = Time.at(10).utc; m = Time.at(5).utc
r = a..b
def try
  yield
rescue => e
  "#{e.class}: #{e.message}"
end
p r.include?(r), r === r, r.member?(r), r.cover?(r)
h = { r: r }
p h[:r].include?(r), h[:r] === r, h[:r].cover?(r)
p try { (..b).first }, try { (a..).last }, (..b).begin, (a..).end
p try { (a..).to_a }, (a..).count, try { r.to_a }, try { r.count }
p try { r.min { |x, y| x <=> y } }, try { r.max { |x, y| x <=> y } }
d = r.dup
p d.equal?(r), d.frozen?, r.clone.equal?(r), r.clone.frozen?, r.frozen?
p try { r.each { |t| p t } }
x = ARGV.empty? ? nil : r
case m
when x then puts "in"
else puts "out"
end
p (a..b).to_s, (a...b).inspect
