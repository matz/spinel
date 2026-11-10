# A module nothing includes has no object its instance methods run on: a
# class calling a method of the same name, or including the module the
# unincluded one includes, does not compile them with the module as self.
module Location
  def stored(key)
    navigational? ? "nav #{key}" : "plain #{key}"
  end
end

module Helpers
  include Location
end

module Lone
  def stored(key)
    navigational? ? "lone #{key}" : "lone plain #{key}"
  end
end

class Failure
  include Location

  def run
    stored("x")
  end

  def navigational?
    true
  end
  protected :navigational?
end

class Other
  def stored(key) = "other #{key}"
end

puts Failure.new.run
puts Other.new.stored("y")
