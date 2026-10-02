# A boxed Hash or Array beside a user class owning the same names still
# answers the builtin when the keys or indices come splatted (#7051).
ATTRS = %i[ title url ]
IDX = [0, 2]
class NodeSet
  def initialize(a) = @a = a
  def slice(start, len = nil) = NodeSet.new(@a)
  def values_at(i) = 0
  def fetch_values(i) = 0
  def dig(i) = 0
  def except(i) = 0
  def delete(i) = 0
end
def pick(flag)
  flag ? NodeSet.new([1]) : { title: "T", url: "U", other: "O" }
end
def pick_a(flag)
  flag ? NodeSet.new([1]) : [10, 20, 30, 40]
end
v = pick(false)
p v.slice(*ATTRS)
p v.values_at(*ATTRS)
p v.fetch_values(*ATTRS)
p v.dig(*[:title])
p v.except(*ATTRS)
keys = [:title]
p v.slice(*keys)
p v.slice(:url, *keys)
w = pick(false)
p w.delete(*[:title])
p w
a = pick_a(false)
p a.slice(*IDX)
p a.values_at(*IDX)
p a.dig(*[1])
b = pick_a(false)
p b.delete(*[20])
p b
p pick(true).slice(*ATTRS).class
# typed receivers
h = { title: "T", url: "U", other: "O" }
p h.slice(*ATTRS), h.values_at(*ATTRS), h.fetch_values(*ATTRS), h.except(*ATTRS), h.dig(*[:url])
arr = [10, 20, 30, 40]
p arr.values_at(*IDX), arr.dig(*[2])
# a table keyed by more than one class takes each splatted key boxed
K = [1, "a"]
m = { 1 => 2, "a" => 3 }
p m.fetch_values(*K), m.values_at(*K)
def fv(h, *ks) = h.fetch_values(*ks)
p fv(m, 1, "a")
