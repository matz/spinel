# A call named like a stored-block reader, on a class whose own method of the
# name does not store its block, runs that block on its own self.

class Strategy
  def self.info(&block)
    @info = block if block
    @info
  end

  info { "#{name}!" }

  def name = "dev"

  def run = instance_eval(&self.class.info)
end

class Log
  def info
    yield
  end

  def message = "logged"

  def report
    self.info { message }
  end
end

p Strategy.new.run
p Log.new.report
