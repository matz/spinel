# A define_method of the program's own, in an unrelated class, is not the one
# a class method of R calls: CRuby prints "helper x", "plain", "fancy".
class Helper
  def define_method(name) = "helper #{name}"
end
p Helper.new.define_method(:x)
class R
  def show(x) = "plain"
  def self.make_fancy! = define_method(:show) { |x| "fancy" }
end
p R.new.show(1)
R.make_fancy!
p R.new.show(2)
