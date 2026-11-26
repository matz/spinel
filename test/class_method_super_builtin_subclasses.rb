# `super` in a class method named subclasses that no class method of the
# program answers reaches Class#subclasses: the receiving class's direct
# subclasses (activesupport's ReloadedClassesFiltering, prepended into
# Class, filters `super` that way, reached through DescendantsTracker).
module Tracker
  def descendants
    subs = subclasses
    subs + subs.flat_map(&:descendants)
  end
end

module Filtering
  def subclasses
    super.reject { |k| k.name == "Hidden" }
  end
end

class Class
  prepend Filtering
end

class Root
  extend Tracker
end
class Mid < Root; end
class Leaf < Mid; end
class Hidden < Root; end
class Other < Root; end

p Root.descendants.map(&:name).sort
p Mid.descendants.map(&:name)
p Leaf.descendants
# a class value Class#subclasses answers reaches its class methods
p Mid.subclasses.map { |k| [k.name, k.descendants] }
# and so does one a class value's subclasses answers
k = [Root, Mid][1]
p k.subclasses.map { |s| [s.name, s.descendants] }
# (so does a class the program makes at run time, and a builtin one)
anon = Class.new
j = [anon, Mid][ARGV.size]
p j.subclasses
p [Integer, Mid][ARGV.size].subclasses
