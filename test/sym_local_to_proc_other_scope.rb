# A Symbol local whose only write in its own scope is a literal is read as
# that Symbol, whatever another method's local of the same name holds: the
# other method's `m` is another variable. Its to_proc then calls the method
# a `module Enumerable` reopening adds, as a written call does.

module Enumerable
  def special? = count > 1
end

def width(keys)
  m = keys.size
  m
end

a = [1, 2]
m = :special?
p m.to_proc.call(a)
p m.to_proc.call([7])
p width(a)
