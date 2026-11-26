# spinel: not-cruby -- each block below reaches instance_eval through a table passed in a way Spinel cannot trace to its literal, so the run-time check raises NotImplementedError where CRuby runs the block
# A table iterated as a parameter is traced only when each call passes the
# same table and the parameter is written nowhere, and a local holding it
# only when its method writes it once. These keep the NotImplementedError,
# never a block run with another self.
def show(label)
  p yield
rescue NotImplementedError => e
  puts "#{label}: #{e.message}"
end

class Written
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
  def to_app = [@run, build(@map)]
  def build(mapping)
    mapping = mapping
    mapping.map { |path, b| [path, Written.new(&b).to_app] }
  end
end
show("written parameter") { Written.new { map("/a") { run 1 } }.to_app }

class Other
  def initialize(&block)
    @run = nil
    @map = {}
    @other = {}
    instance_eval(&block) if block
  end
  def run(app) = @run = app
  def map(path, &block)
    @map[path] = block
    nil
  end
  def to_app = [@run, build(@map), build(@other)]
  def build(mapping) = mapping.map { |path, b| [path, Other.new(&b).to_app] }
end
show("another table passed") { Other.new { map("/a") { run 1 } }.to_app }

class Twice
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
    mapping = @map
    mapping = mapping.dup
    mapping.map { |path, b| [path, Twice.new(&b).to_app] }
  end
end
show("local written twice") { Twice.new { map("/a") { run 1 } }.to_app }

class Splat
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
    tables = [@map]
    [@run, build(*tables)]
  end
  def build(mapping) = mapping.map { |path, b| [path, Splat.new(&b).to_app] }
end
show("only a splat call") { Splat.new { map("/a") { run 1 } }.to_app }

class Parent
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
  def to_app = [@run, @map.map { |path, b| [path, self.class.new(&b).to_app] }]
end
class Child < Parent; end
show("self.class with a subclass") { Parent.new { map("/a") { run 1 } }.to_app }

class OrWritten
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
  def to_app = [@run, build(@map)]
  def build(mapping)
    mapping ||= {}
    mapping.map { |path, b| [path, OrWritten.new(&b).to_app] }
  end
end
show("parameter or-written") { OrWritten.new { map("/a") { run 1 } }.to_app }

class SplatValue
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
    tables = [{}, @map]
    ignored, mapping = *tables, @map
    [@run, mapping.map { |path, b| [path, SplatValue.new(&b).to_app] }]
  end
end
show("value list with a splat") { SplatValue.new { map("/a") { run 1 } }.to_app }

class Trailing
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
    mapping = @map
    first, *rest, mapping = 0, @map
    [@run, mapping.map { |path, b| [path, Trailing.new(&b).to_app] }]
  end
end
show("target after a splat") { Trailing.new { map("/a") { run 1 } }.to_app }
