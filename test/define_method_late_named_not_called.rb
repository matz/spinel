# A String spells the name of a class method nothing calls: its
# define_method is a version of R that only that method's run would make, so
# R#show stays the def, and CRuby prints "plain 1".
class R
  def show(x) = "plain #{x}"
  def self.make_fancy! = define_method(:show) { |x| "fancy #{x}" }
end
p "make_fancy!"
p R.new.show(1)
