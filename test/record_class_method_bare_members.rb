# A receiverless `members` in a class method of a Struct or Data class is the
# class's own method: self is the class there, and its singleton answers
# ahead of Object's.

def members = :top_level   # private on Object; a record class answers first

Entry = Data.define(:code, :label)
class Entry
  def self.list = members
  def self.same_keys?(fields) = fields.keys.sort == members.sort
  def self.count = members.size
  def self.known(names) = names.select { |n| members.include?(n) }
  def self.strings = members.map { |m| m.to_s }
  def self.nested = [[:code], [:other]].map { |row| row.all? { |n| members.include?(n) } }
  def self.thunk = proc { members }.call
  def self.arrow = -> { members.first }.call
  class << self
    def via_singleton = members
  end
end

p Entry.list
p Entry.same_keys?({ label: 1, code: 2 })
p Entry.same_keys?({ label: 1 })
p Entry.count
p Entry.known(%i[label missing code])
p Entry.strings
p Entry.nested
p Entry.thunk
p Entry.arrow
p Entry.via_singleton

# the block that builds the class, a Struct, a subclass, a namespace
Pair = Struct.new(:left, :right) do
  def self.names = members
  def self.flipped = members.reverse
  def names = members        # an instance's own, as before
end
class Wide < Pair
  def self.inherited_names = members
end
class Point < Struct.new(:x, :y, keyword_init: true)
  def self.axes = members
end
class Version < Data.define(:major, :minor)
  def self.parts = members
end
module Shapes
  Circle = Data.define(:radius) do
    def self.fields = members
  end
end

p Pair.names
p Pair.flipped
p Pair.new(1, 2).names
p Wide.inherited_names
p Wide.names
p Point.axes
p Version.parts
p Shapes::Circle.fields

# a program's own class method of the name wins, where it is defined
Custom = Struct.new(:a) do
  def self.members = [:custom]
  def self.read = members
end
class Child < Custom
  def self.read_child = members
end
p Custom.read
p Child.read_child

# no other class has the method: Object's answers
class Plain
  def self.read = members
end
p Plain.read
