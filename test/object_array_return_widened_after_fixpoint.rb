# A method's object-array return that the fixpoint narrowed, and that a
# receiver widened only after the fixpoint takes back, is not left typed
# while its body reads the poly array.
#
# @model holds Class values. During the fixpoint it is a Class, so the call
# `@model.load(2)` does not stop Item.load from returning sp_PtrArray. After
# the fixpoint, `@model.new(attrs)`, whose receiver may be any class, binds
# Query#initialize's `model` and widens @model to poly. The run of
# narrow_object_arrays after the fixpoint then kills Item.load's return slot.
# Its pin used to be kept for one more round, but no round follows that run:
# a later return pass (the String-storage one, which the boxed String `s`
# starts) made the return sp_PtrArray * while `items` read the poly array,
# and the compile was refused.

class Item
  def name
    "item"
  end

  def self.load(n)
    items = []
    n.times { items << Item.new }
    items
  end
end

class Query
  def initialize(model)
    @model = model
  end

  def attrs
    @attrs
  end

  def to_a
    @model.load(2)
  end

  def build
    @model.new(attrs)
  end
end

Item.load(2).each { |x| puts x.name }
v = ARGV.empty? ? Item.new : 1
p Query.new(v.class).to_a.size
s = ARGV.empty? ? +"a" : 1
p s
