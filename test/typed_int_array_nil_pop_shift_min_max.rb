# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# pop / shift / min / max / sample / delete / delete_at on an Integer array
# that holds nils and -2**63: each answers the element, nil for a nil
# element or an empty array, and min / max raise CRuby's ArgumentError when
# a nil meets a number.
def t(label)
  print label, ": "
  p yield
rescue => e
  puts "#{e.class}: #{e.message}"
end
n = ARGV.size
m = -9223372036854775807 - (n + 1)
a = Array.new(4)
a[0] = 5
a[3] = m
t("a")        { a }
t("pop")      { a.pop }
t("pop nil")  { a.pop }
t("a")        { a }
t("shift")    { a.shift }
t("shift nil") { a.shift }
t("a")        { a }
t("pop empty") { a.pop }
t("shift empty") { a.shift }
b = Array.new(3)
b[1] = 5
t("min nil")  { b.min }
t("max nil")  { b.max }
t("compact min") { b.compact.min }
t("compact max") { b.compact.max }
t("[nil].min") { [b[0]].min }
t("[nil].max") { [b[2]].max }
t("[].min")   { [].min }
t("sample 1") { [b[1]].sample }
t("sample nil") { [b[0]].sample }
t("sample empty") { [].sample }
c = Array.new(3)
c[0] = m
c[2] = m
t("delete m") { c.delete(m) }
t("c")        { c }
t("delete nil") { c.delete(nil) }
t("c")        { c }
t("delete missing") { c.delete(7) }
t("delete_at") { Array.new(2).tap { |d| d[1] = m }.delete_at(1) }
t("delete_at nil") { Array.new(2).delete_at(0) }
t("delete_at oob") { Array.new(2).delete_at(5) }
t("delete_if") { Array.new(3).tap { |d| d[0] = m }.delete_if(&:nil?) }
t("minmax")   { [m, 1].minmax }
t("min 2")    { Array.new(3).tap { |d| d[0] = m; d[1] = 1; d[2] = 2 }.min(2) }
t("sort nil") { b.sort }
t("sort_by")  { b.sort_by { |e| e || 0 } }
t("first")    { b.first }
t("last")     { b.last }
t("compact first") { b.compact.first }
t("compact last")  { b.compact.last }
t("[].first") { [].first }
t("index nil") { b.index(nil) }
t("index m")  { b.index(m) }
t("rindex nil") { b.rindex(nil) }
t("uniq")     { b.uniq }
t("sum")      { b.compact.sum }
t("count nil") { b.count(nil) }
t("insert")   { b.dup.insert(1, nil) }
t("unshift")  { b.dup.unshift(nil) }
t("push")     { b.dup.push(nil, m) }
t("reverse")  { b.reverse }
t("rotate")   { b.rotate }
t("slice")    { b[1, 2] }
t("concat")   { b + [nil, m] }
t("flatten")  { [b, [nil]].flatten }
t("zip")      { b.zip(b) }
t("sum nil")  { b.sum }
