# spinel: gc-stress
# A program with threads and a finalizer: an array's walk that another loop
# encloses (a zip, the each_with_index arm) leaves its safepoint and
# finalizer polls to that loop, as a proved while does, so it can read a
# second array in place behind one test ahead of it (see
# test/loop_cobounded_reads.rb). Rows an outer loop fetches are read again
# at each run of the inner loop; worker threads read shared arrays while
# others build and drop garbage, and a finalizer runs between the outer
# loop's passes.

def matmul(a, t)
  a.map do |ai|
    t.map do |tj|
      s = 0.0
      ai.zip(tj) { |av, tv| s += av * tv }
      s
    end
  end
end

def matmul_while(a, t)
  out = []
  a.each do |ai|
    row = []
    t.each do |tj|
      s = 0.0
      k = 0
      while k < ai.size
        s += ai[k] * tj[k]
        k += 1
      end
      row << s
    end
    out << row
  end
  out
end

def cross(a, b)
  s = 0
  a.each do |x|
    b.each_index { |i| s += x * b[i] - a[i % a.size] }
  end
  s
end

# (a finalizer that writes nothing: one that pushed to a shared Array from
# whichever thread ran it would race, as such a push does in any thread)
def quiet_fin = proc { nil }

class Tracked
  def initialize(fin) = ObjectSpace.define_finalizer(self, fin)
end

n = 24
a = Array.new(n) { |i| Array.new(n) { |j| (i - j) * 0.125 } }
b = Array.new(n) { |i| Array.new(n) { |j| (i + j) * 0.25 } }
t = Array.new(n) { |j| Array.new(n) { |i| (i + j) * 0.25 } }
ref = matmul_while(a, t)
p matmul(a, t) == ref, ref[3][5], ref[n - 1].sum

workers = 4.times.map do |w|
  Thread.new do
    acc = 0.0
    10.times do |k|
      m = matmul(a, t)
      acc += m[(w + k) % n][k % n]
      Tracked.new(quiet_fin) if k.even?
      Array.new(300) { |i| "w#{w}-#{i}" }
    end
    acc
  end
end
p workers.map(&:value).sum.round(6)
GC.start
p cross(Array.new(30) { |i| i * 3 }, Array.new(40) { |i| i + 1 }), cross([], [1, 2])

# a shorter row partway: the checked copy runs it
def dot_rows(a, t)
  t.map do |tj|
    s = 0.0
    k = 0
    while k < a.size
      s += a[k] * tj[k]
      k += 1
    end
    s
  end
end
short = Array.new(n) { |j| Array.new(j == 7 ? 5 : n) { |i| i * 0.5 } }
begin
  p dot_rows(a[0], short).size
rescue => e
  p e.class
end
p dot_rows(a[1], Array.new(3) { |j| Array.new(n) { |i| i + j * 0.5 } })
