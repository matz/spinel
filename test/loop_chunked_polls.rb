# spinel: gc-stress
# A loop in a region that runs no code (a proved `i < a.length`, or one
# counted to an Integer bound its body leaves as it is) polls once per run of
# up to 1024 passes, ahead of its test, not at each pass. Each shape below
# runs across the edges of a run (no pass, one, 1023, 1024, 1025, 2049) in a
# threaded program, while another thread allocates, so collections stop the
# loops at their polls, and must answer what the loop answers pass by pass.

class Box
  attr_reader :n
  def initialize(n); @n = n; end

  # an instance variable's bound
  def sum_to(a)
    s = 0
    j = 0
    while j < @n
      s += a[j]
      j += 1
    end
    s
  end

  # ...that the body lowers with `&&=` midway through a run, so it is no
  # bound the run may count to
  def sum_lowered(a, at, to)
    s = 0
    j = 0
    while j < @n
      s += a[j]
      @n &&= to if j == at
      j += 1
    end
    [s, j, @n]
  end
end

# proved: i < a.length, read in place
def psum(a)
  s = 0
  i = 0
  while i < a.length
    s += a[i]
    i += 1
  end
  s
end

# proved, a step of 3
def psum3(a)
  s = 0
  i = 0
  while i < a.length
    s += a[i]
    i += 3
  end
  s
end

# proved, storing into another array of the length
def pscale(a, r)
  i = 0
  while i < a.length
    r[i] = a[i] * 2
    i += 1
  end
  r
end

# a local bound, <, a body that grows the array it stores into
def copy_n(a, n)
  b = [0]
  j = 0
  while j < n
    b[j] = a[j] + 0
    j += 1
  end
  b
end

# <= a local bound, a step of 2
def every_other(a, last)
  s = 0
  j = 0
  while j <= last
    s += a[j]
    j += 2
  end
  s
end

# counting down: > and >= a bound, steps of 1 and 5
def down(a)
  s = 0
  j = a.length - 1
  while j >= 0
    s += a[j]
    j -= 1
  end
  t = 0
  k = a.length - 1
  while k > 3
    t += a[k]
    k -= 5
  end
  [s, t]
end

# a literal bound and a Float array
def fsum_lit(f)
  s = 0.0
  j = 0
  while j < 1500
    s += f[j]
    j += 1
  end
  s
end

# a counted loop that leaves by return midway through a run
def first_over(a, t)
  j = 0
  while j < a.length
    return j if a[j] > t
    j += 1
  end
  -1
end

# a read past the end raises midway through a run
def over_read(a, n)
  s = 0
  j = 0
  while j < n
    s += a[j]
    j += 1
  end
  s
rescue NoMethodError, TypeError => e
  [s, e.class]
end

# a pass that counts a String's characters costs what the String holds:
# such a loop keeps its polls at each pass
def char_count(w, n)
  s = 0
  j = 0
  while j < n
    s += w[j].length
    j += 1
  end
  s
end

# ...as does such a loop nested in another, which would otherwise leave its
# polls to the enclosing loop
def char_rows(w, reps)
  s = 0
  r = 0
  while r < reps
    j = 0
    while j < w.length
      s += w[j].length
      j += 1
    end
    r += 1
  end
  s
end

def words(n) = Array.new(n) { |k| "w\u00e9#{k}" }
def ints(n) = Array.new(n) { |k| k % 7 + 1 }
def floats(n) = Array.new(n) { |k| (k % 5) * 0.5 }

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
  [0, 1, 1023, 1024, 1025, 2049].each do |n|
    a = ints(n)
    out << [n, psum(a), psum3(a), pscale(a, Array.new(n, 0)).sum, copy_n(a, n).size,
            n > 0 ? every_other(a, n - 1) : 0, down(a), Box.new(n).sum_to(a),
            first_over(a + [100], 50)]
  end
  [1500, 2049].each { |n| out << fsum_lit(floats(n)) }
  out << over_read(Array.new(1030, 1), 2000)
  out << Box.new(3000).sum_lowered(ints(3000), 10, 20)
  out << char_count(words(1500), 1500)
  out << char_rows(words(1500), 2)
  out
end

work.value.each { |row| p row }
$stop = true
p churn.value
