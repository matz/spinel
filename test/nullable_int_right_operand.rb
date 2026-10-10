# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# A nullable Integer / Float as the RIGHT operand: nil raises CRuby's
# coercion TypeError for + - * / % ** and the Comparable ArgumentError for
# < <= > >=, == is false and <=> nil; a present -2**63 is a number there.
def t(label)
  print label, ": "
  p yield
rescue => e
  puts "#{e.class}: #{e.message}"
end
n = ARGV.size
m = -9223372036854775807 - (n + 1)
a = [1, 2]
x = a.index(9)
t("1 + nil")   { 1 + x }
t("1 - nil")   { 1 - x }
t("2 * nil")   { 2 * x }
t("1 / nil")   { 1 / x }
t("1 % nil")   { 1 % x }
t("2 ** nil")  { 2 ** x }
t("1 < nil")   { 1 < x }
t("1 <= nil")  { 1 <= x }
t("1 > nil")   { 1 > x }
t("1 >= nil")  { 1 >= x }
t("1 == nil")  { 1 == x }
t("1 != nil")  { 1 != x }
t("1 <=> nil") { 1 <=> x }
t("1.eql? nil") { 1.eql?(x) }
t("1 & nil")   { 1 & x }
t("1 << nil")  { 1 << x }
t("1.0 + nil") { 1.0 + x }
t("1.0 * nil") { 1.0 * x }
t("1.0 < nil") { 1.0 < x }
t("1.0 == nil") { 1.0 == x }
t("1.0 <=> nil") { 1.0 <=> x }
t("n + nil")   { n + x }
t("n < nil")   { n < x }
t("1.between?(nil, 5)") { 1.between?(x, 5) }
f = [1.5].find { |e| e > 9 }
t("1 + fnil")  { 1 + f }
t("1 < fnil")  { 1 < f }
t("1.0 + fnil") { 1.0 + f }
t("1.0 < fnil") { 1.0 < f }
t("1.0 == fnil") { 1.0 == f }
y = [m].find { |e| e < 0 }
t("1 + min")   { 1 + y }
t("1 * min")   { 1 * y }
t("1 / min")   { 1 / y }
t("1 % min")   { 1 % y }
t("1 < min")   { 1 < y }
t("1 <= min")  { 1 <= y }
t("1 > min")   { 1 > y }
t("1 >= min")  { 1 >= y }
t("1 == min")  { 1 == y }
t("1 <=> min") { 1 <=> y }
t("1 & min")   { 1 & y }
t("1 | min")   { 1 | y }
t("1.0 + min") { 1.0 + y }
t("1.0 < min") { 1.0 < y }
t("1.0 == min") { 1.0 == y }
t("n + min")   { n + y }
t("n < min")   { n < y }
t("1.between?(min, 5)") { 1.between?(y, 5) }
t("1.clamp(min, 5)") { 1.clamp(y, 5) }
