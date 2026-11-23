# A class method's bare `new` builds the receiving class, a subclass
# too, by an arm for each. A subclass whose initialize takes fewer
# arguments than the call gives raised ArgumentError for every receiver,
# the parent's too: the argument filler wrote its raise as a statement
# ahead of the arms. It raises in the subclass's arm now.
class Base
  def initialize(a, b)
    @a = a
    @b = b
  end
  def show = [@a, @b]
  def self.build(x) = new(x, 9)
end
class Sub < Base
  def initialize(a)
    super(a, 7)
  end
end
class Sub2 < Base; end
p Base.build(6).show, Sub2.build(5).show
p (Sub.build(1) rescue $!.message)
p Sub.new(1).show
