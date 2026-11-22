# A builtin module mixed into the including class by a hook:
# extend Enumerable and def self.each in the class_eval block, and a subclass.
module M
  def self.included(base)
    base.class_eval do
      extend Enumerable
      def self.each
        yield 2
        yield 5
      end
    end
  end
end
class A
  include M
end
class B < A
end
p A.to_a, A.max, B.sort, B.count
