# A constant that aliases a class elsewhere (`NS::Key = NS::Impl::Key`) binds
# NS::Key only: the class statement of NS::Impl::Key, under its own name, is
# still the class's body, and its macro state is followed.
module Consts
  def kind(k = nil)
    return @kind if k.nil?
    @kind = k
  end
  def constant(c)
    const_set(c, public_send(["calc", kind, c.to_s.downcase].join("_")))
  end
end
module NS
  module Impl
    class Key
      extend Consts
      def self.calc_a_size = 1
      def self.calc_b_size = 2
      kind :a
      constant :SIZE
    end
  end
  Key = Impl::Key
end
p NS::Key::SIZE
p NS::Impl::Key::SIZE
