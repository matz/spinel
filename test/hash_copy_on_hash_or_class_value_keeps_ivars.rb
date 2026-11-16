# A Hash or a class held in one value: `merge` with no arguments copies the
# Hash, and the copy carries the instance variable the program set on it.
class Rec
  def self.merge(other)
    other
  end
end

h = { 1 => "a", :b => 2 }
h.instance_variable_set(:@tag, 1)
x = [h, Rec][0]
y = x.merge
p y
p y.instance_variable_get(:@tag)
p y.equal?(h)
d = x.dup
p d.instance_variable_get(:@tag)
