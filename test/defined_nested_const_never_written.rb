# `defined?(Lib::Feature)` where the program defines module Lib but nothing
# in it, or anywhere, defines a Feature: nil, whatever the load order. The
# defs the guard holds are then dead, and reflection by name elsewhere is
# not refused for them. (CRuby's did_you_mean prelude puts
# DidYouMean::Correctable around NameError subclasses' `def corrections`.)
module Suggest
  module Spell; end
end

class Lookup < StandardError
  if defined?(Suggest::Correctable) && defined?(Suggest::Spell)
    include Suggest::Correctable
    def corrections = ["a"]
  end
  def hint = respond_to?(:corrections)
end

class Other < StandardError
  if defined?(Suggest::Correctable)
    def corrections = ["b"]
  end
end

class Log
  def self.added?(m) = public_method_defined?(m)
  def self.call_it(n) = new.send(n)
  def m2 = 1
end

p Log.added?(:m2), Log.added?(:nope), Lookup.new.hint, Other.new.respond_to?(:corrections)
p defined?(Suggest::Correctable), defined?(Suggest::Spell), defined?(Suggest::Spell::Deep)
p Log.public_instance_methods(false), Log.call_it(:m2)
