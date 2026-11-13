# A computed public_send on `Object.const_get(name).new(arg)`: the value is
# an instance of a class whose constructor takes one argument (or a
# builtin's), so the send's arms are those classes' methods. An unrelated
# class's method (two-argument constructor) is no arm, and its name raises
# NoMethodError on the instance, as CRuby does.

class Base
  def initialize(req) = @req = req
end

class AController < Base
  def show = "A#show #{@req}"
  def index = "A#index #{@req}"
end

class BController < Base
  def show = "B#show #{@req}"
end

class Pair
  def initialize(a, b) = (@a, @b = a, b)
  def pair_only = "#{@a}-#{@b}"
  def show = "Pair#show"
end

class Plain
  def plain_only = "plain"
end

Pair.new(1, 2).pair_only if ARGV.size > 99
Plain.new.plain_only if ARGV.size > 99

def run(cname, action, req)
  Object.const_get(cname).new(req).public_send(action.to_s.to_sym)
rescue NoMethodError => e
  "NoMethodError #{e.name}"
rescue ArgumentError => e
  "ArgumentError #{e.message}"
end

puts run("AController", "show", "r1")
puts run("AController", "index", "r2")
puts run("BController", "show", "r3")
puts run("BController", "index", "r4")
puts run("AController", "pair_only", "r5")
puts run("Pair", "show", "r6")
puts run("Plain", "plain_only", "r7")
