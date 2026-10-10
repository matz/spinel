# pop / shift / min / max / sample / delete / delete_at on a Float array
# that holds nils and the NaN whose payload was the Float sentinel: each
# answers the element, nil for a nil element or an empty array.
def t(label)
  print label, ": "
  p yield
rescue => e
  puts "#{e.class}: #{e.message}"
end
n = ARGV.size
x = ("\x01\x00\x00\x00\x00\x00\xF8\x7F" * (n + 1)).unpack1("d")
a = Array.new(4)
a[0] = 1.5
a[3] = x
t("a")        { a }
t("pop")      { a.pop }
t("pop nan?") { Array.new(1).tap { |d| d[0] = x }.pop.nan? }
t("pop nil")  { a.pop }
t("shift")    { a.shift }
t("shift nil") { a.shift }
t("a")        { a }
t("pop empty") { a.pop }
b = Array.new(3)
b[1] = 2.5
t("min nil")  { b.min }
t("max nil")  { b.max }
t("compact min") { b.compact.min }
t("[nil].min") { [b[0]].min }
t("[].max")   { [].max }
t("sample 1") { [b[1]].sample }
t("sample nil") { [b[0]].sample }
c = Array.new(3)
c[0] = 2.5
c[2] = 2.5
t("delete")   { c.delete(2.5) }
t("c")        { c }
t("delete nil") { c.delete(nil) }
t("c")        { c }
t("delete_at") { Array.new(2).tap { |d| d[1] = x }.delete_at(1) }
t("delete_at nil") { Array.new(2).delete_at(0) }
t("delete_at oob") { [1.5].delete_at(5) }
t("first")    { b.first }
t("last")     { b.last }
t("compact first") { b.compact.first }
t("index nil") { b.index(nil) }
t("count nil") { b.count(nil) }
t("compact")  { b.compact }
t("sum")      { b.compact.sum }
t("nan in")   { Array.new(2).tap { |d| d[0] = x }.map { |e| e.nil? ? "nil" : e.nan? } }
t("nan min")  { [x].min.nan? }
t("nan max")  { [x].max.nan? }
t("nan pop")  { [x].pop.nan? }
t("nan shift") { [x].shift.nan? }
t("nan sample") { [x].sample.nan? }
t("nan delete") { [x, 1.0].delete(1.0) }
t("nan first") { [x].first.nan? }
t("nan last") { [1.0, x].last.nan? }
t("nan compact") { [x, nil].compact.size }
t("nan nil?") { [x].pop.nil? }
