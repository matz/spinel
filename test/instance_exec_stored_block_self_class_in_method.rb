# A block written in an instance method, stored, and run with instance_exec
# on another object: its `self.class` is that object's class, so a class
# method only that class answers is reached, not the method's own class.

class Config
  def initialize
    @timestamps = false
  end
  attr_reader :timestamps
end

class StateMachine
  def initialize
    @config = Config.new
  end
  attr_reader :config
end

class Base
  def initialize(sm)
    @state_machine = sm
    @callbacks = []
  end
  attr_reader :state_machine

  def after_all(&block)
    @callbacks << block
  end

  def setup_timestamps(aasm_name)
    after_all do
      if self.class.aasm(:"#{aasm_name}").state_machine.config.timestamps
        :x
      end
      :plain
    end
  end

  def run(record)
    @callbacks.map { |cb| record.instance_exec(&cb) }
  end
end

class Rec
  def aasm(name)
    self.class.aasm(name)
  end

  def self.aasm(*args, &block)
    @aasm ||= {}
    name = args[0] || :default
    @aasm[name] ||= Base.new(StateMachine.new)
    @aasm[name].instance_eval(&block) if block
    @aasm[name]
  end
end

b = Rec.aasm(:default)
b.setup_timestamps(:default)
p b.run(Rec.new)
