# A name an attr form makes, and another form too, still may run a method
# that defines: Item's define_method(:name) replaces its reader, and runs
# Base.define_singleton_method, so the macro after `label` waits for it,
# refused (a define_method of the name is no reader).
module Macros
  def attribute(name) = define_method("#{name}_#{sfx}") { name.to_s }
end
class Item
  attr_reader :name
  define_method(:name) { Base.define_singleton_method(:sfx) { "b" }; "i" }
end
def label(x) = x.name
class Base
  extend Macros
  def self.sfx = "a"
  label(Item.new)
  attribute :title
end
p Base.instance_methods(false).sort
