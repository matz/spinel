# A top-level def made after a module's include into Object, with the name
# of one of the module's methods: a call that runs before the def reaches
# the module's method, one after it the def, on main and on any object.
module Greets
  def greet = "module"
  def name_of = "m:" + greet
end
include Greets
class Own
  def greet = "own"
end
class K; end
p greet
p name_of
p Own.new.greet
p Own.new.name_of
p K.new.name_of
def greet = "main"
p greet
p name_of
p Own.new.name_of
p K.new.name_of

# two top-level versions behind one module's
module Counts
  def count = 0
  def twice = count * 2 + 1
end
include Counts
p count, twice
def count = 5
p count, twice
def count = 7
p count, twice
