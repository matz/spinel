# json's generic to_json takes at most one argument (the shipped json 2.18.0:
# `to_json(state = nil)`), whatever Method#arity says (-1): a super out of a
# Hash subclass's own to_json is the builtin's, so a second argument raises
# the same ArgumentError a plain Hash's call does, and no extra count is
# taken for an arity the builtin does not have.
require "json"

class Hs < Hash
  def to_json(*a) = "Hs:" + super
end

h = Hs.new
h["a"] = 1
puts h.to_json
puts h.to_json(nil)
begin
  puts h.to_json(nil, 2)
rescue ArgumentError => e
  puts "ArgumentError #{e.message}"
end
begin
  puts({ "a" => 1 }.to_json(nil, 2))
rescue ArgumentError => e
  puts "ArgumentError #{e.message}"
end
