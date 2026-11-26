# obj.method(name) with a run-time name, in a class method written before the
# class includes a module whose method yields (Minitest's Result.from shape):
# the Method binds the yielding method's proc form
module A
  def sync = yield
end

class T
  def self.get(t, name) = t.method(name)
end

class T
  include A
end

p T.new.sync { 1 }
m = T.get(T.new, ["sync"].first)
p m.name, m.arity, m.call { 42 }
begin
  T.get(T.new, ["zzz"].first)
rescue NameError => e
  p e.class, e.name
end
