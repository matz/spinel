# A bare class_eval block in a class body runs on that class: its ivars are
# the class's own, the ones a `class << self` reader answers. The program
# also gives Kernel a class_eval that takes a block, and runs a stored proc
# through instance_exec somewhere, so block literals handed to a call named
# class_eval are otherwise taken for ones another self may run.
module Kernel
  def class_eval(*args, &block)
    block ? instance_exec(&block) : nil
  end
end

class Runner
  def initialize; @hooks = {}; end
  def on(&b); @hooks[:k] = b; end
  def run(obj); obj.instance_exec(&@hooks[:k]); end
end

class Settings
  class << self
    attr_reader :level, :names
  end

  class_eval do
    @level = 1
    @names = ["a"]
  end

  class_eval do
    @level += 1
    @names << "b"
  end
end

p Settings.level
p Settings.names
rn = Runner.new
rn.on { @hooks.size }
p rn.run(Runner.new)
