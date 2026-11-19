# Methods replaced by a later def. Dispatcher.instantiate's and Box#label's
# first versions are replaced before any call runs them, so no call keeps
# them and they have no C function; Tally#count's first version runs before
# the class is reopened, so both of its versions are kept. The symbol map
# (--emit-symbol-map, and the one --profile writes) lists a function for
# each version that has one, and none for a version that has none.
module Dispatcher
  def self.instantiate(name) = nil

  def self.dispatch(name) = instantiate(name)
end

module Dispatcher
  def self.instantiate(name)
    case name
    when "a" then "A"
    end
  end
end

class Box
  def label = "old"
  def show = label
end

class Box
  def label = "new"
end

class Tally
  def count = 1
end

t = Tally.new
first = t.count

class Tally
  def count = 2
end

p Dispatcher.dispatch("a")
puts Box.new.show
p [first, t.count]
