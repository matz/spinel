# A class method of the program's named methods, private_methods or
# singleton_methods answers the call ahead of the compile-time list, and a
# reopened builtin's list asked of self in its body has the builtin's own
# singleton methods, as the list asked of the constant does.
class Custom
  def self.methods(all = true) = [:custom, all]
  def self.private_methods(all = true) = [:hidden, all]
  def self.x; end
  p methods(false), private_methods(false)
end
p Custom.methods(false), Custom.private_methods(false), Custom.singleton_methods(false).sort

class Listed
  def self.singleton_methods(all = true) = [:listed]
  def self.y; end
  p singleton_methods(false), methods(false).sort
end
p Listed.singleton_methods(false), Listed.methods(false).sort

class Array
  def self.own_helper; end
  p singleton_methods(false).sort == Array.singleton_methods(false).sort
  p singleton_methods(false).include?(:try_convert), methods(false).include?(:own_helper)
end
