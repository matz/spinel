# Rack::Builder's shape: map stores its block in a Hash, and generate_map
# iterates that table as a parameter its callers pass, to_app with @map
# and use after `mapping, @map = @map, nil`; each block is run by a new
# Builder's instance_eval
class Builder
  def initialize(default_app = nil, &block)
    @use = []
    @map = nil
    @run = default_app
    instance_eval(&block) if block
  end

  def use(middleware)
    if @map
      mapping, @map = @map, nil
      @use << proc { |app| generate_map(app, mapping) }
    end
    @use << proc { |app| middleware.new(app) }
    nil
  end

  def run(app) = @run = app

  def map(path, &block)
    @map ||= {}
    @map[path] = block
    nil
  end

  def to_app
    app = @map ? generate_map(@run, @map) : @run
    fail "missing run or map statement" unless app
    @use.reverse.inject(app) { |a, e| e[a] }
  end

  private

  def generate_map(default_app, mapping)
    mapped = default_app ? { "/" => default_app } : {}
    mapping.each { |r, b| mapped[r] = self.class.new(default_app, &b).to_app }
    mapped
  end
end

class Wrap
  def initialize(app) = @app = app
  def inspect = "Wrap(#{@app.inspect})"
end

p Builder.new { map("/a") { run :a } }.to_app
p Builder.new { run :root; map("/a") { run :a }; map("/b") { map("/c") { run :c } } }.to_app
p Builder.new { map("/x") { run :x }; use Wrap; map("/y") { run :y } }.to_app
begin
  Builder.new { use Wrap }.to_app
rescue RuntimeError => e
  puts e.message
end

# a block's own `mapping` is another local: its write is no write of @map
class Shadow
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
  def to_app
    mapping = @map
    [@other].each { |mapping| mapping[:x] = 1 }
    [@run, mapping.map { |path, b| [path, Shadow.new(&b).to_app] }]
  end
end
p Shadow.new { map("/a") { run :a } }.to_app

# a same-named method of another class is not the parameter's def: its
# write to its own parameter is no write of this table
class Routes
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
  def build(mapping) = mapping.map { |path, b| [path, Routes.new(&b).to_app] }
end
class Other
  def build(mapping)
    mapping[:x] = 1
    mapping
  end
  def go = build({})
end
p Routes.new { map("/a") { run :a } }.to_app, Other.new.go
