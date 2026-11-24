# A class-body `attribute :x` runs the program's own `attribute` class
# method where the program defines one (Module has no such method), so it
# declares no reader that would shadow a method the superclass defines.
class Base
  def self.attribute(name)
    (@fields ||= []) << name
  end

  def self.fields = @fields

  def page
    3
  end
end

class Child < Base
  attribute :page
  attribute :title
end

p Child.new.page
p Child.fields
p Child.new.respond_to?(:title)

module Declares
  def attributes(*names) = names.size
end

class Record
  extend Declares
  N = attributes :a, :b

  def a = :own
end

p Record::N
p Record.new.a
p Record.new.respond_to?(:a=)
