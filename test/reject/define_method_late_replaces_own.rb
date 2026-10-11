# A define_method in a method's body defines its method when that method
# runs: CRuby prints "plain 1", then "fancy 2". Spinel registers it from the
# start, so the call before `make_fancy!` would get the replacement too;
# refused, not answered wrongly.
class R
  def show(x) = "plain #{x}"
  def self.make_fancy! = define_method(:show) { |x| "fancy #{x}" }
end
p R.new.show(1)
R.make_fancy!
p R.new.show(2)
