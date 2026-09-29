module Consts
  def self.extended(klass)
    klass.instance_variable_set(:@kind, nil)
  end

  def kind(k = nil)
    return @kind if k.nil?
    @kind = k
  end

  def constant(c, name: c, fallback: nil)
    fn = ["calc", kind, c.to_s.downcase].compact.join("_")
    const_set(name, public_send(fn))
  end

  def noted(name, target)
    module_eval <<-RUBY, __FILE__, __LINE__ + 1
    def self.#{name}(*a) = #{target}(*a) # forwards
    RUBY
  end

  def wrapper(name, target, args)
    module_eval <<-RUBY, __FILE__, __LINE__ + 1
    def self.#{name}(*a)
      r = #{target}(*a)
      [#{args.inspect}, r]
    end
    RUBY
  end
end

class Calc
  extend Consts
  def self.calc_box_size = 32
  def self.calc_box_nonce = 24
  def self.add(a, b) = a + b
  kind :box
  constant :SIZE
  constant :NONCE, name: :NONCE_BYTES
  wrapper :plus, :add,
          %i[int int]
  def self.info = [SIZE, NONCE_BYTES]
end
p Calc.info
p Calc.plus(2, 3)
p Calc.kind

# a trailing comment in a template line cuts only itself
class Calc
  noted :plus2, :add
end
p Calc.plus2(4, 5)
# a class-body write of the macros' state is read as written
class Direct
  extend Consts
  def self.calc_box_size = 64
  @kind = :box
  constant :SIZE
end
p Direct::SIZE
# branches that agree leave the state known
class Agree
  extend Consts
  def self.calc_box_size = 8
  if RUBY_VERSION >= "3"
    kind :box
  else
    kind :box
  end
  constant :SIZE
end
p Agree::SIZE
# a macro reads only what the body wrote before the call
class Late
  extend Consts
  def self.calc_size = 5
  def self.calc_box_size = 6
  constant :SIZE
  @kind = :box
end
p Late::SIZE
# the module extended last answers first; of `extend A, B`, A
module Override
  def constant(c, name: c)
    const_set(name, public_send(["calc", "over", c.to_s.downcase].join("_")))
  end
end
module Other
  def constant(c, name: c) = const_set(name, 0)
end
class Order
  extend Consts
  extend Override, Other
  def self.calc_over_size = 11
  constant :SIZE
end
p Order::SIZE
# a class-body instance_variable_set is read as written
class IvSet
  extend Consts
  def self.calc_box_size = 12
  instance_variable_set(:@kind, :box)
  constant :SIZE
end
p IvSet::SIZE
# a call sees only the extends made before it
class Staged
  extend Consts
  def self.calc_x = 13
  def self.calc_over_y = 14
  constant :X
  extend Override
  constant :Y
end
p Staged::X, Staged::Y
# an extended hook's writes to the class are followed
module HookConsts
  def self.extended(base) = base.instance_variable_set(:@kind, :box)
  def constant(c) = const_set(c, public_send(["calc", @kind, c.to_s.downcase].join("_")))
end
class Hooked
  extend HookConsts
  def self.calc_box_size = 15
  constant :SIZE
end
p Hooked::SIZE
# a helper a later extend overrides answers the nested call
module LayerBase
  def prefix = "base"
  def layered(n) = const_set(n.to_s.upcase, public_send("#{prefix}_#{n}"))
end
module LayerOver
  def prefix = "ovr"
end
class Layered
  extend LayerBase
  extend LayerOver
  def self.base_x = 0
  def self.ovr_x = 16
  layered :x
end
p Layered::X
# a guard over two lines keeps the program's line numbers
class Guarded
  extend Consts
  def self.calc_g = 18
  constant :G if RUBY_VERSION >= "3" &&
                 RUBY_VERSION < "9"
end
p Guarded::G, __LINE__
# a macro in a class-body ivar write is looked up in the class's extends
module PickBase
  def pick(k) = :base
end
module PickOver
  def pick(k) = k
end
class Picked
  extend Consts
  extend PickBase
  extend PickOver
  def self.calc_base_size = 0
  def self.calc_box_size = 19
  @kind = pick(:box)
  constant :SIZE
end
p Picked::SIZE
# a define_singleton_method lambda reads the state when it is called
module LamConsts
  def lkind(k)
    @lkind = k
  end
  def lreader(n) = define_singleton_method(n, -> { @lkind })
end
class Lam
  extend LamConsts
  lkind :box
  lreader :k
  lkind :other
end
p Lam.k
# a block parameter named like a macro local is the block's own
module Shadow
  def listed(fn)
    (1..2).each { |fn| puts fn }
    const_set(:LISTED, fn)
  end
end
class Shadowed
  extend Shadow
  listed :z
end
p Shadowed::LISTED
# instance_eval of a string defines on the singleton class, not in the body
module Classy
  def classy(n) = instance_eval("def #{n}; 1; end")
end
class ClassyBox
  extend Classy
  classy :k
end
begin
  p ClassyBox.new.k
rescue NoMethodError
  puts "NoMethodError"
end
