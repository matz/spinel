# spinel: rbs-seed-run
# `list.pluck(*keys) if list.is_a?(Relation)` on a `list` the signature
# leaves untyped: the class-side `PluckExt::Base.pluck(col)` took an arm
# (the walk of the receiver's values could not bound its Class values), and
# its parameter list has no gather, so the splat call was refused (#10939).
# The receiver's class set, narrowed by the is_a?, holds a Relation only.
require "set"

module PluckUnused
  def self.delete
    bind(owner)
  end
end

module PluckExt
  def self.pluck(list, *keys)
    return list.pluck(*keys) if list.is_a?(Relation)
  end

  class Base
    ATTRS = Hash.new

    def self.pluck(col)
      col
    end
  end

  class Relation
    def pluck(*cols)
      cols.map { |x| x.is_a?(Base) }
    end
  end
end

p PluckExt.pluck(PluckExt::Relation.new, :name)
