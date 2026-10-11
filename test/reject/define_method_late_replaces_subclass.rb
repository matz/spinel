# A class method's define_method defines on self, which can be a subclass:
# `R.make_fancy!` replaces R's own show (CRuby prints "plain 1", "fancy 2").
class Base
  def self.make_fancy! = define_method(:show) { |x| "fancy #{x}" }
end
class R < Base
  def show(x) = "plain #{x}"
end
p R.new.show(1)
R.make_fancy!
p R.new.show(2)
