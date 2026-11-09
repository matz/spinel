# A to_json written on Object is behind the generator module json gives each
# core class (the json CRuby 4.0 ships): a String or an Integer reaches its own
# generator first, and the Object method is not called for either.
require 'json'

class Object
  def to_json(*a) = "O:" + super
end
puts "x".to_json
puts 1.to_json
