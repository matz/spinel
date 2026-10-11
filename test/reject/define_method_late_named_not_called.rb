# A method nothing calls defines nothing, but here a String spells its name,
# so its define_method is registered: CRuby prints "plain 1".
class R
  def show(x) = "plain #{x}"
  def self.make_fancy! = define_method(:show) { |x| "fancy #{x}" }
end
p "make_fancy!"
p R.new.show(1)
