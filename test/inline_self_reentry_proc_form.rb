# A method that uses its block and reaches itself again (method_missing
# whose body calls __getobj__ and target_respond_to? on self, which the base
# class leaves to method_missing) is not spliced into its own copy: the
# re-entering call goes through the method's proc form. Two such calls made
# 2^depth copies before the inline depth bound, and the compile did not end.
class Delegator
  def self.const_missing(n)
    ::Object.const_get(n)
  end
  def method_missing(m, *args, &block)
    r = true
    target = self.__getobj__
    if r && target_respond_to?(target, m, false)
      target.__send__(m, *args, &block)
    end
  end
end

class Two < Delegator
  def initialize(o) = @o = o
  def __getobj__ = @o
  def target_respond_to?(t, m, i) = t.respond_to?(m, i)
end

p Two.new([1, 2]).size
p Two.new("ab").upcase
p Two.new(5).nope
