# spinel: not-cruby -- each block below reaches instance_eval through a table Spinel cannot trace to its literal, so the run-time check raises NotImplementedError where CRuby runs the block
# A table read back by iteration is traced to its literal blocks only when
# the storing methods' own &block is all it holds, the iteration parameter
# is not written, and each caller passes a literal block. These keep the
# NotImplementedError, never a block run with another self.
def show(label)
  p yield
rescue NotImplementedError => e
  puts "#{label}: #{e.message}"
end

class Second
  def initialize(&block)
    @run = nil
    @maps = []
    instance_eval(&block) if block
  end
  def run(app) = @run = app
  def map(&block) = @maps << block
  def extra = @maps << proc { run 9 }
  def to_app = [@run, @maps.map { |b| Second.new(&b).to_app }]
end
show("second store") { Second.new { map { run 1 } }.to_app }

class Written
  def initialize(&block)
    @run = nil
    @maps = []
    instance_eval(&block) if block
  end
  def run(app) = @run = app
  def map(&block) = @maps << block
  def to_app = [@run, @maps.map { |b| b = b; Written.new(&b).to_app }]
end
show("written parameter") { Written.new { map { run 1 } }.to_app }

class Handed
  def initialize(&block)
    @run = nil
    @maps = []
    instance_eval(&block) if block
  end
  def run(app) = @run = app
  def map(&block) = @maps << block
  def to_app = [@run, @maps.map { |b| Handed.new(&b).to_app }]
end
inner = proc { 1 }
show("non-literal caller") { Handed.new { map(&inner) }.to_app }

class Swapped
  def initialize(&block)
    @run = nil
    @maps = []
    instance_eval(&block) if block
  end
  def run(app) = @run = app
  def map(replacement = nil, &block)
    block = replacement if replacement
    @maps << block
  end
  def to_app = [@run, @maps.map { |b| Swapped.new(&b).to_app }]
end
show("storing method writes its block") { Swapped.new { map { run 1 } }.to_app }

class AndWritten
  def initialize(&block)
    @run = nil
    @maps = []
    instance_eval(&block) if block
  end
  def run(app) = @run = app
  def map(&block) = @maps << block
  def to_app = [@run, @maps.map { |b| b &&= b; AndWritten.new(&b).to_app }]
end
show("parameter and-written") { AndWritten.new { map { run 1 } }.to_app }

class Replaced
  def initialize(&block)
    @run = nil
    @maps = []
    instance_eval(&block) if block
  end
  def run(app) = @run = app
  def map(&block) = @maps << block
  def to_app
    @maps.map! { |b| b }
    [@run, @maps.map { |b| Replaced.new(&b).to_app }]
  end
end
show("table rewritten by map!") { Replaced.new { map { run 1 } }.to_app }

class Reassigned
  def initialize(&block)
    @run = nil
    @maps = []
    instance_eval(&block) if block
  end
  def run(app) = @run = app
  def map(&block) = @maps << block
  def replace_all(blocks) = @maps = blocks
  def to_app = [@run, @maps.map { |b| Reassigned.new(&b).to_app }]
end
show("table ivar reassigned") { Reassigned.new { map { run 1 } }.to_app }

class Swapped0
  def initialize(&block)
    @run = nil
    @maps = []
    instance_eval(&block) if block
  end
  def run(app) = @run = app
  def map(&block) = @maps << block
  def swap(other) = @maps[0] &&= other
  def to_app = [@run, @maps.map { |b| Swapped0.new(&b).to_app }]
end
show("element replaced in place") { Swapped0.new { map { run 1 } }.to_app }
