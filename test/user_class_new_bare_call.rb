# Bare `new` in a class method reaches the class's own `self.new` when it
# defines one, as does a `new(...)` forward, and a subclass inheriting the
# forwarder builds itself through it; the method called on the result then
# dispatches on the runtime class (#5405). A `self.new` whose every answer
# is a `super` builds the class it is called on, whose own methods the
# result then has, through an explicit `new` and a bare one alike.
class Base
  class << self
    def call(...)
      new(...).run
    end
    def new(*a, **k)
      super
    end
  end
  def run = :base
end
class Mid < Base
  def run = :mid
end
p Base.call
p Mid.call

class Base2
  @@made = 0
  class << self
    def call(...) = new(...).run
    def new(*a, **k)
      @@made += 1
      super
    end
    def made = @@made
  end
  def initialize(x = 1, tag: :t) = (@x = x; @tag = tag)
  def run = [:base, @x, @tag]
end
class Mid2 < Base2
  def run = [:mid, @x, @tag]
end
p Base2.call, Mid2.call(5), Mid2.call(6, tag: :z)
p Base2.made, Mid2.made

class Base3
  class << self
    def call(*a)
      new(*a).run
    end
    def new(*a, **k)
      super
    end
  end
  def run = :base
end
class Mid3 < Base3
  def run = :mid
end
p Base3.call
p Mid3.call

class Shape
  def self.new(*a)
    return super(1) if a.empty?
    super
  end
  def self.make(n) = new(n)
  def self.plain = new
  def initialize(n) = @n = n
  def n = @n
end
class Square < Shape
  def side = @n * 2
end
class Cube < Square
  def vol = @n * 3
end
p Square.new(4).side, Cube.new(2).vol, Cube.new.side
p Square.make(5).side, Cube.make(6).vol, Cube.plain.vol
s = Square.make(7)
p s.class, s.n, [Shape.new(1), Square.new(2), Cube.new(3)].map(&:class)

class Reg
  def self.new(*a)
    o = super
    o
  end
  def self.create(n) = new(n)
  def initialize(x) = @x = x
end
class SubReg < Reg
  def twice = @x * 2
end
p SubReg.create(3).twice, Reg.create(1).class

# A `self.new` with another answer beside its `super` answers whichever it
# reaches, the instance of the class it is called on among them.
class Maybe
  def self.new(*a) = a.empty? ? :none : super
  def initialize(x) = @x = x
  def x = @x
end
class MaybeKid < Maybe
  def kid = @x * 3
end
p Maybe.new, MaybeKid.new(2).kid, MaybeKid.new(2).class

# A subclass no code names is not built by the `super`'s arms.
class Lone
  def self.new(x) = super
  def initialize(x) = @x = x
  def x = @x
end
class Unnamed < Lone
  def initialize(x) = @x = x * 2
end
p Lone.new(5).x

# A `self.new` that answers a local holds the receiver's instance only while
# every binding of the local is a read or a `super`: any other binding of it
# (a multiple assignment target, a `for` index, a write in a block or lambda,
# `rescue => o`, an operator write) may answer something
# else, and the call answers whichever it is.
class MasgnBase
  def self.new(x)
    o = super; o, z = :masgn, 1; o
  end
  def initialize(x) = (@x = x)
end
class MasgnKid < MasgnBase
  def kid = :kid
end
r = MasgnKid.new(1)
p r.class
p((r.kid rescue :nm))
p MasgnBase.new(2).class
class ForIdxBase
  def self.new(x)
    o = super; for o in [:forv]; end; o
  end
  def initialize(x) = (@x = x)
end
class ForIdxKid < ForIdxBase
  def kid = :kid
end
r = ForIdxKid.new(1)
p r.class
p((r.kid rescue :nm))
p ForIdxBase.new(2).class
class BlockWriteBase
  def self.new(x)
    o = super; [1].each { o = :blk }; o
  end
  def initialize(x) = (@x = x)
end
class BlockWriteKid < BlockWriteBase
  def kid = :kid
end
r = BlockWriteKid.new(1)
p r.class
p((r.kid rescue :nm))
p BlockWriteBase.new(2).class
class LambdaWriteBase
  def self.new(x)
    o = super; f = -> { o = :lam }; f.call; o
  end
  def initialize(x) = (@x = x)
end
class LambdaWriteKid < LambdaWriteBase
  def kid = :kid
end
r = LambdaWriteKid.new(1)
p r.class
p((r.kid rescue :nm))
p LambdaWriteBase.new(2).class
class RescueVarBase
  def self.new(x)
    o = super; begin; raise "x"; rescue => o; end; o
  end
  def initialize(x) = (@x = x)
end
class RescueVarKid < RescueVarBase
  def kid = :kid
end
r = RescueVarKid.new(1)
p r.class
p((r.kid rescue :nm))
p RescueVarBase.new(2).class
class OpWriteBase
  def self.new(x)
    o = super; o ||= :never; o
  end
  def initialize(x) = (@x = x)
end
class OpWriteKid < OpWriteBase
  def kid = :kid
end
r = OpWriteKid.new(1)
p r.class
p((r.kid rescue :nm))
p OpWriteBase.new(2).class
class BlockLocalBase
  def self.new(x)
    o = super; [1].each { |x; o| o = :shadow }; o
  end
  def initialize(x) = (@x = x)
end
class BlockLocalKid < BlockLocalBase
  def kid = :kid
end
r = BlockLocalKid.new(1)
p r.class
p((r.kid rescue :nm))
p BlockLocalBase.new(2).class

class Reading
  def self.new(x)
    o = super
    [1].each { o.touch }
    f = -> { o.touch }
    f.call
    o
  end
  def initialize(x) = (@x = x)
  def touch = @x
end
class ReadingKid < Reading
  def kid = :kid
end
r = ReadingKid.new(1)
p r.class
p((r.kid rescue :nm))

# A descendant's own `self.new` that reaches the inherited one through
# `super` answers the class it is called on as that one does, whatever
# shape the `super` is in, and a method calling `new` on either gets the
# class it was called on.
class PlainBase
  def self.new(*a) = super
  def self.create(*a) = new(*a)
  def initialize(x = 0) = (@x = x)
  def who = "B#{@x}"
end
class PlainKid < PlainBase
  def self.new(*a)
    o = super
    o
  end
  def who = "K#{@x}"
end
class PlainGrand < PlainKid
  def self.new(*a)
    super
  end
  def who = "G#{@x}"
end
p PlainKid.create(1).who
p PlainGrand.create(2).who
p PlainGrand.new(3).who
p PlainBase.create(4).who
class ArgsBase
  def self.new(*a) = super
  def self.create(*a) = new(*a)
  def initialize(x = 0) = (@x = x)
  def who = "B#{@x}"
end
class ArgsKid < ArgsBase
  def self.new(*a)
    o = super
    o
  end
  def who = "K#{@x}"
end
class ArgsGrand < ArgsKid
  def self.new(*a)
    super(*a)
  end
  def who = "G#{@x}"
end
p ArgsKid.create(1).who
p ArgsGrand.create(2).who
p ArgsGrand.new(3).who
p ArgsBase.create(4).who
class LocalBase
  def self.new(*a) = super
  def self.create(*a) = new(*a)
  def initialize(x = 0) = (@x = x)
  def who = "B#{@x}"
end
class LocalKid < LocalBase
  def self.new(*a)
    o = super
    o
  end
  def who = "K#{@x}"
end
class LocalGrand < LocalKid
  def self.new(*a)
    o = super; o
  end
  def who = "G#{@x}"
end
p LocalKid.create(1).who
p LocalGrand.create(2).who
p LocalGrand.new(3).who
p LocalBase.create(4).who
class TapBase
  def self.new(*a) = super
  def self.create(*a) = new(*a)
  def initialize(x = 0) = (@x = x)
  def who = "B#{@x}"
end
class TapKid < TapBase
  def self.new(*a)
    o = super
    o
  end
  def who = "K#{@x}"
end
class TapGrand < TapKid
  def self.new(*a)
    super.tap { }
  end
  def who = "G#{@x}"
end
p TapKid.create(1).who
p TapGrand.create(2).who
p TapGrand.new(3).who
p TapBase.create(4).who
class GuardBase
  def self.new(*a) = super
  def self.create(*a) = new(*a)
  def initialize(x = 0) = (@x = x)
  def who = "B#{@x}"
end
class GuardKid < GuardBase
  def self.new(*a)
    o = super
    o
  end
  def who = "K#{@x}"
end
class GuardGrand < GuardKid
  def self.new(*a)
    GuardBase.new(*a) && super
  end
  def who = "G#{@x}"
end
p GuardKid.create(1).who
p GuardGrand.create(2).who
p GuardGrand.new(3).who
p GuardBase.create(4).who
