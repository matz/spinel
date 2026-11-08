# spinel: gc-stress
# A Range of Symbols built from ends only known at run time read their ids
# as numbers (`0..1`), as did an endless or mixed literal (`:a..`). It walks
# and compares as a String Range of the Symbols' names does, and answers
# each end and member as a Symbol.

def t(s)
  r = yield
  puts "#{s}: #{r.inspect}"
rescue StandardError => e
  puts "#{s}: #{e.class}: #{e.message}"
end

def probe(label, r, ex, open_end, open_begin, same)
  t("#{label} p") { r }
  t("#{label} to_s") { r.to_s }
  t("#{label} excl") { ex }
  t("#{label} to_a") { r.to_a }
  t("#{label} each") { x = []; r.each { |v| x << v }; x }
  t("#{label} map") { r.map(&:to_s) }
  t("#{label} ends") { [r.begin, r.end, r.first, r.last, r.min, r.max] }
  t("#{label} first last n") { [r.first(2), r.last(2)] }
  t("#{label} include?") { [r.include?(:c), r.include?("c"), r.include?(:z), r.member?(:b)] }
  t("#{label} cover?") { [r.cover?(:cc), r.cover?("c")] }
  t("#{label} ===") { [r === :d, r === "d"] }
  t("#{label} ==") { [r == same, r == ("a".."e"), r.eql?(same), r.hash == same.hash] }
  t("#{label} count size") { [r.count, r.size] }
  t("#{label} minmax") { r.minmax }
  t("#{label} sum") { r.sum("") }
  t("#{label} select") { r.select { |s| s != :b } }
  t("#{label} step") { x = []; r.step(2) { |s| x << s }; x }
  t("#{label} bsearch") { r.bsearch { |s| true } }
  t("#{label} reverse_each") { r.reverse_each.to_a }
  t("#{label} each_slice") { r.each_slice(2).to_a }
  t("#{label} each_with_index") { x = []; r.each_with_index { |s, i| x << [s, i] }; x }
  t("#{label} splat") { [*r] }
  t("#{label} frozen class") { [r.frozen?, r.class, r.dup, ex.exclude_end?] }
  t("#{label} endless") { [open_end, open_end.begin, open_end.end] }
  t("#{label} endless include?") { open_end.include?(:c) }
  t("#{label} endless cover") { open_end.cover?(:zz) }
  t("#{label} beginless") { [open_begin, open_begin.cover?(:c)] }
  t("#{label} beginless include?") { open_begin.include?(:c) }
  x = []
  for s in r
    x << s
  end
  t("#{label} for") { x }
  w = case :c
      when r then "in"
      else "out"
      end
  t("#{label} when") { w }
end

s = [1, :a, :e, nil, "e", :"a b", :"a d"]
probe("box", s[1]..s[2], s[1]...s[2], (s[1]..), (..s[2]), s[1]..s[2])
t("lit endless") { e = (:a..); [e, e.begin, e.cover?(:zz)] }
t("quoted box") { [s[5]..s[6], (s[5]..s[6]).to_a] }
t("nil end") { s[1]..s[3] }
t("sym int") { s[1]..s[0] }
t("int sym") { s[0]..s[1] }
t("sym str") { s[1]..s[4] }
t("lit sym int") { :a..s[0] }
t("in array") { [s[1]..s[2], 1] }
h = {lo: :x, hi: :z}
t("hash") { (h[:lo]..h[:hi]).to_a }
def lo = :m
def hi = :o
t("methods") { (lo..hi).to_a }
x = s[1]
t("local") { [(x..:c).to_a, (:a..x)] }

# a class of the program's own that defines the same names puts the calls on
# a dispatch, which takes boxed String Ranges and Ranges of Symbols too
class Bag
  include Enumerable
  def initialize(a) = (@a = a)
  def each(&b) = @a.each(&b)
  def map(&b) = @a.map(&b)
  def select(&b) = @a.select(&b)
  def sum = @a.sum
  def first = @a[0]
end
t("bag") { b = Bag.new([3, 1]); [b.map { |v| v * 2 }, b.select { |v| v > 1 }, b.sum, b.first] }
z = [1, "b", "e"]
sr = z[1]..z[2]
t("user map") { [sr.map(&:upcase), (s[1]..s[2]).map(&:to_s)] }
t("user select") { [sr.select { |v| v > "c" }, (s[1]..s[2]).select { |v| v > :c }] }
t("user each") { x = []; sr.each { |v| x << v }; (s[1]..s[2]).each { |v| x << v }; x }
t("user sum first") { [sr.sum(""), sr.first, (s[1]..s[2]).first] }
t("to_set") { [sr.to_set.size, (s[1]..s[2]).to_set.include?(:b)] }
t("[]") { sr[0] }
t("[] sym") { (s[1]..s[2])[0] }
t("cover range") { [sr.cover?(z[1]..z[2]), sr.cover?("c".."d"), sr.cover?("a".."c"), (s[1]..s[2]).cover?(s[1]..s[1])] }
t("key?") { sr.key?("c") }
iv = [1, "x", 4]
t("int beside bag") { r = iv[0]..iv[2]; [r.sum(5), r.first(2), r.sum, r.map { |v| v * 2 }] }
