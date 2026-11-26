# A `return yield(x)` answers each call site's own block value when the
# blocks answer different types: the method's return was typed from the
# first site, and a later call whose block answers another type read nil.
def f(x)
  return yield(x) if x > 1
  nil
end
f(2) { nil }
p f(7) { |x| x * 2 }

def g(x)
  return yield(x) if x > 1
  nil
end
p g(2) { |x| "s#{x}" }
p g(7) { |x| x * 2 }
p g(0) { |x| x }

# (QC's program, less a synchronize statement whose return needs retsync)
M = Mutex.new
def ens(xs)
  begin
    xs.each { |x| return yield(x) if x > 1 }
  ensure
    $e = ($e || 0) + 1
  end
  nil
end
def sync(xs)
  M.synchronize { xs.each { |x| return yield(x) if x > 1 } }
  nil
end
def run1
  ens([1, 2]) { |x| return :from_run1 if x > 1 }
  :after1
end
def run2
  sync([1, 2]) { |x| return [:from_run2, x] if x > 1 }
  :after2
end
def run3
  ens([1, 3]) { |x| $s3 = x }
  "str"
end
def run4 = (v = ens([0, 7]) { |x| x * 2 }; [v, 1.5])
p run1, run2, run3, $s3, run4, $e, M.locked?
