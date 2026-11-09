# A super out of a to_json that a program writes on a core class reaches the
# generator method json gives that class. The tables the names layer answers
# from carry json's generator modules, one per core class, as the json CRuby 4.0
# ships has them.
require 'json'

# A Hash subclass whose to_json supers (the module json puts after Hash).
class Hs < Hash
  def to_json(*a) = "Hs:" + super
end
h = Hs.new
h["a"] = 1
puts h.to_json

# A (...) forwarder prepended to Array.
module W
  def to_json(...) = "W:" + super
end
class Array
  prepend W
end
puts [1, 2].to_json
