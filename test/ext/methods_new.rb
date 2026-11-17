# --ext-class fixture: `new` of the classes, which a host calls by name on
# a class value (methods_new_host.c): an initialize of fixed parameters,
# one with optional and rest parameters, a class with no initialize, a
# class that inherits its initialize, a class with an initialize of its
# own under a class with another one, an initialize that yields, a class
# the program never makes, a class written by its path and a class under
# it, a class under a class that is named from the root (`::Top`), a
# `class << X` of another object in a class body, two classes of
# one name, a class opened again from inside another module, an
# initialize under a visibility call, and the classes whose
# table has no row of an initialize: a class under a class that is not of
# the program, a keyword parameter, the initialize of a module, a constant
# of the class's own name, a private `new`, and a `new` of the class
# (`def self.new`, `class << self`), also for a class under it.
module ExtNew
  class Point
    attr_reader :x, :y

    def initialize(x, y)
      @x = x
      @y = y
    end

    def sum = @x + @y
  end

  class Opt
    attr_reader :n

    def initialize(a, b = 2, *more)
      @n = a + b + more.size
    end
  end

  class Bare
    def hello = "bare"
  end

  # the initialize of Opt: the rows of Opt
  class Kid < Opt
    def kid = "kid #{@n}"
  end

  # an initialize of its own: its row, and none of Point
  class Own < Point
    attr_reader :s

    def initialize(s)
      super(1, 2)
      @s = s
    end
  end

  class Blk
    attr_reader :v

    def initialize(k = 1)
      @v = yield k
    end
  end

  # the program makes no Lazy: only the host does
  class Lazy
    attr_reader :name

    def initialize(name)
      @name = name
    end
  end

  # a class opened again by its path, from inside another module: the row
  # of its initialize, and none for the second definition
  module Home
    class Room
      attr_reader :n

      def initialize(n)
        @n = n
      end
    end
  end

  module Guest
    class Home::Room
      def more = @n + 1
    end
  end

  # an initialize under a visibility call
  class Priv
    attr_reader :x

    private def initialize(x)
      @x = x
    end
  end

  # a class under a class that is not of the program: no row, also with
  # an initialize of its own
  class Oops < StandardError
    attr_reader :code

    def initialize(msg, code)
      super(msg)
      @code = code
    end
  end

  # a constant of the class's own name inside the class: the forwarder
  # would make an object of that class, so the table has no row
  class Same
    attr_reader :x

    class Same
      def initialize(a, b)
        @ab = a + b
      end
    end

    def initialize(x)
      @x = x
    end
  end

  # a keyword parameter: no row
  class Kw
    def initialize(a:, b: 1)
      @v = a + b
    end
  end

  # the initialize of a module: no row
  module Init
    def initialize(v)
      @v = v
    end
  end

  class Mixed
    include Init
  end

  # a second Point, in a namespace of its own, and a class under it: the
  # rows of this Point, not of the Point above
  module Inner
    class Point
      attr_reader :z

      def initialize(z)
        @z = z
      end
    end

    class Kid < Point
    end
  end

  class Single
    private_class_method :new

    def self.instance = @instance ||= new

    def tag = "single"
  end

  class Custom
    attr_reader :v

    def self.new(v = 3)
      o = allocate
      o.keep(v)
      o
    end

    def keep(v) = @v = v
  end

  # an initialize of its own under a class with a self.new: the rows of
  # that self.new, which runs
  class Under < Custom
    def initialize(a, b, c)
      @v = a + b + c
    end
  end

  # a new in `class << self`, beside an initialize of another count: the
  # row of that new
  class Meta
    attr_reader :v

    class << self
      def new(a, b)
        o = allocate
        o.keep(a + b)
        o
      end
    end

    def initialize(x)
      @v = x
    end

    def keep(v) = @v = v
  end

  # a `class << X` of another object inside a class body: its `def new`
  # is no `new` of the class, which has the row of its initialize
  class Tool
    def self.label = "tool"
  end

  class Shy
    attr_reader :x

    class << Tool
      def new(a, b) = a + b
    end

    def initialize(x)
      @x = x
    end
  end

  # one object of each class, for the class values of the host
  #: () -> Array[untyped]
  def self.sample
    [Point.new(1, 2), Opt.new(1), Bare.new, Kid.new(1), Own.new("s"), Blk.new { |k| k }, Single.instance, Custom.new,
     Deep.new(1, 2), Inner::Point.new(1), Inner::Kid.new(1), Kw.new(a: 1), Mixed.new(1), Priv.new(1), Same.new(1),
     Under.new, Meta.new(1, 2), Home::Room.new(1), Far.new(1, 2), Shy.new(1), FromRoot.new(1)]
  end

  #: () -> singleton(Lazy)
  def self.lazy = Lazy

  #: () -> singleton(Oops)
  def self.oops = Oops
end

# a class written by its path, with the initialize of the class above it
class ExtNew::Deep < ExtNew::Point
  def deep = "deep #{sum}"
end

# a class under that class, written in the module: the same initialize
module ExtNew
  class Far < Deep
    def far = "far #{sum}"
  end
end

# a class at the root, and a class of the module that names it from the
# root. The module has a class of the same name, so the reference keeps
# its `::`
class ExtNewTop
  attr_reader :x

  def initialize(x)
    @x = x
  end
end

module ExtNew
  class ExtNewTop
  end

  class FromRoot < ::ExtNewTop
    def twice = @x * 2
  end
end
