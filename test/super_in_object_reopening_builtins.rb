# A super in a program's reopening of Object reaches the builtin method of
# the name that CRuby's Object, Kernel or BasicObject has, for any receiver.
class Object
  def ==(x) = super
  def eql?(x) = super
  def equal?(x) = super
  def itself = super
  def frozen? = super
  def nil? = super
  def !() = super
  def respond_to_missing?(name, include_all = false) = name.to_s.start_with?("ghost_") || super
end
o = Object.new
p [o == o, o == 1, o.eql?(o), o.eql?(2), o.equal?(o), o.equal?(2)]
p o.itself.equal?(o)
p [o.frozen?, o.freeze.frozen?]
p [o.nil?, !o]
p o.send(:respond_to_missing?, :ghost_a, false)
p o.send(:respond_to_missing?, :other, false)
