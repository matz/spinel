# `a[i, n] = [x]` and `a[i..j] = [x]` put the elements of the Array on the
# right into the receiver: a read of the receiver can be x, and a call on it
# dispatches to x's class's method. The start and the length may come from a
# splat, the Range from a local or a parameter, the call through send.
class A; def foo = "A"; end
class B; def foo = "B"; end
class C; def foo = "C"; end

s = [A.new]
s[1, 0] = [B.new]
s.each { |v| puts v.foo }

r = [A.new]
r[1..1] = [C.new]
r.each { |v| puts v.foo }

rg = 1..1
q = [A.new]
q[rg] = [B.new]
q.each { |v| puts v.foo }

sl = [1, 0]
t = [A.new]
t[*sl] = [C.new]
t.each { |v| puts v.foo }

u = [A.new]
u.send(:[]=, 1, 0, [B.new])
u.each { |v| puts v.foo }

x = [A.new]
x[1..] = [C.new]
x.each { |v| puts v.foo }

def put(ary, at, val)
  ary[at] = val
  ary
end
put([A.new], 1..1, [B.new]).each { |v| puts v.foo }

# `h[k] = list` keeps the list whole, as one element: what the Hash holds
# is the list, and no Row in it. Read as a splice, the Rows reached the Hash
# beside it, a value read from that one could be a Row, and `set[row.id] =
# true` on it got an arm for Row's own []=, a write of true to any member.
# The members widened to boxed values and a Row took 88 bytes for 72.
Row = Struct.new(:id, :n)

class Store
  def initialize
    @sets = {}
    @lists = {}
  end

  def add(count)
    i = 0
    while i < count
      row = Row.new("r#{i}", i % 2)
      set = @sets[row.n]
      if set.nil?
        set = {}
        @sets[row.n] = set
      end
      set[row.id] = true
      list = @lists[row.id]
      if list.nil?
        list = []
        @lists[row.id] = list
      end
      list << row
      i += 1
    end
  end

  def count(n)
    @sets[n].size + @lists["r0"].size
  end
end

st = Store.new
st.add(5)
puts st.count(1)

# Spinel's GC.stat has "bytes", the live bytes its collector counts. CRuby
# has no such counter; its Row lives in one heap slot, counted at 40.
def live_bytes
  GC.start
  s = GC.stat
  s.key?("bytes") ? s["bytes"] : GC.stat(:heap_live_slots) * 40
end

N = 10_000
rows = Array.new(N)
id = "r"
b0 = live_bytes
N.times { |i| rows[i] = Row.new(id, i) }
per = (live_bytes - b0) / N
puts rows.size
puts "a Row of a String and an Integer: at most 72 bytes: " + (per <= 72 ? "yes" : "no (" + per.to_s + ")")
