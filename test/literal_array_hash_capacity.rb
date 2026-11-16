# An Array or Hash literal takes its buffer at its length: `[a, b, c]` the
# four elements an Array starts with at the least, `{k => v, ...}` of n pairs
# the slots that hold n entries without growing (a Hash grows at half full).
# They took sixteen whatever they held; an empty `[]` or `{}` still does.
#
# Each kind is built here, as a literal and from empty, and grown past its
# first buffer and past sixteen, with the operations that move elements
# around a buffer (shift and push as a queue, unshift, insert, delete, clear
# and refill, nil elements and nil values, which keep a bitmap the size of
# the buffer), a literal with a splat, which still starts from empty, and an
# Array or Hash subclass, whose instance embeds its first buffer.
#
# The last part counts what a small literal keeps alive. Spinel's GC.stat
# has "bytes", the live bytes its collector counts, which includes an Array's
# element buffer and a Hash's tables. A three-element Array took 224 bytes
# and a four-pair Hash 504 to 648 on a 64-bit host, most of it the sixteen
# slots it did not use. CRuby has no such counter; its small Array and Hash
# each live in one heap slot, counted here at the smallest slot's 40.

class Pt
  attr_reader :x

  def initialize(x)
    @x = x
  end
end

class Row < Array
end

class Table < Hash
end

# ---- Arrays ----

ints = []
20.times { |i| ints << i * 3 }
p ints
p ints.size

q = []
40.times do |i|
  q << i
  q.shift if i.odd?
end
p q

u = []
10.times { |i| u.unshift(i) }
u.insert(2, 100)
p u

holes = Array.new(3)
holes[5] = 7
p holes
holes << 8
holes[1] = 1
p holes

floats = []
9.times { |i| floats << i * 0.5 }
floats << 4.75
p floats
fl = [1.5, 2.5]
fl[3] = 3.5
p fl

objs = []
18.times { |i| objs << Pt.new(i) }
p objs.map(&:x)

rows = []
6.times { |i| rows << [i, i + 1] }
rows << { "k" => 1 }.keys
p rows

squares = [1, 2, 3].map { |v| v * v }
squares << 10
squares << 11
p squares

filled = Array.new(3, 9)
filled << 1
filled << 2
p filled

row = Row.new
6.times { |i| row << i }
p row
p row.class

lit = [1, 2, 3]
20.times { |i| lit << i * 2 }
p lit
p [0, *lit.first(3), 9]
flit = [1.5]
6.times { |i| flit << i * 0.25 }
p flit

# ---- Hashes ----

syml = { id: 1 }
syml[:x] = 2
syml[:y] = 3
syml[:z] = 4
syml.delete(:id)
syml[:w] = 5
p syml
strl = { "a" => 1, "b" => nil }
10.times { |i| strl["k" + i.to_s] = i }
p strl
intl = { 1 => "one", 2 => "two" }
10.times { |i| intl[i + 10] = "v" + i.to_s }
p intl
nine = { a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8, i: 9 }
nine[:j] = 10
p nine
mixed = { 1 => "a", "b" => 2, x: 1 }
6.times { |i| mixed[i * 3] = i }
p mixed
base = { a: 1, b: 2 }
spread = { **base, c: 3 }
spread[:d] = 4
p spread

ii = {}
20.times { |i| ii[i * 5] = i }
5.times { |i| ii.delete(i * 10) }
p ii
iin = {}
iin[1] = nil
5.times { |i| iin[i + 10] = i }
p iin

is = {}
12.times { |i| is[i] = "v" + i.to_s }
p is

si = {}
12.times { |i| si["k" + i.to_s] = i }
si.delete("k3")
si["k3"] = 33
p si
sin = {}
sin["a"] = nil
6.times { |i| sin["n" + i.to_s] = i }
p sin

ss = {}
10.times { |i| ss["k" + i.to_s] = "v" + i.to_s }
p ss

sp = {}
10.times { |i| sp["k" + i.to_s] = i.even? ? i : "s" + i.to_s }
p sp

sym = { id: 1, a: 2, b: 3, c: 4 }
sym[:d] = 5
sym[:e] = 6
p sym

pp_h = {}
10.times { |i| pp_h[i.even? ? i : "k" + i.to_s] = i.even? ? "v" : i }
p pp_h

cl = {}
12.times { |i| cl[i] = i }
cl.clear
3.times { |i| cl[i + 100] = i }
p cl

counts = Hash.new(0)
"the quick brown fox jumps over the lazy dog".each_char { |c| counts[c] += 1 }
p counts.size
p counts["o"]

merged = { "a" => 1, "b" => 2 }.merge({ "c" => 3, "d" => 4, "e" => 5 })
p merged
copy = merged.dup
copy["f"] = 6
p copy.size
p merged.size

t = Table.new
10.times { |i| t[i] = i * i }
p t
p t.class

# ---- what a small Array or Hash keeps alive ----

N = 10_000

def live_bytes
  GC.start
  s = GC.stat
  s.key?("bytes") ? s["bytes"] : GC.stat(:heap_live_slots) * 40
end

def report(name, bytes, limit)
  puts name + ": at most " + limit.to_s + " bytes: " + (bytes <= limit ? "yes" : "no (" + bytes.to_s + ")")
end

a3 = Array.new(N)
b0 = live_bytes
N.times { |i| a3[i] = [i, i + 1, i + 2] }
report("[i, i + 1, i + 2]", (live_bytes - b0) / N, 160)

f1 = Array.new(N)
b0 = live_bytes
N.times { |i| f1[i] = [i * 0.5] }
report("[f]", (live_bytes - b0) / N, 160)

hs = Array.new(N)
b0 = live_bytes
N.times { |i| hs[i] = { id: i, a: i, b: i, c: i } }
report("{id:, a:, b:, c:}", (live_bytes - b0) / N, 520)

hstr = Array.new(N)
b0 = live_bytes
N.times { |i| hstr[i] = { "id" => i, "a" => i, "b" => i, "c" => i } }
report("{\"id\" => i, ...}", (live_bytes - b0) / N, 408)

hi = Array.new(N)
b0 = live_bytes
N.times { |i| hi[i] = { i => i } }
report("{i => i}", (live_bytes - b0) / N, 428)

p a3.size + f1.size + hs.size + hstr.size + hi.size
