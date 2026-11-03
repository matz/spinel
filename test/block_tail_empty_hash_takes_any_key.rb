# A block whose value is an empty {} hands back a hash that takes any key:
# the value reaches a map element, a fetch default or a yield's value, which
# the literal's own key sites cannot see.

module Store
  SLOTS = {}
  def self.get(key) = SLOTS[key]
  def self.set(key, value) = SLOTS[key] = value
  def self.fetch(key) = get(key) || set(key, yield)
end

p Store.fetch(:count) { 3 }
h = Store.fetch(:opts) { {} }
h[:a] = 1
p Store.fetch(:opts) { {} }

rows = [1, 2].map { {} }
rows[0][:k] = 1
rows[1][2] = "two"
p rows

d = {}.fetch(:x) { {} }
d[:z] = 1
p d

n = [1, 2].map { |x| next {} if x > 1; {} }
n[1][:n] = 5
p n

w = 1.then { ({}) }
w[:paren] = true
p w

# an unrelated `return` in the method changes nothing for its blocks
def rows_of(a)
  return [] if a.empty?

  r = a.map { {} }
  r[0][:k] = a.size
  r
end
p rows_of([1, 2])
p rows_of([])

def defaults(h, k)
  return nil if k.nil?

  v = h.fetch(k) { {} }
  v[:seen] = true
  v
end
p defaults({}, :a)
p defaults({}, nil)

def empty_in_parens = ({})
p empty_in_parens
