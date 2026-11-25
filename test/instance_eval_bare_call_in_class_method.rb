# A bare call in a block that a class method hands to instance_eval runs
# on the block's receiver, even where the class has a class method of the
# same name.
class Collector
  attr_reader :names
  def initialize = @names = []
  def add(name) = @names << name
end

class Base
  def self.added = (@added ||= [])
  def self.add(name) = added << name
  def self.collect(&block)
    c = Collector.new
    c.instance_eval(&block)
    c.names
  end
  def self.collect_exec(&block) = Collector.new.tap { |c| c.instance_exec(&block) }.names
end

class D < Base
  def self.go = collect { add :in_class_method }
  def self.go_exec = collect_exec { add :exec_in_class_method }
end
p D.go, D.go_exec, D.added
