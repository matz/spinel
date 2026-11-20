# frozen_string_literal: true
require_relative 'catch_throw'

class CatchStepper
  def initialize(mode)
    @mode = mode
  end

  def step(tag)
    throw :cext_jump, [:thrown, tag] if @mode == :throw
    raise ArgumentError, "raised in step" if @mode == :raise
    [:returned, tag]
  end
end

class CatchCounter
  def each
    yield 1
    yield 2
    yield 3
    :finished
  ensure
    puts "ensure ran"
  end
end

class CatchHomeStepper
  def step(_tag)
    $saved_cext_return = proc { return "expired" }
    throw :cext_jump, +"escaped"
  end
end

p CextCatch.run(CatchStepper.new(:return))
p CextCatch.run(CatchStepper.new(:throw))
begin
  CextCatch.run(CatchStepper.new(:raise))
rescue ArgumentError => e
  p e.message
end
p(catch(:ruby_jump) do
  CextCatch.throw_value(5)
  :not_reached
end)
begin
  CextCatch.throw_value(1)
rescue UncaughtThrowError => e
  p [e.message, e.tag, e.value]
end
p CextCatch.first_two([10, 20, 30])
p CextCatch.first_two([10])
p CextCatch.first_two(CatchCounter.new)
p CextCatch.first_above([1, 5, 9], 4)
p CextCatch.first_above(CatchCounter.new, 1)
p CextCatch.first_above(CatchCounter.new, 7)
p CextCatch.run(CatchHomeStepper.new)
GC.start
begin
  $saved_cext_return.call
rescue LocalJumpError => error
  p error.reason
end
