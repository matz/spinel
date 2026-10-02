# A top-level `defined?(X)` guard sees a rooted `::Object::X` write, here
# made from inside a module body: the constant is Object's own, not the
# module's. (No mixin into Object anywhere, which would mask the case.)
module GuardWrap
  ::Object::RootedConst = :rooted
end
puts "missing rooted" unless defined?(RootedConst)
p RootedConst
