# spinel: share
class Collector
  attr_reader :names

  def initialize
    @names = []
  end

  def add(name)
    @names << name
  end
end

class Base
  def self.added
    @added ||= []
  end

  def self.add(name)
    added << name
  end

  def self.collect(&block)
    c = Collector.new
    c.instance_eval(&block)
    c.names
  end
end

class A < Base
  add :class_level
  COLLECTED = collect do
    add :in_block
  end
end

p A.added
p A::COLLECTED
