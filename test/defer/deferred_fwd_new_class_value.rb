# Compiled with --defer-refusals: `...` forwarded to `new` on a class value,
# where an initialize takes keywords and a Struct could be constructed, is
# refused where the call is emitted, so a method holding it raises
# NotImplementedError when it runs and a program that never runs it
# (activerecord's Relation.create, `relation_class_for(model).new(model,
# ...)`) builds. Without the flag it is refused.
# spinel: defer-refusals: 2:start true after
Point = Struct.new(:x, :y)
class Rel
  def initialize(model, table: nil) = (@model = model; @table = table)
  def show = "#{@model}:#{@table}"
end
class Plain
  def initialize(a) = @a = a
end
Plain.new(1)
module Maker
  def self.klass_for(m) = m == :point ? Point : Rel
  def self.create(model, ...) = klass_for(model).new(model, ...)
end

puts "start"
begin
  p Maker.create(:rel, table: "t").show
rescue NotImplementedError => e
  puts e.message.include?("forwarded to `new` on a class value")
end
puts "after"
proc { "a" }.ruby2_keywords
puts "not reached"
