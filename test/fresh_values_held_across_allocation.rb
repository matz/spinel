# spinel: gc-stress
# A new object the generated code made and then held only in a C local was
# freed by the next allocation, which SPINEL_GC_STRESS=2 makes collect.

# a zero-argument call on a boxed receiver reaching an Object method: its
# empty rest's Array and **kwrest's Hash, and empty-literal defaults
class Box; end
class Object
  def try(*args, **kw) = [args, kw]
  def opt(a = [], b = {}) = [a, b]
end
[5, Box.new].each { |t| p t.try, t.opt }
# ...a callee that captures them makes the cell before it roots them
class Object
  def keep(*args, **kw) = -> { [args, kw] }
end
class Bag; def keep(*a) = a; end
[5, Box.new, Bag.new].each { |t| p t.keep.then { |k| k.is_a?(Proc) ? k.call : k } }

# each_slice and each_cons windows bound to a block parameter of another type
def windows(xs)
  r = []
  xs.each_slice(2) { |w| r << w.map { |x| x } }
  xs.each_cons(2) { |w| r << w.map { |x| x } }
  r
end
p windows([1, 2, 3])
p windows([[1], ["a"]].map { |v| v[0] })

# Hash methods on a Hash the call itself made
def h = yield
p h { { a: 1, b: nil } }.compact
p h { { a: 1, b: nil } }.compact!
p h { { a: 1, b: 2 } }.invert
p h { { a: 1, b: 2 } }.flatten

# spawn with a command String made for it
pid = spawn("echo #{6 * 7}")
Process.wait(pid)
pid = spawn(["echo", "argv0"], "x")
Process.wait(pid)

# Process::Status comparisons whose operands each make an object
system("exit 3")
p $? == $?, $? != $?
