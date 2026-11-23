# spinel: needs-rbs-parser
# spinel: share
# Inline `# @rbs type name = T` aliases resolve by lexical scope to a fixpoint
# across declarations, matching --rbs: forward references, chained aliases,
# inner aliases shadowing outer ones (even when declared after use), absolute
# `::name` and relative `Inner::row` lookups, multi-line continuations,
# standalone or immediately above declarations, in methods, attributes and ivars,
# without colliding with a `# @rbs type: T` parameter annotation.

# @rbs type word_list = Array[String]
# @rbs type payload = Hash[Symbol, untyped]
# @rbs type scalar = Integer
#   | String
# @rbs type patches_later = Array[patch_t]
# @rbs type nilable_later = later_t | nil
# @rbs type patch_t = Array[untyped]
# @rbs type later_t = Integer
# @rbs type value_t = String
# @rbs type box_item = Item

class Item
  attr_reader :name #: String

  #: (String) -> void
  def initialize(name)
    @name = name
  end
end

module Outer
  # @rbs type patch_t = Array[String]
  # @rbs type patches = Array[patch_t]
  # @rbs type chosen_t = value_t
  # @rbs type value_t = Integer
  class Box
    # @rbs @words: word_list?
    # @rbs @note: value_t?

    attr_reader :label #: ::value_t?

    def initialize
      @words = nil
      @note = nil
      @label = nil
    end

    #: (word_list) -> word_list
    def words(xs)
      @words = xs
      xs
    end

    #: (payload) -> Integer
    def pay(h)
      h[:n].to_i
    end

    #: (patch_t) -> patches
    def patch(xs)
      [xs]
    end

    #: (::patch_t) -> patches_later
    def top_patch(xs)
      [xs]
    end

    #: (nilable_later) -> nilable_later
    def nilable(n)
      n
    end

    # @rbs type item_alias = box_item
    #: (chosen_t, item_alias) -> value_t
    def chosen(n, item)
      @note = n
      n + item.name.length
    end

    #: (scalar) -> Array[scalar]
    def mixed(v)
      [v]
    end

    #: (Inner::row) -> Inner::row
    def row(r)
      r
    end

    # @rbs type: ::value_t
    # @rbs return: ::value_t
    def kind(type)
      type.upcase
    end

    def trailing_alias #: value_t?
      @note
    end
  end

  module Inner
    # @rbs type row = Hash[String, Integer]
  end

  #: () -> Inner::row
  def self.row_of
    { "a" => 1 }
  end
end

b = Outer::Box.new
p b.words(["hi", "there"])
p b.pay({ n: 7 })
p b.patch(["p"])
p b.top_patch([1, "x"])
p b.nilable(nil)
p b.nilable(42)
p b.chosen(10, Item.new("abc"))
p b.mixed(5)
p b.mixed("s")
p b.row(Outer.row_of)
p b.kind("ok")
p b.trailing_alias
p b.label
