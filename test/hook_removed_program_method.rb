# A class method named remove_method answers the call in place of Module's:
# it removes nothing, so no method_removed hook is called and the method is
# still there at the next point.
class Base
  def self.remove_method(*names) = puts("custom remove #{names.inspect}")
end
class K < Base
  def self.method_removed(n) = puts("removed #{n}")
  def a = "a"
  remove_method :a
  puts new.a
  puts method_defined?(:a)
end
puts K.new.a
puts "done"
