# A public_send with a name known only at run time probes each candidate
# arm under its own recovery point, dropping the arms that do not emit.
# A refusal later in the same method has to land on the method's own
# recovery point: left pointing into the returned probe frame, it jumped
# there and the compiler aborted instead of reporting the one refusal.
class Box
  def initialize(v) = @v = v
  def get = @v
  def put(x) = @v = x
  def size = 1
end

def run(o, name, arg)
  r = o.public_send(name, arg)
  x = proc { "a" }.ruby2_keywords
  [r, x]
end

p run(Box.new(1), :put, 2)
p run([1, 2, 3], :push, 4)
p run("ab", :center, 6)
