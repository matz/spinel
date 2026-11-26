# A block stored by one instance's method, kept in an Array or a Hash, and
# read back by iterating that table is handed to another instance whose
# initialize runs it with instance_eval (Rack::Builder#map's shape)
class B
  def initialize(name, &block)
    @name = name
    @run = nil
    @maps = []
    instance_eval(&block) if block
  end
  def run(app) = @run = app
  def map(&block) = @maps << block
  def to_app = [@name, @run, @maps.map { |b| B.new(:inner, &b).to_app }]
end
p B.new(:outer) { map { run 1 } }.to_app
p B.new(:two) { map { run 2 }; map { run 3 } }.to_app

class E
  def initialize(&block)
    @run = nil
    @apps = []
    instance_eval(&block) if block
  end
  def run(app) = @run = app
  def map(&block)
    @apps.push(block)
    nil
  end
  def apps
    out = []
    @apps.each { |b| out << E.new(&b).app }
    out
  end
  def app = @run
end
p E.new { map { run :a }; map { run :b } }.apps

class H
  def initialize(&block)
    @run = nil
    @map = {}
    instance_eval(&block) if block
  end
  def run(app) = @run = app
  def map(path, &block)
    @map[path] = block
    nil
  end
  def to_app
    mapped = {}
    @map.each { |path, b| mapped[path] = H.new(&b).to_app }
    [@run, mapped]
  end
end
p H.new { run :root; map("/x") { run :x }; map("/y") { run :y; map("/z") { run :z } } }.to_app
