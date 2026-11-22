# A hook that aliases a builtin's methods in a subclass of Array: in the
# class_eval block, by the alias keyword, and on the hook's parameter.
module Counted
  def self.included(base)
    base.class_eval do
      alias_method :count_all, :size
      alias old_push push
    end
    base.alias_method :head, :first
  end
end
class Stack < Array
  include Counted
  def push(x)
    old_push(x * 2)
  end
end
s = Stack.new
s.push(1)
s.push(2)
p s.count_all, s.head, s.to_a
