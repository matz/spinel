# A builtin module mixed into the including class by a hook:
# include Enumerable in the class_eval block: the instance side enumerates.
module M
  def self.included(base)
    base.class_eval do
      include Enumerable
    end
  end
end
class A
  include M
  def each
    yield 3
    yield 1
  end
end
p A.new.sort, A.new.map { |x| x + 1 }, A.new.include?(3), A.new.first
