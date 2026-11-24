# spinel: gc-stress
# A block loop a region holds (`n.times`, an Array's `each` and
# `each_with_index`) polls once per run of up to 1024 passes, ahead of its
# test, as a while does; one whose passes left fit in one run runs them as a
# plain loop. Each shape below runs across the edges of a run (no pass, one,
# 1023, 1024, 1025, 2049) in a threaded program, while another thread
# allocates, so collections stop the loops at their polls.

def tsum(a, n)
  s = 0
  n.times { |i| s += a[i] }
  s
end

def isum(a)
  s = 0
  a.each { |x| s += x }
  s
end

def fsum(f)
  s = 0.0
  f.each { |x| s += x }
  s
end

def byte_total(w)
  s = 0
  w.each { |x| s += x.bytesize }
  s
end

# counting a String's characters costs what it holds: this walk keeps its
# polls at each pass
def lens(w)
  s = 0
  w.each { |x| s += x.length }
  s
end

def wsum(a)
  s = 0
  a.each_with_index { |x, i| s += x * i }
  s
end

# a `next` ends a pass early
def evens(a)
  s = 0
  a.each { |x| next if x.odd?; s += x }
  s
end

# a store into another array, which may grow it
def copy(a, b)
  a.each_with_index { |x, i| b[i] = x + 1 }
  b.size
end

# a store into the array a count walks into, which cannot grow the count
def fill(r, n)
  n.times { |i| r[i] = i * 2 }
  r.size
end

# a raise midway through a run (the second, at the 1501st pass)
def until_zero(a)
  s = 0
  a.each { |x| s += 3000 / (x - 1500) }
  s
rescue ZeroDivisionError => e
  [s, e.message]
end

def ints(n) = Array.new(n) { |k| k % 7 + 1 }
def floats(n) = Array.new(n) { |k| (k % 5) * 0.5 }
def words(n) = Array.new(n) { |k| "w#{k % 13}" }

# (each kind of array is built in a method of its own, so each stays typed)
def int_row(n)
  a = ints(n)
  [n, tsum(a, n), isum(a), wsum(a), evens(a), copy(a, [0]), fill([0], n)]
end

$stop = false
ready = Queue.new
churn = Thread.new do
  junk = 0
  loop do
    x = Array.new(64) { |k| "s#{k}" }
    junk += x.size
    ready << true if junk == x.size
    break if $stop
    Thread.pass
  end
  junk > 0
end
# (the loops start once the allocating thread has, so it runs beside them)
ready.pop

work = Thread.new do
  out = []
  [0, 1, 1023, 1024, 1025, 2049].each { |n| out << int_row(n) }
  [0, 1025, 2049].each { |n| w = words(n); out << [byte_total(w), lens(w)] }
  [1023, 2049].each { |n| out << fsum(floats(n)) }
  out << until_zero(Array.new(3000) { |k| k })
  out
end

work.value.each { |row| p row }
$stop = true
p churn.value
