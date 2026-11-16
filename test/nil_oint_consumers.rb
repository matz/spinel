# An Integer or Float that may be nil (an oint: a.first of an emptied
# Array) is nil where CRuby's nil is: a key of its own in a Hash, the
# operator's own TypeError as an operand, false for eql?, and
# NoMethodError as the receiver of an Integer iterator.

def t(label)
  print label, ": "
  p yield
rescue => e
  puts "#{e.class}: #{e.message}"
end

a = [1]
a.shift if ARGV.size == 0
x = a.first
t("/") { 7 / x }
t("%") { 7 % x }
t("divmod") { 7.divmod(x) }
t("div") { 7.div(x) }
t("modulo") { 7.modulo(x) }
t("&") { 6 & x }
t("|") { 6 | x }
t("^") { 6 ^ x }
t("<<") { 6 << x }
t(">>") { 6 >> x }
t("pow") { 7.pow(2, x) }
t("eql?") { 1.eql?(x) }
t("equal?") { 1.equal?(x) }
f = [1.5]
f.shift if ARGV.size == 0
y = f.first
t("Float eql?") { 1.5.eql?(y) }

b = [1]
b.shift if ARGV.size == 0
x = b.first
h = { 1 => 2 }
h[x] = 3
p h, h[x], h.fetch(x, :d), h.key?(x), h.dig(x)
g = { 1 => 2 }
g.store(x, 4)
p g, g.values_at(x, 1), g.delete(x), g
k = Hash.new(0)
k[x] += 1
k[1] += 1
k[x] += 1
p k
m = { 1 => 2.5 }
m[x] = 1.5
m.each { |kk, vv| puts "#{kk.inspect}=#{vv}" }

def ti(label)
  print label, ": "
  r = []
  yield r
  p r
rescue => e
  puts "#{e.class}: #{e.message}"
end
c = [2]
c.shift if ARGV.size == 0
x = c.first
ti("times") { |r| x.times { |i| r << i } }
ti("upto") { |r| x.upto(3) { |i| r << i } }
ti("downto") { |r| x.downto(0) { |i| r << i } }
ti("step") { |r| x.step(4, 2) { |i| r << i } }
ti("step float") { |r| x.step(3, 0.5) { |i| r << i } }
ti("times.to_a") { |r| r.concat(x.times.to_a) }
f = [2.5]
f.shift if ARGV.size == 0
y = f.first
ti("Float step") { |r| y.step(4) { |i| r << i } }
