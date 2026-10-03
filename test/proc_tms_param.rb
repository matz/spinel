# A Process::Tms handed to a proc, a lambda or a block called through its
# Proc arrives boxed and is read back as the struct it is.

f = ->(tm) { tm.utime >= 0.0 }
p f.call(Process.times)
g = proc { |tm| [tm.stime >= 0.0, tm.class] }
p g.call(Process.times)
def run(x, &b) = b.call(x)
p run(Process.times) { |tm| tm.cutime >= 0.0 }
def gen
  yield Process.times
end
pr = proc { |tm| p tm.cstime >= 0.0 }
gen(&pr)
h = Hash.new { |hh, k| Process.times }
p h[:a].utime >= 0.0
