# A method a subclass inherits refuses a frozen subclass instance, as the
# subclass's own would: plain writes, ||= / &&= on every slot kind, Strings too.
class Base
  def set = (@x = 1)
  def memo = (@y ||= 2)
  def name_memo = (@s ||= +"n")
  def name_stmt
    @t ||= +"t"
    :done
  end
  def name_and
    @u &&= +"u"
  end
  def prime = (@u = +"p")
end
class Child < Base; end
[:set, :memo, :name_memo, :name_stmt, :name_and].each do |m|
  begin
    c = Child.new
    c.prime
    c.freeze.public_send(m)
    p [m, :written]
  rescue FrozenError => e
    p [m, e.class]
  end
end
