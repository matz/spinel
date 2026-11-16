# --ext-class fixture: methods that take a block, which a host calls by
# name with a Proc of its own (methods_blk_host.c): a method that yields,
# a method with a block parameter, optional and rest parameters before the
# block, a method the named class inherits, a singleton method, a super
# that hands the block on, and methods with no block, which go on without
# the block of the host.
module ExtBlk
  class Base
    def initialize
      @items = [1, 2, 3]
    end

    # inherited: the named class answers it
    def each
      i = 0
      while i < @items.size
        yield @items[i]
        i += 1
      end
      self
    end

    def up(x)
      yield x
    end

    def plain
      "plain"
    end
  end

  class Rel < Base
    # the block goes to the method of the ancestor through super
    def up(x)
      super
    end

    # a super whose method takes no block
    def plain
      super + "!"
    end

    # the def inside has a block of its own
    def nest
      def nested_yield
        yield
      end
      "nest"
    end

    def map(&blk)
      out = []
      each { |x| out << blk.call(x) }
      out
    end

    def pick(n = 1, *more, &blk)
      blk.call(n + more.size)
    end

    # the block is optional: the method asks
    def maybe
      block_given? ? yield(7) : "no block"
    end

    def self.twice(x)
      yield(yield(x))
    end

    def size
      @items.size
    end
  end

  #: () -> untyped
  def self.make
    Rel.new
  end
end
