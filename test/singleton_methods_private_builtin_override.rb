# A reopened builtin's class method of a builtin name made private leaves
# the singleton_methods(false) list, asked of self in the body or by name.
class Array
  def self.try_convert(x) = nil
  private_class_method :try_convert
  def self.own_helper; end
  p singleton_methods(false).include?(:try_convert), singleton_methods(false).include?(:own_helper)
end
p Array.singleton_methods(false).include?(:try_convert), Array.singleton_methods(false).include?(:own_helper)
p Array.singleton_methods(false).include?(:[])
