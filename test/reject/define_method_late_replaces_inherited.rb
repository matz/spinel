# The method a define_method replaces can come from a superclass or an
# included module: before `make_fancy!` runs, CRuby still calls that one.
module Plain
  def show(x) = "plain #{x}"
end
class Base
  def tag = "base"
end
class R < Base
  include Plain
  def self.make_fancy!
    define_method(:show) { |x| "fancy #{x}" }
    define_method(:tag) { "fancy" }
  end
end
p R.new.show(1)
p R.new.tag
R.make_fancy!
p R.new.show(2)
